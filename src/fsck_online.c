/**
 * bookmarkfs/src/fsck_online.c
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
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "backend.h"
#include "fsck_ops.h"
#include "ioctl.h"
#include "macros.h"
#include "sandbox.h"
#include "version.h"
#include "xstd.h"

#define FSCK_ALL_DONE  ( 1u << 24 )

struct fsck_ctx {
    struct fsck_dir *dir_stack;
    size_t           dir_stack_size;
    size_t           dir_stack_top;

    char   *dent_buf;
    size_t  dent_buf_size;
    size_t  dent_buf_used;

    uint32_t flags;
};

struct fsck_dir {
    uint64_t id;
    int      fd;
    uint32_t flags;

    size_t dent_start;
    size_t dent_off;
};
#define FSCK_DIR_DONE  ( 1u << 0 )

// Forward declaration start
#ifdef __linux__
static ssize_t getdents_ (int, void *, size_t);
#endif

static int  next_subdir   (struct fsck_ctx *, struct fsck_dir *,
                           struct dirent const **);
static int  open_subdir   (int, char const *, uint64_t *);
static int  reset_top     (struct fsck_ctx *);
static void print_help    (void);
static void print_version (void);
// Forward declaration end

#ifdef __linux__

// Some libc (e.g., musl) may declare a getdents() function
// in dirent.h with conflicting types.
#define getdents getdents_
static ssize_t
getdents_ (
    int     dirfd,
    void   *buf,
    size_t  bufsize
) {
    return syscall(SYS_getdents64, dirfd, buf, bufsize);
}

#endif  /* defined(__linux__) */

static int
next_subdir (
    struct fsck_ctx      *ctx,
    struct fsck_dir      *dir,
    struct dirent const **dent_ptr
) {
    size_t start = dir->dent_start;
    size_t off   = dir->dent_off;
    size_t len   = ctx->dent_buf_used;
    while (1) {
        if (off == len) {
#define DIRENT_BUFSIZE  4096
            if (start + DIRENT_BUFSIZE > ctx->dent_buf_size) {
                ctx->dent_buf_size = start + DIRENT_BUFSIZE;
                ctx->dent_buf = xrealloc(ctx->dent_buf, ctx->dent_buf_size);
            }
            ssize_t nbytes
                = getdents(dir->fd, ctx->dent_buf + start, DIRENT_BUFSIZE);
            if (nbytes < 0) {
                log_printf("getdents(): %s", xstrerror(errno));
                return -1;
            }
            if (nbytes == 0) {
                *dent_ptr = NULL;
                return 0;
            }
            off = start;
            len = off + nbytes;
            ctx->dent_buf_used = len;
        }

        struct dirent *dent = (struct dirent *)(ctx->dent_buf + off);
        off += dent->d_reclen;
        debug_assert(off <= len);
        if (dent->d_type == DT_DIR) {
            *dent_ptr = dent;
            dir->dent_off = off;
            return 0;
        }
    }
}

static int
open_subdir (
    int         dirfd,
    char const *name,
    uint64_t   *id_ptr
) {
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_RESOLVE_BENEATH;
    int fd = openat(dirfd, name, flags);
    if (fd < 0) {
        log_printf("openat(): %s", xstrerror(errno));
        return -1;
    }

    struct stat stat_buf;
    if (0 != fstat(fd, &stat_buf)) {
        log_printf("fstat(): %s", xstrerror(errno));
        goto fail;
    }
    *id_ptr = stat_buf.st_ino & BOOKMARKFS_MAX_ID;
    return fd;

  fail:
    close(fd);
    return -1;
}

