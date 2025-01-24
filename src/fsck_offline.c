/**
 * bookmarkfs/src/fsck_offline.c
 * ----
 *
 * Copyright (C) 2024  CismonX <admin@cismon.net>
 *
 * This file is part of BookmarkFS.
 *
 * BookmarkFS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BookmarkFS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BookmarkFS.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fsck_ops.h"
#include "ioctl.h"
#include "version.h"
#include "xstd.h"

#define FSCK_ALL_DONE  ( 1u << 24 )

#define BACKEND_CALL(ctx, name, ...)  \
    (ctx)->backend->name((ctx)->backend_ctx, __VA_ARGS__)

struct fsck_ctx {
    struct bookmarkfs_backend const *backend;
    void                            *backend_ctx;

    struct fsck_dir *dir_stack;
    size_t           dir_stack_size;
    size_t           dir_stack_top;

    char     *path;
    uint32_t  flags;
};

struct fsck_data {
    struct bookmarkfs_fsck_handler_entry *entry;

    enum bookmarkfs_fsck_result result;
};

struct fsck_dir {
    uint64_t  id;
    void     *cookie;
    off_t     off;
};
#define FSCK_DIR_INIT(id_)  (struct fsck_dir) { .id = (id_), .off = -1 }

// Forward declaration start
static int  do_fsck    (struct fsck_ctx *, struct fsck_dir *,
                        struct bookmarkfs_fsck_data const *,
                        struct bookmarkfs_fsck_handler_entry *);
static int  do_fsck_cb (void *, int, uint64_t, uint64_t, char const *);
static int  do_list    (struct fsck_ctx const *, struct fsck_dir *,
                        uint64_t *);
static int  do_list_cb (void *, struct bookmarkfs_bookmark_entry const *);
static void free_dir   (struct fsck_ctx const *, struct fsck_dir const *);
static int  init_top   (struct fsck_ctx const *, uint64_t, char *, uint64_t *);
static int  reset_top  (struct fsck_ctx *);
// Forward declaration end

static int
do_fsck (
    struct fsck_ctx                      *ctx,
    struct fsck_dir                      *dir,
    struct bookmarkfs_fsck_data const    *apply_data,
    struct bookmarkfs_fsck_handler_entry *entry_buf
) {
    struct fsck_data data;
    bookmarkfs_bookmark_fsck_cb *callback = NULL;
    // `entry_buf == NULL` means rewind
    if (entry_buf != NULL) {
        entry_buf->parent_id = dir->id;
        data.entry = entry_buf;
        callback = do_fsck_cb;
    }
    data.result = BOOKMARKFS_FSCK_RESULT_END;

    uint32_t flags = ctx->flags & BOOKMARKFS_BOOKMARK_TYPE_MASK;
    int status = BACKEND_CALL(ctx, bookmark_fsck, dir->id, apply_data,
            flags, callback, &data, &dir->cookie);
    if (status < 0) {
        log_printf("bookmark_fsck(): %s", xstrerror(-status));
        return status;
    }
    return data.result;
}

static int
do_fsck_cb (
    void       *user_data,
    int         result,
    uint64_t    id,
    uint64_t    extra,
    char const *name
) {
    struct fsck_data *data = user_data;

    struct bookmarkfs_fsck_data *entry_data = &data->entry->data;
    entry_data->id    = id;
    entry_data->extra = extra;
    if (name != entry_data->name) {
        strncpy(entry_data->name, name, sizeof(entry_data->name));
    }
    data->result = result;
    return 1;
}

static int
do_list (
    struct fsck_ctx const *ctx,
    struct fsck_dir       *dir,
    uint64_t              *subdir_id_ptr
) {
    struct bookmarkfs_bookmark_entry entry = {
        .next    = dir->off,
        .stat.id = UINT64_MAX,
    };
    int status = BACKEND_CALL(ctx, bookmark_list, dir->id, dir->off, 0,
            do_list_cb, &entry, &dir->cookie);
    if (status < 0) {
        return status;
    }
    dir->off = entry.next;
    *subdir_id_ptr = entry.stat.id;
    return 0;
}

static int
do_list_cb (
    void                                   *user_data,
    struct bookmarkfs_bookmark_entry const *entry
) {
    struct bookmarkfs_bookmark_entry *result = user_data;

    if (entry->stat.value_len >= 0) {
        return 0;
    }
    result->next    = entry->next;
    result->stat.id = entry->stat.id;
    return 1;
}

static void
free_dir (
    struct fsck_ctx const *ctx,
    struct fsck_dir const *dir
) {
    BACKEND_CALL(ctx, object_free,
            dir->cookie, BOOKMARKFS_OBJECT_TYPE_BLCOOKIE);
}

static int
init_top (
    struct fsck_ctx const *ctx,
    uint64_t               id,
    char                  *path,
    uint64_t              *result_id_ptr
) {
    for (char *sep; path != NULL; path = sep) {
        sep = strchr(path, '/');
        if (sep != NULL) {
            *(sep++) = '\0';
        }
        if (path[0] == '\0') {
            continue;
        }

        struct bookmarkfs_bookmark_stat stat_buf;
        int status = BACKEND_CALL(ctx, bookmark_lookup, id, path, 0,
                &stat_buf);
        if (status < 0) {
          fail:
            log_printf("bookmark_lookup(): %s: %s", path, xstrerror(-status));
            return status;
        }
        if (stat_buf.value_len >= 0) {
            status = -ENOTDIR;
            goto fail;
        }
        id = stat_buf.id;
    }

    *result_id_ptr = id;
    return 0;
}

static int
reset_top (
    struct fsck_ctx *ctx
) {
    ctx->flags &= ~FSCK_ALL_DONE;

    struct fsck_dir *dir = &ctx->dir_stack[0];
    if (dir->off < 0) {
        return do_fsck(ctx, dir, NULL, NULL);
    } else {
        dir->off = -1;
        return 0;
    }
}

static int
fsck_apply (
    void                                 *fsck_ctx,
    struct bookmarkfs_fsck_handler_entry *entry
) {
    struct fsck_ctx *ctx = fsck_ctx;

    struct fsck_dir *dir = ctx->dir_stack + ctx->dir_stack_top;
    return do_fsck(ctx, dir, &entry->data, entry);
}

static int
fsck_control (
    void *fsck_ctx,
    int   control
) {
    struct fsck_ctx *ctx = fsck_ctx;

    struct fsck_dir *dir = ctx->dir_stack + ctx->dir_stack_top;
    switch (control) {
      case BOOKMARKFS_FSCK_SKIP:
        if (dir->off >= 0) {
            break;
        }
        dir->off = 0;
        // fallthrough
      case BOOKMARKFS_FSCK_REWIND:
        return do_fsck(ctx, dir, NULL, NULL);

      case BOOKMARKFS_FSCK_SKIP_CHILDREN:
        if (ctx->dir_stack_top == 0) {
            ctx->flags |= FSCK_ALL_DONE;
        } else {
            free_dir(ctx, dir);
            --ctx->dir_stack_top;
        }
        break;

      case BOOKMARKFS_FSCK_RESET:
        for (; dir > ctx->dir_stack; --dir) {
            free_dir(ctx, dir);
        }
        ctx->dir_stack_top = 0;
        return reset_top(ctx);

      case BOOKMARKFS_FSCK_SAVE:
        if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
            break;
        }
        return ctx->backend->backend_sync(ctx->backend_ctx);

      default:
        unreachable();
    }
    return 0;
}

static int
fsck_create (
    struct bookmarkfs_backend const  *backend,
    char                             *path,
    struct bookmarkfs_conf_opt       *opts,
    uint32_t                          flags,
    void                            **fsck_ctx_ptr
) {
    char *sep = strchr(path, ':');
    if (sep != NULL) {
        *(sep++) = '\0';
    }

    struct bookmarkfs_backend_conf const conf = {
        .version    = BOOKMARKFS_VERNUM,
        .flags      = (flags | BOOKMARKFS_BACKEND_FSCK_ONLY) & 0xffff,
        .store_path = path,
        .opts       = opts,
    };
    struct bookmarkfs_backend_create_resp resp = {
        .bookmarks_root_id = UINT64_MAX,
        .tags_root_id      = UINT64_MAX,
    };
    if (0 != backend->backend_create(&conf, &resp)) {
        return -1;
    }
    debug_assert(resp.flags & BOOKMARKFS_BACKEND_EXCLUSIVE);

    uint64_t root_id = resp.bookmarks_root_id;
    if (!BOOKMARKFS_BOOKMARK_IS_TYPE(flags, BOOKMARK)) {
        if (BOOKMARKFS_BOOKMARK_IS_TYPE(flags, TAG)) {
            root_id = resp.tags_root_id;
            if (root_id == UINT64_MAX) {
                log_puts("backend does not support tags");
                goto fail;
            }
        } else if (BOOKMARKFS_BOOKMARK_IS_TYPE(flags, KEYWORD)) {
            if (!(resp.flags & BOOKMARKFS_BACKEND_HAS_KEYWORD)) {
                log_puts("backend does not support keywords");
                goto fail;
            }
            root_id = 0;
        }
        // Tag/keyword fsck only applies to toplevel dir.
        sep = NULL;
        flags &= ~BOOKMARKFS_FSCK_OP_RECURSIVE;
    }

    size_t dir_stack_size = 1;
    if (flags & BOOKMARKFS_FSCK_OP_RECURSIVE) {
        dir_stack_size = 16;
    }
    struct fsck_ctx *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct fsck_ctx) {
        .backend        = backend,
        .backend_ctx    = resp.backend_ctx,
        .dir_stack      = xmalloc(sizeof(struct fsck_dir) * dir_stack_size),
        .dir_stack_size = dir_stack_size,
        .path           = sep,
        .flags          = flags,
    };
    struct fsck_dir *dir = &ctx->dir_stack[0];
    *dir = FSCK_DIR_INIT(root_id);

    if (flags & BOOKMARKFS_BACKEND_NO_SANDBOX) {
        if (0 != init_top(ctx, dir->id, sep, &dir->id)) {
            free(ctx);
            goto fail;
        }
    }
    *fsck_ctx_ptr = ctx;
    return 0;

  fail:
    backend->backend_destroy(resp.backend_ctx);
    return -1;
}

static void
fsck_destroy (
    void *fsck_ctx
) {
    struct fsck_ctx *ctx = fsck_ctx;

    for (size_t idx = 0; idx <= ctx->dir_stack_top; ++idx) {
        free_dir(ctx, ctx->dir_stack + idx);
    }
    ctx->backend->backend_destroy(ctx->backend_ctx);
    free(ctx->dir_stack);
    free(ctx);
}

static void
fsck_info (
    struct bookmarkfs_backend const *backend,
    uint32_t                         flags
) {
    backend->backend_info(flags | BOOKMARKFS_FRONTEND_FSCK);
}

static int
fsck_next (
    void                                 *fsck_ctx,
    struct bookmarkfs_fsck_handler_entry *entry
) {
    struct fsck_ctx *ctx = fsck_ctx;

    if (ctx->flags & FSCK_ALL_DONE) {
        return 0;
    }
    struct fsck_dir *dir = ctx->dir_stack + ctx->dir_stack_top;
    int result;
    while (1) {
        if (dir->off < 0) {
            result = do_fsck(ctx, dir, NULL, entry);
            if (result < 0) {
                goto end;
            }
            if (result != BOOKMARKFS_FSCK_RESULT_END) {
                goto end;
            }
            dir->off = 0;
        }
        if (!(ctx->flags & BOOKMARKFS_FSCK_OP_RECURSIVE)) {
            return 0;
        }

        uint64_t subdir_id;
        for (; ; --dir) {
            result = do_list(ctx, dir, &subdir_id);
            if (result < 0) {
                goto end;
            }
            if (subdir_id != UINT64_MAX) {
                break;
            }
            if (dir == ctx->dir_stack) {
                goto end;
            }
            free_dir(ctx, dir);
        }

        if (++dir >= ctx->dir_stack + ctx->dir_stack_size) {
            size_t old_size = ctx->dir_stack_size;
            ctx->dir_stack_size = old_size + (old_size >> 1);
            ctx->dir_stack = xrealloc(ctx->dir_stack,
                    sizeof(struct fsck_dir) * ctx->dir_stack_size);
            dir = ctx->dir_stack + old_size;
        }
        *dir = FSCK_DIR_INIT(subdir_id);
    }

  end:
    ctx->dir_stack_top = dir - ctx->dir_stack;
    return result;
}

static int
fsck_sandbox (
    void *fsck_ctx
) {
    struct fsck_ctx *ctx = fsck_ctx;

    struct bookmarkfs_backend_create_resp info = {
        .bookmarks_root_id = UINT64_MAX,
    };
    if (0 != BACKEND_CALL(ctx, backend_sandbox, &info)) {
        return -1;
    }

    uint64_t top_id = ctx->dir_stack[0].id;
    if (top_id == UINT64_MAX) {
        top_id = info.bookmarks_root_id;
    }
    if (0 != init_top(ctx, top_id, ctx->path, &top_id)) {
        return -1;
    }
    ctx->dir_stack[0].id = top_id;
    return 0;
}

struct bookmarkfs_fsck_ops const fsck_offline_ops = {
    .info = fsck_info,

    .create  = fsck_create,
    .sandbox = fsck_sandbox,
    .destroy = fsck_destroy,

    .next    = fsck_next,
    .control = fsck_control,
    .apply   = fsck_apply,
};