static int
reset_top (
    struct fsck_ctx *ctx
) {
    struct fsck_dir *dir = &ctx->dir_stack[0];
    if (0 != ioctl(dir->fd, BOOKMARKFS_IOC_FSCK_REWIND)) {
        log_printf("ioctl(): %s", xstrerror(errno));
        return -1;
    }
    if (0 != lseek(dir->fd, 0, SEEK_SET)) {
        log_printf("lseek(): %s", xstrerror(errno));
        return -1;
    }
    dir->flags    = 0;
    dir->dent_off = 0;

    ctx->dir_stack_top = 0;
    ctx->dent_buf_used = 0;
    ctx->flags &= ~FSCK_ALL_DONE;
    return 0;
}

static void
print_help (void)
{
    puts("Usage: fsck.bookmarkfs [options] <pathname>\n"
            "\n"
            "Common options:\n"
            "  -o backend=<name>      Backend used by the filesystem\n"
            "  -o @<key>[=<value>]    Backend-specific option\n"
            "  -o handler=<name>      Handler for resolving fsck errors\n"
            "  -o %<key>[=<value>]    Handler-specific option\n"
            "  -o repair              Attempt to repair fsck errors\n"
            "  -o rl_app=<name>       Readline application name\n"
            "  -o type=tag|keyword    Perform fsck on tags/keywords\n"
            "\n"
            "  -i    Enable interactive mode\n"
            "  -R    Perform fsck on subdirectories recursively\n"
            "\n"
            "Other options:\n"
            "  -o no_sandbox     Disable sandbox\n"
#ifdef __linux__
            "  -o no_landlock    Disable Landlock features for sandbox\n"
#endif
            "\n"
            "  -h    Print help message and exit\n"
            "  -V    Print version information and exit\n"
            "\n"
            "See the fsck.bookmarkfs(1) manpage for more information,\n"
            "or run 'info bookmarkfs' for the full user manual.\n"
            "\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.");
}

static void
print_version (void)
{
    printf("fsck.bookmarkfs (BookmarkFS) %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
    puts(BOOKMARKFS_FEATURE_STRING(DEBUG,            "debug"));
    puts(BOOKMARKFS_FEATURE_STRING(INTERACTIVE_FSCK, "interactive"));
}

static int
fsck_apply (
    void                                 *fsck_ctx,
    struct bookmarkfs_fsck_handler_entry *entry
) {
    struct fsck_ctx *ctx = fsck_ctx;

    if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
        log_puts("fsck is in readonly mode");
        return -1;
    }

    struct fsck_dir *dir = ctx->dir_stack + ctx->dir_stack_top;
    int result = ioctl(dir->fd, BOOKMARKFS_IOC_FSCK_APPLY, &entry->data);
    if (result < 0) {
        log_printf("ioctl(): %s", xstrerror(errno));
        return -1;
    }
    entry->parent_id = dir->id;
    return result;
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
        dir->flags |= FSCK_DIR_DONE;
        // fallthrough
      case BOOKMARKFS_FSCK_REWIND:
        if (0 != ioctl(dir->fd, BOOKMARKFS_IOC_FSCK_REWIND)) {
            log_printf("ioctl(): %s", xstrerror(errno));
            return -1;
        }
        break;

      case BOOKMARKFS_FSCK_SKIP_CHILDREN:
        if (ctx->dir_stack_top == 0) {
            ctx->flags |= FSCK_ALL_DONE;
        } else {
            ctx->dent_buf_used = dir->dent_start;
            close(dir->fd);
            --ctx->dir_stack_top;
        }
        break;

      case BOOKMARKFS_FSCK_RESET:
        for (; dir > ctx->dir_stack; --dir) {
            close(dir->fd);
        }
        ctx->dir_stack_top = 0;
        return reset_top(ctx);

      case BOOKMARKFS_FSCK_SAVE:
        if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
            break;
        }
        return xfsync(dir->fd);

      default:
        unreachable();
    }
    return 0;
}

static int
fsck_create (
    struct bookmarkfs_backend const  *UNUSED_VAR(backend),
    char                             *path,
    struct bookmarkfs_conf_opt       *UNUSED_VAR(opts),
    uint32_t                          flags,
    void                            **fsck_ctx_ptr
) {
    int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        log_printf("open(): %s", xstrerror(errno));
        return -1;
    }

    size_t dir_stack_size = 1;
    if (flags & BOOKMARKFS_FSCK_OP_RECURSIVE) {
        dir_stack_size = 16;
    }
    struct fsck_ctx *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct fsck_ctx) {
        .dir_stack      = xmalloc(sizeof(struct fsck_dir) * dir_stack_size),
        .dir_stack_size = dir_stack_size,
        .flags          = flags,
    };
    ctx->dir_stack[0] = (struct fsck_dir) {
        .id = UINT64_MAX,
        .fd = dirfd,
    };
    *fsck_ctx_ptr = ctx;
    return 0;
}

static void
fsck_destroy (
    void *fsck_ctx
) {
    struct fsck_ctx *ctx = fsck_ctx;

    for (size_t idx = 0; idx <= ctx->dir_stack_top; ++idx) {
        close(ctx->dir_stack[idx].fd);
    }
    free(ctx->dir_stack);
    free(ctx->dent_buf);
    free(ctx);
}

static void
fsck_info (
    struct bookmarkfs_backend const *UNUSED_VAR(backend),
    uint32_t                         flags
) {
    if (flags & BOOKMARKFS_BACKEND_INFO_HELP) {
        print_help();
    } else if (flags & BOOKMARKFS_BACKEND_INFO_VERSION) {
        print_version();
    }
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
        if (!(dir->flags & FSCK_DIR_DONE)) {
            result = ioctl(dir->fd, BOOKMARKFS_IOC_FSCK_NEXT, &entry->data);
            if (result < 0) {
                log_printf("ioctl(): %s", xstrerror(errno));
                goto end;
            }
            if (result != BOOKMARKFS_FSCK_RESULT_END) {
                entry->parent_id = dir->id;
                goto end;
            }
            dir->flags |= FSCK_DIR_DONE;
        }
        if (!(ctx->flags & BOOKMARKFS_FSCK_OP_RECURSIVE)) {
            return 0;
        }

        struct dirent const *dent;
        for (; ; --dir) {
            result = next_subdir(ctx, dir, &dent);
            if (result < 0) {
                goto end;
            }
            if (dent != NULL) {
                break;
            }
            if (dir == ctx->dir_stack) {
                goto end;
            }
            ctx->dent_buf_used = dir->dent_start;
            close(dir->fd);
            debug_puts("exiting directory...");
        }

        uint64_t id;
        result = open_subdir(dir->fd, dent->d_name, &id);
        if (result < 0) {
            goto end;
        }
        debug_printf("entering directory '%s'...", dent->d_name);
        if (++dir >= ctx->dir_stack + ctx->dir_stack_size) {
            size_t old_size = ctx->dir_stack_size;
            ctx->dir_stack_size = old_size + (old_size >> 1);
            ctx->dir_stack = xrealloc(ctx->dir_stack,
                    sizeof(struct fsck_dir) * ctx->dir_stack_size);
            dir = ctx->dir_stack + old_size;
        }
        *dir = (struct fsck_dir) {
            .id         = id,
            .fd         = result,
            .dent_start = ctx->dent_buf_used,
            .dent_off   = ctx->dent_buf_used,
        };
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

    if (ctx->flags & BOOKMARKFS_BACKEND_NO_SANDBOX) {
        return 0;
    }
    uint32_t flags = 0;
    if (ctx->flags & BOOKMARKFS_BACKEND_NO_LANDLOCK) {
        flags |= SANDBOX_NO_LANDLOCK;
    }
    return sandbox_enter(ctx->dir_stack[0].fd, flags);
}

struct bookmarkfs_fsck_ops const fsck_online_ops = {
    .info = fsck_info,

    .create  = fsck_create,
    .sandbox = fsck_sandbox,
    .destroy = fsck_destroy,
    
    .next    = fsck_next,
    .control = fsck_control,
    .apply   = fsck_apply,
};
