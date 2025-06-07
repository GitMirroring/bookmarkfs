/**
 * bookmarkfs/src/fs_ops.c
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

#include "fs_ops.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
#  include <sys/xattr.h>
#endif
#include <unistd.h>

#include "hash.h"
#include "hashmap.h"
#include "ioctl.h"
#include "macros.h"
#include "xstd.h"

#define FS_FILEMODE_REG   ( S_IFREG | (ctx.flags.accmode & ~0111u) )
#define FS_FILEMODE_DIR   ( S_IFDIR | ctx.flags.accmode )
#define FS_FILEMODE_ROOT  ( FS_FILEMODE_DIR & ~(S_IWUSR | S_IWGRP | S_IWOTH) )

// The 2.5s timeout value is chosen according to how frequent
// Chromium saves its bookmarks (`bookmarks::BookmarkStorage::kSaveDelay`).
//
// See Chromium source code: /components/bookmarks/browser/bookmark_storage.h
#define FS_ENTRY_TIMEOUT_MAX  3600.
#define FS_ENTRY_TIMEOUT  ( ctx.flags.exclusive ? FS_ENTRY_TIMEOUT_MAX : 2.5 )

#define BOOKMARKS_ROOT_ID  ( ctx.bookmarks_root_id )
#define TAGS_ROOT_ID       ( ctx.tags_root_id )

#define INTFS_NAME_BOOKMARKS  "bookmarks"
#define INTFS_NAME_TAGS       "tags"
#define INTFS_NAME_KEYWORDS   "keywords"

#define FH_BUF_BLOCKSIZE    512
#define FH_BUF_RESERVED(s)  ( FH_BUF_BLOCKSIZE - (s) % FH_BUF_BLOCKSIZE )

#define FH_FLAG_DIRTY  ( 1u << 0 )
#define FH_FLAG_FSCK   ( 1u << 1 )

#define SUBSYS_ID_BITS          ( 64 - BOOKMARKFS_BOOKMARK_TYPE_BITS )
#define SUBSYS_ID_MASK          ( ((fuse_ino_t)1 << SUBSYS_ID_BITS) - 1 )
#define INODE_SUBSYS_TYPE(ino)  ( (ino) >> SUBSYS_ID_BITS )
#define INODE_SUBSYS_ID(ino)    ( (ino) & SUBSYS_ID_MASK )
#define SUBSYS_NODEID(id, t)    ( ((fuse_ino_t)(t) << SUBSYS_ID_BITS) | (id) )

#define STRUCT_FUSE_ENTRY_PARAM_INIT        \
    {                                       \
        .generation    = 1,                 \
        .attr          = STRUCT_STAT_INIT,  \
        .attr_timeout  = FS_ENTRY_TIMEOUT,  \
        .entry_timeout = FS_ENTRY_TIMEOUT,  \
    }
#define STRUCT_STAT_INIT  \
    { .st_nlink = 1, .st_uid = ctx.uid, .st_gid = ctx.gid }

#define BACKEND_CALL(name, ...)  \
    ctx.backend_impl->name(ctx.backend_ctx, __VA_ARGS__)

#define BM_XATTR_FOREACH(name, name_len)                                \
    for (char const *name = ctx.xattr_names, *next_; ; name = next_) {  \
        size_t name_len = strlen(name);                                 \
        if (name_len == 0) {                                            \
            break;                                                      \
        }                                                               \
        next_ = name + name_len + 1;
#define BM_XATTR_FOREACH_END  \
    }

#define backend_has_tags()      ( TAGS_ROOT_ID != UINT64_MAX )
#define backend_has_keywords()  ( ctx.flags.has_keyword )
#define send_reply(name, ...)  \
    reply_errcheck(fuse_reply_##name(__VA_ARGS__), #name, __LINE__)

// Some platforms (e.g., Arm Morello) have 128-bit pointers,
// where we cannot safely store a pointer to `fi->fh`.
#if defined(SIZEOF_UINTPTR_T) && (SIZEOF_UINTPTR_T > 8)
#  error "sizeof(uintptr_t) > sizeof(uint64_t)"
#endif
#define ptr2uint(val)  ( (uintptr_t)(void *)(val) )
#define uint2ptr(val)  ( (void *)(uintptr_t)(val) )

enum {
    INTFS_ID_ROOT = FUSE_ROOT_ID,
    INTFS_ID_BOOKMARKS,
    INTFS_ID_TAGS,
    INTFS_ID_KEYWORDS,
};

enum {
    SUBSYS_TYPE_INTERNAL,
    SUBSYS_TYPE_BOOKMARK,
    SUBSYS_TYPE_TAG,
    SUBSYS_TYPE_KEYWORD,
};

struct fs_file_handle {
    fuse_ino_t    ino;
    unsigned long hashcode;

    uint32_t refcount;
    uint32_t flags;

    char     *buf;
    uint32_t  buf_len;
    uint32_t  data_len;
    void     *cookie;

    struct timespec mtime;
};

struct fs_ctx {
    struct bookmarkfs_backend const *backend_impl;
    void                            *backend_ctx;

    struct hashmap  *fh_map;
    uid_t            uid;
    gid_t            gid;
    struct fs_flags  flags;

    uint64_t    bookmarks_root_id;
    uint64_t    tags_root_id;
    size_t      file_max;
    char const *xattr_names;

    char   *buf;
    size_t  buf_len;

    struct fuse_session *session;
};

struct bm_check_ctx {
    struct bookmarkfs_fsck_data out;

    int result;
};

struct bm_getxattr_ctx {
    fuse_req_t req;
    size_t     buf_max;
};

struct bm_read_ctx {
    struct fs_file_handle *fh;
};

struct bm_readdir_ctx {
    fuse_req_t req;
    uint32_t   flags;

    size_t buf_len;
    size_t reply_len;
};

// Forward declaration start
#ifndef __FreeBSD__
static int  bm_syncdir    (fuse_ino_t);
static int  intfs_syncdir (fuse_ino_t);
static int  inval_dir     (fuse_ino_t, struct fs_file_handle *);
static int  inval_inode   (fuse_ino_t);
#endif  /* !defined(__FreeBSD__) */

static int  bm_check       (fuse_ino_t, struct bookmarkfs_fsck_data const *,
                            uint32_t, struct bm_check_ctx *, void *);
static int  bm_check_cb    (void *, int, uint64_t, uint64_t, char const *);
static int  bm_create      (fuse_ino_t, char const *, int, struct stat *);
static int  bm_delete      (fuse_ino_t, char const *, uint32_t);
static int  bm_do_write    (fuse_ino_t, struct fs_file_handle *);
static void bm_fh_free     (struct fs_file_handle *, long);
static struct fs_file_handle * FUNCATTR_PURE
            bm_fh_get      (fuse_ino_t, unsigned long *, unsigned long *);
static struct fs_file_handle *
            bm_fh_new      (fuse_ino_t);
static void bm_fillstat    (struct bookmarkfs_bookmark_stat const *, int, bool,
                            struct stat *);
static int  bm_free        (fuse_ino_t, struct fuse_file_info const *);
static int  bm_freedir     (fuse_ino_t, void *);
static int  bm_getxattr    (fuse_req_t, fuse_ino_t, char const *, size_t);
static int  bm_getxattr_cb (void *, void const *, size_t);
static int  bm_ioctl       (fuse_req_t, fuse_ino_t, unsigned, uint32_t,
                            void const *, size_t, size_t, void *);
static int  bm_lookup      (fuse_ino_t, char const *, uint32_t, struct stat *);
static int  bm_lsxattr     (fuse_ino_t, char *, size_t, size_t *);
static int  bm_mkdir       (fuse_ino_t, char const *, int, struct stat *);
static int  bm_open        (fuse_ino_t, struct fuse_file_info *);
static int  bm_opendir     (fuse_ino_t, uint32_t, struct fuse_file_info *);
static int  bm_permute     (fuse_ino_t, struct bookmarkfs_permd_data const *,
                            uint32_t);
static int  bm_read        (fuse_req_t, fuse_ino_t, size_t, off_t,
                            struct fs_file_handle *);
static int  bm_read_cb     (void *, void const *, size_t);
static int  bm_readdir     (fuse_req_t, fuse_ino_t, size_t, off_t, uint32_t,
                            void *);
static int  bm_readdir_cb  (void *, struct bookmarkfs_bookmark_entry const *);
static int  bm_rename      (fuse_ino_t, char const *, fuse_ino_t, char const *,
                            uint32_t);
static int  bm_rmxattr     (fuse_ino_t, char const *);
static int  bm_set_keyword (fuse_ino_t, char const *, struct stat *);
static int  bm_set_tag     (fuse_ino_t, fuse_ino_t, char const *,
                            struct stat *);
static int  bm_setattr     (fuse_ino_t, int, bool,
                            struct fuse_file_info const *, struct stat *);
static int  bm_setxattr    (fuse_ino_t, char const *, void const *, size_t,
                            int);
static int  bm_write       (fuse_req_t, int, char const *, size_t, off_t,
                            struct fs_file_handle *);
static void do_delete      (fuse_req_t, fuse_ino_t, char const *, uint32_t);
static void do_readdir     (fuse_req_t, fuse_ino_t, size_t, off_t, uint32_t,
                            struct fuse_file_info const *);
static unsigned long
            fh_entry_hash  (void const *);
static int  fh_entry_comp  (union hashmap_key, void const *);
static bool has_access     (fuse_req_t, int);
static int  intfs_create   (fuse_ino_t, char const *, int, struct stat *,
                            struct fuse_file_info *);
static int  intfs_delete   (fuse_ino_t, char const *, uint32_t);
static int  intfs_freedir  (fuse_ino_t, void *);
static int  intfs_ioctl    (fuse_req_t, fuse_ino_t, unsigned, void const *,
                            size_t, size_t, void *);
static int  intfs_link     (fuse_ino_t, fuse_ino_t, char const *,
                            struct stat *);
static int  intfs_lookup   (fuse_ino_t, char const *, struct stat *);
static int  intfs_lsxattr  (fuse_ino_t, char *, size_t, size_t *);
static int  intfs_mkdir    (fuse_ino_t, char const *, struct stat *);
static int  intfs_opendir  (fuse_ino_t, struct fuse_file_info *);
static int  intfs_readdir  (fuse_req_t, fuse_ino_t, size_t, off_t, uint32_t,
                            void *);
static int  intfs_rename   (fuse_ino_t, char const *, char const *, uint32_t);
static bool is_xattr_name  (char const *);
static int  reply_errcheck (int, char const *, int);
static int  timespec_cmp   (struct timespec const *, struct timespec const *);
// Forward declaration end

static struct fs_ctx ctx = {
    .tags_root_id = UINT64_MAX,
};

#ifndef __FreeBSD__

static int
bm_syncdir (
    fuse_ino_t ino
) {
    struct fs_file_handle *fh = bm_fh_get(ino, NULL, NULL);
    return inval_dir(ino, fh);
}

static int
intfs_syncdir (
    fuse_ino_t ino
) {
    switch (ino) {
      case INTFS_ID_BOOKMARKS:
        ino = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        ino = TAGS_ROOT_ID;
        break;

      case INTFS_ID_KEYWORDS:
        ino = 0;
        break;

      default:
        return 0;
    }
    return bm_syncdir(ino);
}

/**
 * An explicit flush is required, when the kernel holds dent cache
 * indefinitely, and dents are changed in a way that it could not perceive.
 *
 * Not needed on FreeBSD, since it does not support dent caching for fusefs.
 */
static int
inval_dir (
    fuse_ino_t             ino,
    struct fs_file_handle *fh
) {
    if (ctx.flags.readonly || !ctx.flags.exclusive) {
        return 0;
    }
    if (fh->flags & FH_FLAG_DIRTY) {
        if (0 != inval_inode(ino)) {
            return -EIO;
        }
        fh->flags &= ~FH_FLAG_DIRTY;
    }
    return 0;
}

/**
 * XXX: We cannot do this on FreeBSD.
 *
 * The client holds an exclusive lock of a vnode whenever a VFS request is
 * being processed on it.  Meanwhile, FUSE_NOTIFY_INVAL_INODE also requires
 * locking the vnode, which may block (on writing to the FUSE device).
 *
 * Once FUSE_NOTIFY_INVAL_INODE blocks, we're not able to precess other
 * requests until write(2) returns.
 * This results in a deadlock since we're using a single-threaded event loop.
 */
static int
inval_inode (
    fuse_ino_t ino
) {
    int status = fuse_lowlevel_notify_inval_inode(ctx.session, ino, 0, 0);
    if (status < 0) {
        log_printf("fuse_lowlevel_notify_inval_inode(): %s",
                xstrerror(-status));
    }
    return status;
}

#endif  /* !defined(__FreeBSD__) */

static int
bm_check (
    fuse_ino_t                         parent,
    struct bookmarkfs_fsck_data const *data,
    uint32_t                           flags,
    struct bm_check_ctx               *ckctx,
    void                              *cookie
) {
    if (ctx.backend_impl->bookmark_check == NULL) {
        return -ENOTTY;
    }
    bookmarkfs_bookmark_check_cb *callback = NULL;
    if (ckctx != NULL) {
        callback = bm_check_cb;
        ckctx->result = BOOKMARKFS_FSCK_RESULT_END;
    }
    int status = BACKEND_CALL(bookmark_check, INODE_SUBSYS_ID(parent), data,
            flags, callback, ckctx, &cookie);
    if (status < 0) {
        return status;
    }
    if (ckctx != NULL) {
        return ckctx->result;
    }
    return 0;
}

static int
bm_check_cb (
    void       *user_data,
    int         result,
    uint64_t    id,
    uint64_t    extra,
    char const *name
) {
    struct bm_check_ctx *ckctx = user_data;

    ckctx->out.id    = id;
    ckctx->out.extra = extra;
    if (name != ckctx->out.name) {
        strncpy(ckctx->out.name, name, sizeof(ckctx->out.name));
    }
    ckctx->result = result;
    return 1;
}

static int
bm_create (
    fuse_ino_t   parent,
    char const  *name,
    int          flags,
    struct stat *stat_buf
) {
    struct bookmarkfs_bookmark_stat stat;
    int status = BACKEND_CALL(bookmark_create, INODE_SUBSYS_ID(parent), name,
            0, &stat);
    if (status < 0) {
        if (status != -EEXIST || (flags & O_EXCL)) {
            return status;
        }
    }

    if (flags & O_TRUNC) {
        // See bm_open() for comments on O_RDONLY|O_TRUNC.
        if ((flags & O_ACCMODE) != O_RDONLY) {
            stat.value_len = 0;
        }
    }
    bm_fillstat(&stat, SUBSYS_TYPE_BOOKMARK, false, stat_buf);
    return 0;
}

static int
bm_delete (
    fuse_ino_t  parent,
    char const *name,
    uint32_t    flags
) {
    return BACKEND_CALL(bookmark_delete, INODE_SUBSYS_ID(parent), name, flags);
}

static int
bm_do_write (
    fuse_ino_t             ino,
    struct fs_file_handle *fh
) {
    if (fh == NULL || !(fh->flags & FH_FLAG_DIRTY)) {
        return 0;
    }

    size_t data_len = fh->data_len;
    if (ctx.flags.eol && fh->buf[data_len - 1] == '\n') {
        --data_len;
    }
    uint64_t id = INODE_SUBSYS_ID(ino);
    int status = BACKEND_CALL(bookmark_set, id, NULL, 0, fh->buf, data_len);
    if (status < 0) {
        return status;
    }

    struct timespec const times[2] = {
        [1] = fh->mtime,
    };
    uint32_t set_flags = BOOKMARK_FLAG(SET_MTIME);
    status = BACKEND_CALL(bookmark_set, id, NULL, set_flags, times, 0);
    if (status < 0) {
        return status;
    }

    fh->flags &= ~FH_FLAG_DIRTY;
    return 0;
}

static void
bm_fh_free (
    struct fs_file_handle *fh,
    long                   entry_id
) {
    hashmap_delete(ctx.fh_map, fh, entry_id);
    free(fh);
}

static struct fs_file_handle *
bm_fh_get (
    fuse_ino_t     ino,
    unsigned long *hashcode_ptr,
    unsigned long *entry_id_ptr
) {
    unsigned long hashcode = hash_digest(&ino, sizeof(ino));
    if (hashcode_ptr != NULL) {
        *hashcode_ptr = hashcode;
    }

    union hashmap_key key = { .u64 = ino };
    return hashmap_search(ctx.fh_map, key, hashcode, entry_id_ptr);
}

static struct fs_file_handle *
bm_fh_new (
    fuse_ino_t ino
) {
    unsigned long hashcode;
    struct fs_file_handle *fh = bm_fh_get(ino, &hashcode, NULL);
    if (fh != NULL) {
        ++fh->refcount;
        return fh;
    }

    fh = xmalloc(sizeof(*fh));
    *fh = (struct fs_file_handle) {
        .ino      = ino,
        .hashcode = hashcode,
        .refcount = 1,
    };
    hashmap_insert(ctx.fh_map, hashcode, fh);
    return fh;
}

static void
bm_fillstat (
    struct bookmarkfs_bookmark_stat const *src,
    int                                    subsys_type,
    bool                                   with_local,
    struct stat                           *dest
) {
    fuse_ino_t ino = SUBSYS_NODEID(src->id, subsys_type);

    dest->st_ino  = ino;
    dest->st_atim = src->atime;
    dest->st_mtim = src->mtime;
    dest->st_ctim = src->mtime;

    if (src->value_len < 0) {
        dest->st_mode = FS_FILEMODE_DIR;
        return;
    }
    dest->st_mode = FS_FILEMODE_REG;
    dest->st_size = src->value_len + ctx.flags.eol;

    if (!with_local) {
        return;
    }
    struct fs_file_handle *fh = bm_fh_get(ino, NULL, NULL);
    if (fh == NULL || !(fh->flags & FH_FLAG_DIRTY)) {
        return;
    }
    // If file content changes locally, remote mtime no longer matters,
    // since remote changes to the file will eventually be overwritten.
    if (!ctx.flags.ctime || 0 < timespec_cmp(&fh->mtime, &dest->st_mtim)) {
        dest->st_mtim = fh->mtime;
        dest->st_ctim = fh->mtime;
    }
    dest->st_size = fh->data_len;
}

static int
bm_free (
    fuse_ino_t                   ino,
    struct fuse_file_info const *fi
) {
    struct fs_file_handle *fh = uint2ptr(fi->fh);
    if (--fh->refcount > 0) {
        return 0;
    }

    int status = bm_do_write(ino, fh);
#ifndef __FreeBSD__
    if (status != 0) {
        // When writeback fails, bookmark retains original data.
        // The kernel should re-fetch it from the server.
        if (0 != inval_inode(ino)) {
            status = -EIO;
        }
    }
#endif
    if (!ctx.flags.readonly) {
        free(fh->buf);
    }
    if (fh->cookie != NULL) {
        BACKEND_CALL(cookie_free, fh->cookie, BOOKMARKFS_COOKIE_TYPE_WATCH);
    }
    bm_fh_free(fh, -1);
    return status;
}

static int
bm_freedir (
    fuse_ino_t  ino,
    void       *cookie
) {
    BACKEND_CALL(cookie_free, cookie, BOOKMARKFS_COOKIE_TYPE_LIST);

    unsigned long entry_id;
    struct fs_file_handle *fh = bm_fh_get(ino, NULL, &entry_id);
    if ((fh->flags & FH_FLAG_FSCK) && fh->cookie == cookie) {
        fh->flags &= ~FH_FLAG_FSCK;
    }
    if (--fh->refcount > 0) {
        return 0;
    }

    int status = 0;
#ifndef __FreeBSD__
    status = inval_dir(ino, fh);
#endif
    bm_fh_free(fh, entry_id);
    return status;
}

static int
bm_getxattr (
    fuse_req_t   req,
    fuse_ino_t   ino,
    char const  *name,
    size_t       buf_max
) {
    if (!is_xattr_name(name)) {
        return -ENOATTR;
    }

    name += strlen(BOOKMARKFS_XATTR_PREFIX);
    struct bm_getxattr_ctx gctx = {
        .req     = req,
        .buf_max = buf_max,
    };
    return BACKEND_CALL(bookmark_get, INODE_SUBSYS_ID(ino), name,
            bm_getxattr_cb, &gctx, NULL);
}

static int
bm_getxattr_cb (
    void       *user_data,
    void const *xattr_val,
    size_t      xattr_len
) {
    struct bm_getxattr_ctx const *gctx = user_data;

    size_t buf_max = gctx->buf_max;
    if (buf_max == 0) {
        send_reply(xattr, gctx->req, xattr_len);
        return 0;
    }

    if (xattr_len > buf_max) {
        return -ERANGE;
    }
    return send_reply(buf, gctx->req, xattr_val, xattr_len);
}

static int
bm_ioctl (
    fuse_req_t  req,
    fuse_ino_t  ino,
    unsigned    cmd,
    uint32_t    flags,
    void const *ibuf,
    size_t      ibuf_len,
    size_t      obuf_len,
    void       *cookie
) {
    struct fs_file_handle *fh = bm_fh_get(ino, NULL, NULL);
    void *obuf = ctx.buf;
    int result = 0;

    switch (cmd) {
      case BOOKMARKFS_IOC_FSCK_REWIND:
        result = bm_check(ino, NULL, flags, NULL, cookie);
        if (result == 0) {
            fh->flags &= ~FH_FLAG_FSCK;
        }
        break;

      case BOOKMARKFS_IOC_FSCK_NEXT:
        debug_assert(obuf_len == sizeof(struct bookmarkfs_fsck_data));
        if (!has_access(req, R_OK)) {
            return -EACCES;
        }
        if ((fh->flags & FH_FLAG_FSCK) && fh->cookie != cookie) {
            return -EBUSY;
        }
        result = bm_check(ino, NULL, flags, obuf, cookie);
        if (result < 0) {
            break;
        }
        if (result == BOOKMARKFS_FSCK_RESULT_END) {
            fh->flags &= ~FH_FLAG_FSCK;
            obuf_len = 0;
        } else {
            fh->cookie = cookie;
            fh->flags |= FH_FLAG_FSCK;
        }
        break;

      case BOOKMARKFS_IOC_FSCK_APPLY:
        debug_assert(ibuf_len == sizeof(struct bookmarkfs_fsck_data));
        debug_assert(ibuf_len == obuf_len);
        if (ctx.flags.readonly) {
            return -EROFS;
        }
        if (!has_access(req, W_OK | X_OK)) {
            return -EACCES;
        }
        result = bm_check(ino, ibuf, flags, obuf, cookie);
        if (result == BOOKMARKFS_FSCK_RESULT_END) {
            fh->flags |= FH_FLAG_DIRTY;
            obuf_len = 0;
        }
        break;

      case BOOKMARKFS_IOC_PERMD:
        debug_assert(ibuf_len == sizeof(struct bookmarkfs_permd_data));
        if (ctx.flags.readonly) {
            return -EROFS;
        }
        if (!has_access(req, W_OK | X_OK)) {
            return -EACCES;
        }
        result = bm_permute(ino, ibuf, flags);
        if (result < 0) {
            return result;
        }
        fh->flags |= FH_FLAG_DIRTY;
        break;

      default:
        return -ENOTTY;
    }
    return send_reply(ioctl, req, result, obuf, obuf_len);
}

static int
bm_lookup (
    fuse_ino_t   parent,
    char const  *name,
    uint32_t     flags,
    struct stat *stat_buf
) {
    uint64_t parent_id = INODE_SUBSYS_ID(parent);
    struct bookmarkfs_bookmark_stat stat;
    int status = BACKEND_CALL(bookmark_lookup, parent_id, name, flags, &stat);
    if (status < 0) {
        return status;
    }

    int subsys_type = SUBSYS_TYPE_BOOKMARK;
    if (BOOKMARKFS_BOOKMARK_IS_TYPE(flags, TAG) && parent_id == TAGS_ROOT_ID) {
        subsys_type = SUBSYS_TYPE_TAG;
    }
    bm_fillstat(&stat, subsys_type, true, stat_buf);
    return 0;
}

static int
bm_lsxattr (
    fuse_ino_t  UNUSED_VAR(ino),
    char       *buf,
    size_t      buf_max,
    size_t     *buf_len_ptr
) {
    size_t buf_len    = 0;
    size_t prefix_len = strlen(BOOKMARKFS_XATTR_PREFIX);

    BM_XATTR_FOREACH(xattr, xattr_len)
        xattr_len += prefix_len + 1;
        buf_len   += xattr_len;
        if (buf_max == 0) {
            continue;
        }
        if (unlikely(buf_len > buf_max)) {
            return -ERANGE;
        }
        char *dest = buf + buf_len - xattr_len;
        memcpy(dest, BOOKMARKFS_XATTR_PREFIX, prefix_len);
        memcpy(dest + prefix_len, xattr, xattr_len - prefix_len);
    BM_XATTR_FOREACH_END

    *buf_len_ptr = buf_len;
    return 0;
}

static int
bm_mkdir (
    fuse_ino_t   parent,
    char const  *name,
    int          subsys_type,
    struct stat *stat_buf
) {
    uint32_t flags = BOOKMARK_FLAG(CREATE_DIR);
    if (subsys_type == SUBSYS_TYPE_TAG) {
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
    }
    struct bookmarkfs_bookmark_stat stat;
    int status = BACKEND_CALL(bookmark_create, INODE_SUBSYS_ID(parent), name,
            flags, &stat);
    if (status < 0) {
        return status;
    }

    bm_fillstat(&stat, subsys_type, false, stat_buf);
    return 0;
}

static int
bm_open (
    fuse_ino_t             ino,
    struct fuse_file_info *fi
) {
    struct fs_file_handle *fh = bm_fh_new(ino);

    int flags = fi->flags;
    if (flags & O_TRUNC) {
        // POSIX does not specify the behavior for O_RDONLY|O_TRUNC.
        // We choose to silently ignore O_TRUNC.
        if ((flags & O_ACCMODE) != O_RDONLY) {
            fh->data_len = 0;
            xgetrealtime(&fh->mtime);
            fh->flags |= FH_FLAG_DIRTY;
        }
    }

#ifdef O_DIRECT
    if (flags & O_DIRECT) {
        fi->direct_io = 1;
    }
#endif
#ifndef __FreeBSD__
    // Cannot reliably keep cache on FreeBSD, we're unable to explicitly
    // flush it.  See comments for inval_inode().
    fi->keep_cache = 1;
#endif
    fi->noflush = 1;
    fi->fh = ptr2uint(fh);
    return 0;
}

static int
bm_opendir (
    fuse_ino_t             ino,
    uint32_t               flags,
    struct fuse_file_info *fi
) {
    uint64_t id = INODE_SUBSYS_ID(ino);
    void *cookie = NULL;
    int status = BACKEND_CALL(bookmark_list, id, 0, flags, NULL, NULL,
            &cookie);
    if (status < 0) {
        return status;
    }

    bm_fh_new(ino);
    // Do not cache tag and keyword dents, since a bookmark
    // delete or rename may have a side effect of changing them.
    switch (flags & BOOKMARKFS_BOOKMARK_TYPE_MASK) {
      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        if (id != TAGS_ROOT_ID) {
            break;
        }
        // fallthrough
      case BOOKMARKFS_BOOKMARK_TYPE(BOOKMARK):
        fi->cache_readdir = 1;
        fi->keep_cache    = 1;
        break;
    }
    fi->fh = ptr2uint(cookie);
    return 0;
}

static int
bm_permute (
    fuse_ino_t                          ino,
    struct bookmarkfs_permd_data const *permd_data,
    uint32_t                            flags
) {
    return BACKEND_CALL(bookmark_permute, INODE_SUBSYS_ID(ino), permd_data->op,
            permd_data->name1, permd_data->name2, flags);
}

static int
bm_read (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fs_file_handle *fh
) {
    if (fh->flags & FH_FLAG_DIRTY) {
        goto do_read;
    }
    void **cookie_ptr = &fh->cookie;
    if (ctx.flags.exclusive) {
        // In exclusive mode, ignore server-side changes.
        if (fh->buf != NULL) {
            goto do_read;
        }
        cookie_ptr = NULL;
    }
    struct bm_read_ctx rctx = { .fh = fh };
    int result = BACKEND_CALL(bookmark_get, INODE_SUBSYS_ID(ino), NULL,
            bm_read_cb, &rctx, cookie_ptr);
    if (result < 0) {
        if (result != -EAGAIN && result != -ESTALE) {
            return result;
        }
    }

  do_read:  ;
    char   *buf     = fh->buf;
    size_t  buf_len = fh->data_len;
    if ((size_t)off > buf_len) {
        off = buf_len;
    }
    size_t bytes_read = buf_len - off;
    if (bytes_read > buf_max) {
        bytes_read = buf_max;
    }
    // buf may be NULL
    return send_reply(buf, req, off == 0 ? buf : buf + off, bytes_read);
}

static int
bm_read_cb (
    void       *user_data,
    void const *val,
    size_t      val_len
) {
    struct bm_read_ctx *rctx = user_data;

    size_t buf_len = val_len;
    if (ctx.flags.readonly) {
        buf_len += ctx.flags.eol;
    } else {
        buf_len += FH_BUF_RESERVED(buf_len);
    }

    struct fs_file_handle *fh = rctx->fh;
    if (fh->buf_len < buf_len) {
        free(fh->buf);
        fh->buf     = xmalloc(buf_len);
        fh->buf_len = buf_len;
    }
    memcpy(fh->buf, val, val_len);
    if (ctx.flags.eol) {
        fh->buf[val_len++] = '\n';
    }
    fh->data_len = val_len;
    return 0;
}

static int
bm_readdir (
    fuse_req_t  req,
    fuse_ino_t  ino,
    size_t      buf_max,
    off_t       off,
    uint32_t    flags,
    void       *cookie
) {
    struct bm_readdir_ctx rctx = {
        .req     = req,
        .flags   = flags,
        .buf_len = buf_max,
    };
    uint64_t id = INODE_SUBSYS_ID(ino);
    if (BOOKMARKFS_BOOKMARK_IS_TYPE(flags, TAG) && id != TAGS_ROOT_ID) {
        rctx.flags &= ~BOOKMARKFS_BOOKMARK_TYPE(TAG);
    }
    int status = BACKEND_CALL(bookmark_list, id, off, flags, bm_readdir_cb,
            &rctx, &cookie);
    if (status < 0) {
        return status;
    }

    return send_reply(buf, req, ctx.buf, rctx.reply_len);
}

static int
bm_readdir_cb (
    void                                   *user_data,
    struct bookmarkfs_bookmark_entry const *entry
) {
    struct bm_readdir_ctx *rctx = user_data;

    int subsys_type = SUBSYS_TYPE_BOOKMARK;
    if (BOOKMARKFS_BOOKMARK_IS_TYPE(rctx->flags, TAG)) {
        subsys_type = SUBSYS_TYPE_TAG;
    }
    bool is_readdirplus = rctx->flags & BOOKMARK_FLAG(LIST_WITHSTAT);
    struct fuse_entry_param ep = STRUCT_FUSE_ENTRY_PARAM_INIT;
    bm_fillstat(&entry->stat, subsys_type, is_readdirplus, &ep.attr);
    ep.ino = ep.attr.st_ino;

    size_t entry_size;
    char   *buf     = ctx.buf       + rctx->reply_len;
    size_t  buf_len = rctx->buf_len - rctx->reply_len;
    if (is_readdirplus) {
        entry_size = fuse_add_direntry_plus(rctx->req, buf, buf_len,
                entry->name, &ep, entry->off);
    } else {
        entry_size = fuse_add_direntry(rctx->req, buf, buf_len, entry->name,
                &ep.attr, entry->off);
    }
    if (entry_size > buf_len) {
        return 1;
    }
    rctx->reply_len += entry_size;
    return 0;
}

static int
bm_rename (
    fuse_ino_t  parent,
    char const *name,
    fuse_ino_t  new_parent,
    char const *new_name,
    uint32_t    flags
) {
    return BACKEND_CALL(bookmark_rename, INODE_SUBSYS_ID(parent), name,
            INODE_SUBSYS_ID(new_parent), new_name, flags);
}

static int
bm_rmxattr (
    fuse_ino_t  UNUSED_VAR(ino),
    char const *name
) {
    if (!is_xattr_name(name)) {
        return -ENOATTR;
    }
    return -EPERM;
}

static int
bm_set_keyword (
    fuse_ino_t   ino,
    char const  *keyword,
    struct stat *stat_buf
) {
    struct bookmarkfs_bookmark_stat stat;
    stat.id = INODE_SUBSYS_ID(ino);
    int status = BACKEND_CALL(bookmark_create, 0, keyword,
            BOOKMARKFS_BOOKMARK_TYPE(KEYWORD), &stat);
    if (status < 0) {
        return status;
    }

    bm_fillstat(&stat, SUBSYS_TYPE_BOOKMARK, true, stat_buf);
    return 0;
}

static int
bm_set_tag (
    fuse_ino_t   ino,
    fuse_ino_t   new_parent,
    char const  *new_name,
    struct stat *stat_buf
) {
    struct bookmarkfs_bookmark_stat stat;
    stat.id = INODE_SUBSYS_ID(ino);
    int status = BACKEND_CALL(bookmark_create, INODE_SUBSYS_ID(new_parent),
            new_name, BOOKMARKFS_BOOKMARK_TYPE(TAG), &stat);
    if (status < 0) {
        return status;
    }

    bm_fillstat(&stat, SUBSYS_TYPE_BOOKMARK, true, stat_buf);
    return 0;
}

static int
bm_setattr (
    fuse_ino_t                   ino,
    int                          mask,
    bool                         is_tag,
    struct fuse_file_info const *fi,
    struct stat                 *stat_buf
) {
#define FUSE_SET_ATTR_NAME_(name)  FUSE_SET_ATTR_##name
#define TO_SET(...)  BITWISE_OR(FUSE_SET_ATTR_NAME_, __VA_ARGS__)

    int flags_unsupported = TO_SET(MODE, UID, GID);
    if (mask & flags_unsupported) {
        return -EPERM;
    }

    if (mask & TO_SET(SIZE)) {
        debug_assert(!is_tag);

        struct fs_file_handle *fh = uint2ptr(fi->fh);
        debug_assert(fh == bm_fh_get(ino, NULL, NULL));

        size_t new_len = stat_buf->st_size;
        if (new_len > ctx.file_max) {
            return -EFBIG;
        }
        if (fh->data_len < new_len) {
            if (fh->buf_len < new_len) {
                fh->buf_len = new_len + FH_BUF_RESERVED(new_len);
                fh->buf     = xrealloc(fh->buf, fh->buf_len);
            }
            memset(fh->buf + fh->data_len, 0, new_len - fh->data_len);
        }
        fh->data_len = new_len;
        xgetrealtime(&fh->mtime);
        fh->flags |= FH_FLAG_DIRTY;
    }

    if (mask & (TO_SET(ATIME, MTIME))) {
        if (ctx.flags.ctime) {
            mask |= TO_SET(MTIME, MTIME_NOW);
        }

        struct timespec now = { 0 };
        if (mask & TO_SET(ATIME_NOW, MTIME_NOW)) {
            xgetrealtime(&now);
        }

        uint32_t set_flags = 0;
        if (is_tag) {
            set_flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        }
        struct timespec times[] = { now, now };
        if (mask & TO_SET(ATIME)) {
            if (!(mask & TO_SET(ATIME_NOW))) {
                times[0] = stat_buf->st_atim;
            }
            set_flags |= BOOKMARK_FLAG(SET_ATIME);
        }
        if (mask & TO_SET(MTIME)) {
            if (!(mask & TO_SET(MTIME_NOW))) {
                times[1] = stat_buf->st_mtim;
            }
            set_flags |= BOOKMARK_FLAG(SET_MTIME);
        }
        int status = BACKEND_CALL(bookmark_set, INODE_SUBSYS_ID(ino), NULL,
                set_flags, times, 0);
        if (status < 0) {
            return status;
        }
    }
#undef TO_SET

    uint32_t lookup_flags = 0;
    if (is_tag) {
        lookup_flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
    }
    *stat_buf = (struct stat) STRUCT_STAT_INIT;
    return bm_lookup(ino, NULL, lookup_flags, stat_buf);
}

static int
bm_setxattr (
    fuse_ino_t  ino,
    char const *name,
    void const *val,
    size_t      val_len,
    int         flags
) {
    if (!is_xattr_name(name)) {
        return -ENOATTR;
    }
#ifdef __linux__
    if (flags == XATTR_CREATE) {
        return -EEXIST;
    }
#else
    debug_assert(flags == 0);
#endif

    name += strlen(BOOKMARKFS_XATTR_PREFIX);
    return BACKEND_CALL(bookmark_set, INODE_SUBSYS_ID(ino), name, 0, val,
            val_len);
}

static int
bm_write (
    fuse_req_t             req,
    int                    flags,
    char const            *req_buf,
    size_t                 req_buf_len,
    off_t                  off,
    struct fs_file_handle *fh
) {
    if (req_buf_len == 0) {
        return 0;
    }

    if (flags & O_APPEND) {
        off = fh->data_len;
    }
    size_t req_off_max = req_buf_len + off;
    if (req_off_max > ctx.file_max) {
        return -EFBIG;
    }
    char *buf = fh->buf;
    if (req_off_max > fh->buf_len) {
        fh->buf_len = req_off_max + FH_BUF_RESERVED(req_off_max);
        buf         = xrealloc(buf, fh->buf_len);
        fh->buf     = buf;
    }
    memcpy(buf + off, req_buf, req_buf_len);

    off_t hole_len = off - fh->data_len;
    if (hole_len > 0) {
        memset(buf + fh->data_len, 0, hole_len);
    }
    if (req_off_max > fh->data_len) {
        fh->data_len = req_off_max;
    }

    xgetrealtime(&fh->mtime);
    // We're tempted to free the cookie here, however,
    // that would make a read following a writeback always realloc the buffer.
    fh->flags |= FH_FLAG_DIRTY;

    return send_reply(write, req, req_buf_len);
}

static void
do_delete (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name,
    uint32_t    flags
) {
    int status = -EIO;

    switch (INODE_SUBSYS_TYPE(parent)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_delete(parent, name, flags);
        break;

      case SUBSYS_TYPE_TAG:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        // fallthrough
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_delete(parent, name, flags);
        break;
    }

    send_reply(err, req, -status);
}

static void
do_readdir (
    fuse_req_t                   req,
    fuse_ino_t                   ino,
    size_t                       buf_max,
    off_t                        off,
    uint32_t                     flags,
    struct fuse_file_info const *fi
) {
    debug_assert(off >= 0);
    int status = -EIO;

    if (unlikely(buf_max > ctx.buf_len)) {
        // Most likely that a process has just called getdirentries()
        // with a custom buffer size.  Should not happen on Linux.
        debug_printf("bm_readdir(): reply_buf_max > page_size (%zu > %zu)",
                buf_max, ctx.buf_len);
        buf_max = ctx.buf_len;
    }

    void *cookie = uint2ptr(fi->fh);
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_readdir(req, ino, buf_max, off, flags, cookie);
        break;

      case SUBSYS_TYPE_TAG:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        // fallthrough
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_readdir(req, ino, buf_max, off, flags, cookie);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }
}

static unsigned long
fh_entry_hash (
    void const *entry
) {
    struct fs_file_handle const *fh = entry;

    return fh->hashcode;
}

static int
fh_entry_comp (
    union hashmap_key  key,
    void const        *entry
) {
    struct fs_file_handle const *fh = entry;

    return key.u64 == fh->ino ? 0 : -1;
}

static bool
has_access (
    fuse_req_t req,
    int        to_check
) {
    struct fuse_ctx const *fctx = fuse_req_ctx(req);

    mode_t req_mode = 0;
    if (to_check & R_OK) {
        req_mode |= (S_IRUSR | S_IRGRP | S_IROTH);
    }
    if (to_check & W_OK) {
        req_mode |= (S_IWUSR | S_IWGRP | S_IWOTH);
    }
    if (to_check & X_OK) {
        req_mode |= (S_IXUSR | S_IXGRP | S_IXOTH);
    }
    if (fctx->uid != ctx.uid) {
        req_mode &= ~(S_IRUSR | S_IWUSR | S_IXUSR);
    }
    if (fctx->gid != ctx.gid) {
        req_mode &= ~(S_IRGRP | S_IWGRP | S_IXGRP);
    }
    return req_mode & ~ctx.flags.accmode;
}

static int
intfs_create (
    fuse_ino_t             parent,
    char const            *name,
    int                    flags,
    struct stat           *stat_buf,
    struct fuse_file_info *fi
) {
    int status = -EPERM;

    switch (parent) {
      case INTFS_ID_BOOKMARKS:
        status = bm_create(BOOKMARKS_ROOT_ID, name, flags, stat_buf);
        if (status < 0) {
            break;
        }
        status = bm_open(SUBSYS_NODEID(stat_buf->st_ino, SUBSYS_TYPE_BOOKMARK),
                fi);
        break;
    }
    return status;
}

static int
intfs_delete (
    fuse_ino_t  parent,
    char const *name,
    uint32_t    flags
) {
    switch (parent) {
      case INTFS_ID_BOOKMARKS:
        parent = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        parent = TAGS_ROOT_ID;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        break;

      case INTFS_ID_KEYWORDS:
        parent = 0;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(KEYWORD);
        break;

      default:
        return -EPERM;
    }
    return bm_delete(parent, name, flags);
}

static int
intfs_freedir (
    fuse_ino_t  ino,
    void       *cookie
) {
    switch (ino) {
      case INTFS_ID_BOOKMARKS:
        ino = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        ino = TAGS_ROOT_ID;
        break;

      case INTFS_ID_KEYWORDS:
        ino = 0;
        break;

      default:
        return 0;
    }
    return bm_freedir(ino, cookie);
}

static int
intfs_ioctl (
    fuse_req_t  req,
    fuse_ino_t  ino,
    unsigned    cmd,
    void const *ibuf,
    size_t      ibuf_len,
    size_t      obuf_len,
    void       *cookie
) {
    uint32_t flags = 0;
    switch (ino) {
      case INTFS_ID_BOOKMARKS:
        ino = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        ino = TAGS_ROOT_ID;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        break;

      case INTFS_ID_KEYWORDS:
        ino = 0;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(KEYWORD);
        break;

      default:
        return -ENOTTY;
    }
    return bm_ioctl(req, ino, cmd, flags, ibuf, ibuf_len, obuf_len, cookie);
}

static int
intfs_link (
    fuse_ino_t   ino,
    fuse_ino_t   new_parent,
    char const  *new_name,
    struct stat *stat_buf
) {
    int status = -EPERM;

    switch (new_parent) {
      case INTFS_ID_KEYWORDS:
        status = bm_set_keyword(ino, new_name, stat_buf);
        break;
    }
    return status;
}

static int
intfs_lookup (
    fuse_ino_t   parent,
    char const  *name,
    struct stat *stat_buf
) {
    int status = -ENOENT;

    switch (parent) {
      case INTFS_ID_ROOT:
        if (name == NULL) {
            stat_buf->st_mode = FS_FILEMODE_ROOT;
            status = 0;
        }
#define INTFS_ENTRY(which, id, flags, cond)                 \
        else if (0 == strcmp(name, INTFS_NAME_##which)) {   \
            parent = INTFS_ID_##which;                      \
            name   = NULL;                                  \
            /* fallthrough */                               \
      case INTFS_ID_##which:                                \
            if (!(cond)) {                                  \
                break;                                      \
            }                                               \
            status = bm_lookup(id, name, flags, stat_buf);  \
        }
        INTFS_ENTRY(BOOKMARKS, BOOKMARKS_ROOT_ID, 0, true)
        INTFS_ENTRY(TAGS, TAGS_ROOT_ID, BOOKMARKFS_BOOKMARK_TYPE(TAG),
                backend_has_tags())
        INTFS_ENTRY(KEYWORDS, 0, BOOKMARKFS_BOOKMARK_TYPE(KEYWORD),
                backend_has_keywords())
#undef INTFS_ENTRY
        break;
    }
    if (status < 0) {
        return status;
    }
    if (name == NULL) {
        stat_buf->st_ino = parent;
    }
    return 0;
}

static int
intfs_lsxattr (
    fuse_ino_t  ino,
    char       *buf,
    size_t      buf_max,
    size_t     *buf_len_ptr
) {
    int status = 0;

    switch (ino) {
      case INTFS_ID_BOOKMARKS:
        status = bm_lsxattr(BOOKMARKS_ROOT_ID, buf, buf_max, buf_len_ptr);
        break;
    }
    return status;
}

static int
intfs_mkdir (
    fuse_ino_t   parent,
    char const  *name,
    struct stat *stat_buf
) {
    int subsys_type = SUBSYS_TYPE_BOOKMARK;
    switch (parent) {
      case INTFS_ID_BOOKMARKS:
        parent = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        parent      = TAGS_ROOT_ID;
        subsys_type = SUBSYS_TYPE_TAG;
        break;

      default:
        return -EACCES;
    }
    return bm_mkdir(parent, name, subsys_type, stat_buf);
}

static int
intfs_opendir (
    fuse_ino_t             ino,
    struct fuse_file_info *fi
) {
    uint32_t flags = 0;
    switch (ino) {
      case INTFS_ID_ROOT:
        fi->cache_readdir = 1;
        fi->keep_cache    = 1;
        return 0;

      case INTFS_ID_BOOKMARKS:
        ino = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        ino = TAGS_ROOT_ID;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        break;

      case INTFS_ID_KEYWORDS:
        ino = 0;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(KEYWORD);
        break;
    }
    return bm_opendir(ino, flags, fi);
}

static int
intfs_readdir (
    fuse_req_t  req,
    fuse_ino_t  ino,
    size_t      buf_max,
    off_t       off,
    uint32_t    flags,
    void       *cookie
) {
    switch (ino) {
      case INTFS_ID_BOOKMARKS:
        return bm_readdir(req, BOOKMARKS_ROOT_ID, buf_max, off, flags, cookie);

      case INTFS_ID_TAGS:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        return bm_readdir(req, TAGS_ROOT_ID, buf_max, off, flags, cookie);

      case INTFS_ID_KEYWORDS:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(KEYWORD);
        return bm_readdir(req, 0, buf_max, off, flags, cookie);
    }
    debug_assert(ino == INTFS_ID_ROOT);

    bool    is_readdirplus = flags & BOOKMARK_FLAG(LIST_WITHSTAT);
    char   *reply_buf      = ctx.buf;
    size_t  reply_size     = 0;

#define INTFS_DIRENT(which, cond)  \
    { (cond) ? INTFS_NAME_##which : NULL, INTFS_ID_##which, false }
    struct intfs_dirent {
        char const *name;
        unsigned    id;
        bool        is_root;
    } const dents[] = {
        INTFS_DIRENT(BOOKMARKS, true),
        INTFS_DIRENT(TAGS,      backend_has_tags()),
        INTFS_DIRENT(KEYWORDS,  backend_has_keywords()),
    };
    size_t dents_cnt = sizeof(dents) / sizeof(struct intfs_dirent);

    for (size_t idx = off; idx < dents_cnt; ++idx) {
        struct intfs_dirent const *dent = dents + idx;
        if (dent->name == NULL) {
            continue;
        }

        struct fuse_entry_param ep = STRUCT_FUSE_ENTRY_PARAM_INIT;
        if (dent->is_root) {
            ep.attr.st_mode = FS_FILEMODE_ROOT;
        } else {
            if (is_readdirplus) {
                int status = intfs_lookup(dent->id, NULL, &ep.attr);
                if (status < 0) {
                    return status;
                }
            } else {
                ep.attr.st_mode = FS_FILEMODE_DIR;
            }
        }
        ep.ino         = dent->id;
        ep.attr.st_ino = dent->id;

        size_t entry_size;
        if (is_readdirplus) {
            entry_size = fuse_add_direntry_plus(req, reply_buf, buf_max,
                    dent->name, &ep, idx + 1);
        } else {
            entry_size = fuse_add_direntry(req, reply_buf, buf_max,
                    dent->name, &ep.attr, idx + 1);
        }
        if (entry_size > buf_max) {
            break;
        }
        reply_buf  += entry_size;
        buf_max    -= entry_size;
        reply_size += entry_size;
    }
    return send_reply(buf, req, reply_buf - reply_size, reply_size);
}

static int
intfs_rename (
    fuse_ino_t  parent,
    char const *name,
    char const *new_name,
    uint32_t    flags
) {
    switch (parent) {
      case INTFS_ID_BOOKMARKS:
        parent = BOOKMARKS_ROOT_ID;
        break;

      case INTFS_ID_TAGS:
        parent = TAGS_ROOT_ID;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        break;

      case INTFS_ID_KEYWORDS:
        parent = 0;
        flags |= BOOKMARKFS_BOOKMARK_TYPE(KEYWORD);
        break;

      default:
        return -EPERM;
    }
    return bm_rename(parent, name, parent, new_name, flags);
}

static bool
is_xattr_name (
    char const *name
) {
    size_t name_len   = strlen(name);
    size_t prefix_len = strlen(BOOKMARKFS_XATTR_PREFIX);
    if (name_len <= prefix_len) {
        return false;
    }
    if (0 != memcmp(name, BOOKMARKFS_XATTR_PREFIX, prefix_len)) {
        return false;
    }
    name     += prefix_len;
    name_len -= prefix_len;

    BM_XATTR_FOREACH(xattr, xattr_len)
        if (name_len == xattr_len && 0 == memcmp(name, xattr, name_len)) {
            return true;
        }
    BM_XATTR_FOREACH_END
    return false;
}

static int
reply_errcheck (
    int         err,
    char const *name,
    int         line
) {
    if (err < 0) {
        log_printf("%d: fuse_reply_%s(): %s", line, name, xstrerror(-err));
    }
    return 0;
}

static int
timespec_cmp (
    struct timespec const *t1,
    struct timespec const *t2
) {
    if (t1->tv_sec < t2->tv_sec) {
        return -1;
    }
    if (t1->tv_sec > t2->tv_sec) {
        return 1;
    }
    if (t1->tv_nsec < t2->tv_nsec) {
        return -1;
    }
    if (t1->tv_nsec > t2->tv_nsec) {
        return 1;
    }
    return 0;
}

void
fs_init_backend (
    struct bookmarkfs_backend const *backend_impl,
    void                            *backend_ctx
) {
    ctx.backend_impl = backend_impl;
    ctx.backend_ctx  = backend_ctx;
}

void
fs_init_fuse (
    struct fuse_session *session
) {
    ctx.session = session;
}

void
fs_init_metadata (
    uint64_t    bookmarks_root_id,
    uint64_t    tags_root_id,
    char const *xattr_names
) {
    if (bookmarks_root_id != UINT64_MAX) {
        BOOKMARKS_ROOT_ID = bookmarks_root_id;
    }
    if (tags_root_id != UINT64_MAX) {
        TAGS_ROOT_ID = tags_root_id;
    }
    if (xattr_names != NULL) {
        ctx.xattr_names = xattr_names;
    }
}

void
fs_init_opts (
    struct fs_flags flags,
    size_t          file_max
) {
    ctx.flags    = flags;
    ctx.file_max = file_max;
}

void
fs_op_create (
    fuse_req_t             req,
    fuse_ino_t             parent,
    char const            *name,
    mode_t                 UNUSED_VAR(mode),
    struct fuse_file_info *fi
) {
    int status = -EPERM;

    struct fuse_entry_param ep = STRUCT_FUSE_ENTRY_PARAM_INIT;
    switch (INODE_SUBSYS_TYPE(parent)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_create(parent, name, fi->flags, &ep.attr, fi);
        break;

      case SUBSYS_TYPE_BOOKMARK:
        status = bm_create(parent, name, fi->flags, &ep.attr);
        if (status < 0) {
            break;
        }
        status = bm_open(ep.attr.st_ino, fi);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    ep.ino = ep.attr.st_ino;
    send_reply(create, req, &ep, fi);
}

void
fs_op_destroy (
    void *UNUSED_VAR(user_data)
) {
    hashmap_destroy(ctx.fh_map);
    free(ctx.buf);
}

void
fs_op_fsync (
    fuse_req_t             req,
    fuse_ino_t             ino,
    int                    UNUSED_VAR(data_sync),
    struct fuse_file_info *fi
) {
    int status = 0;
    if (ctx.flags.readonly) {
        goto end;
    }

    struct fs_file_handle *fh = uint2ptr(fi->fh);
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_do_write(ino, fh);
        break;
    }
    if (status < 0) {
        goto end;
    }

    status = ctx.backend_impl->bookmark_sync(ctx.backend_ctx);

  end:
    send_reply(err, req, -status);
}

void
fs_op_fsyncdir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    int                    UNUSED_VAR(data_sync),
    struct fuse_file_info *UNUSED_VAR(fi)
) {
    int status = 0;
    if (ctx.flags.readonly) {
        goto end;
    }

    switch (INODE_SUBSYS_TYPE(ino)) {
#ifndef __FreeBSD__
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_syncdir(ino);
        break;

      case SUBSYS_TYPE_BOOKMARK:
        status = bm_syncdir(ino);
        break;
#endif
    }
    if (status < 0) {
        goto end;
    }

    status = ctx.backend_impl->bookmark_sync(ctx.backend_ctx);

  end:
    send_reply(err, req, -status);
}

void
fs_op_getattr (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *UNUSED_VAR(fi)
) {
    int status = -EIO;

    uint32_t flags = 0;
    struct stat stat_buf = STRUCT_STAT_INIT;
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_lookup(ino, NULL, &stat_buf);
        break;

      case SUBSYS_TYPE_TAG:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        // fallthrough
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_lookup(ino, NULL, flags, &stat_buf);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    send_reply(attr, req, &stat_buf, FS_ENTRY_TIMEOUT);
}

void
fs_op_getxattr (
    fuse_req_t  req,
    fuse_ino_t  ino,
    char const *name,
    size_t      buf_max
) {
    int status = -ENOATTR;

    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_getxattr(req, ino, name, buf_max);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }
}

void
fs_op_init (
    void                  *UNUSED_VAR(user_data),
    struct fuse_conn_info *UNUSED_VAR(conn)
) {
    // We're tempted to enable writeback caching in exclusive mode, however,
    // FUSE_NOTIFY_INVAL_INODE does not clear the cached `st_size` attribute,
    // leading to bad file size after the backend fails to set bookmark URL.
#if 0
    if (ctx.flags.exclusive) {
        conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    }
#endif

    ctx.fh_map = hashmap_create(fh_entry_comp, fh_entry_hash);
    ctx.uid = geteuid();
    ctx.gid = getegid();

    ctx.buf_len = sysconf(_SC_PAGE_SIZE);
    xassert(ctx.buf_len >= sizeof(union {
        struct bm_check_ctx          check_ctx;
        struct bookmarkfs_permd_data permd;
    }));
    // The requested buffer size for FUSE_READDIR and FUSE_READDIRPLUS
    // always equals to the page size on Linux.
    //
    // On FreeBSD, it equals to the `nbytes` argument of the underlying
    // getdirentries() - also equals to the page size when called from
    // readdir() (unless it is not a multiple of DIRBLKSIZ).
    ctx.buf = xmalloc(ctx.buf_len);
}

void
fs_op_ioctl (
    fuse_req_t             req,
    fuse_ino_t             ino,
    unsigned               cmd,
    void                  *UNUSED_VAR(arg),
    struct fuse_file_info *fi,
    unsigned               flags,
    void const            *ibuf,
    size_t                 ibuf_len,
    size_t                 obuf_len
) {
    int status = -ENOTTY;
    // Currently all ioctls only apply to directories.
    if (!(flags & FUSE_IOCTL_DIR)) {
        goto fail;
    }

    void *cookie = uint2ptr(fi->fh);
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_ioctl(req, ino, cmd, ibuf, ibuf_len, obuf_len, cookie);
        break;

      case SUBSYS_TYPE_BOOKMARK:
        status = bm_ioctl(req, ino, cmd, 0, ibuf, ibuf_len, obuf_len, cookie);
        break;
    }
    if (status < 0) {
      fail:
        send_reply(err, req, -status);
        return;
    }
}

void
fs_op_link (
    fuse_req_t  req,
    fuse_ino_t  ino,
    fuse_ino_t  new_parent,
    char const *new_name
) {
    int status = -EPERM;
    if (INODE_SUBSYS_TYPE(ino) != SUBSYS_TYPE_BOOKMARK) {
        goto fail;
    }

    struct fuse_entry_param ep = STRUCT_FUSE_ENTRY_PARAM_INIT;
    switch (INODE_SUBSYS_TYPE(new_parent)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_link(ino, new_parent, new_name, &ep.attr);
        break;

      case SUBSYS_TYPE_TAG:
        status = bm_set_tag(ino, new_parent, new_name, &ep.attr);
        break;
    }
    if (status < 0) {
      fail:
        send_reply(err, req, -status);
        return;
    }

    ep.ino = ep.attr.st_ino;
    send_reply(entry, req, &ep);
}

void
fs_op_listxattr (
    fuse_req_t req,
    fuse_ino_t ino,
    size_t     buf_max
) {
    int status = -E2BIG;
    if (unlikely(buf_max > ctx.buf_len)) {
        goto fail;
    }
    status = 0;

    char   *buf     = ctx.buf;
    size_t  buf_len = 0;
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_lsxattr(ino, buf, buf_max, &buf_len);
        break;

      case SUBSYS_TYPE_BOOKMARK:
        status = bm_lsxattr(ino, buf, buf_max, &buf_len);
        break;
    }
    if (status < 0) {
      fail:
        send_reply(err, req, -status);
        return;
    }

    if (buf_max == 0) {
        send_reply(xattr, req, buf_len);
    } else {
        send_reply(buf, req, buf, buf_len);
    }
}

void
fs_op_lookup (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name
) {
    int status = -EIO;

    uint32_t flags = 0;
    struct fuse_entry_param ep = STRUCT_FUSE_ENTRY_PARAM_INIT;
    switch (INODE_SUBSYS_TYPE(parent)) {
      case SUBSYS_TYPE_INTERNAL:
        ep.entry_timeout = FS_ENTRY_TIMEOUT_MAX;
        status = intfs_lookup(parent, name, &ep.attr);
        break;

      case SUBSYS_TYPE_TAG:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        // fallthrough
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_lookup(parent, name, flags, &ep.attr);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    ep.ino = ep.attr.st_ino;
    send_reply(entry, req, &ep);
}

void
fs_op_mkdir (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name,
    mode_t      UNUSED_VAR(mode)
) {
    int status = -EPERM;

    struct fuse_entry_param ep = STRUCT_FUSE_ENTRY_PARAM_INIT;
    switch (INODE_SUBSYS_TYPE(parent)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_mkdir(parent, name, &ep.attr);
        break;

      case SUBSYS_TYPE_BOOKMARK:
        status = bm_mkdir(parent, name, SUBSYS_TYPE_BOOKMARK, &ep.attr);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    ep.ino = ep.attr.st_ino;
    send_reply(entry, req, &ep);
}

void
fs_op_open (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
) {
    int status = -EIO;

    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_open(ino, fi);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    send_reply(open, req, fi);
}

void
fs_op_opendir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
) {
    int status = -EIO;

    uint32_t flags = 0;
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_opendir(ino, fi);
        break;

      case SUBSYS_TYPE_TAG:
        flags |= BOOKMARKFS_BOOKMARK_TYPE(TAG);
        // fallthrough
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_opendir(ino, flags, fi);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    send_reply(open, req, fi);
}

void
fs_op_read (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fuse_file_info *fi
) {
    debug_assert(off >= 0);
    int status = -EIO;

    struct fs_file_handle *fh = uint2ptr(fi->fh);
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_read(req, ino, buf_max, off, fh);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }
}

void
fs_op_readdir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fuse_file_info *fi
) {
    do_readdir(req, ino, buf_max, off, 0, fi);
}

void
fs_op_readdirplus (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fuse_file_info *fi
) {
    do_readdir(req, ino, buf_max, off, BOOKMARK_FLAG(LIST_WITHSTAT), fi);
}

void
fs_op_release (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
) {
    int status = -EIO;

    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_free(ino, fi);
        break;
    }

    send_reply(err, req, -status);
}

void
fs_op_releasedir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
) {
    int status = -EIO;

    void *cookie = uint2ptr(fi->fh);
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_INTERNAL:
        status = intfs_freedir(ino, cookie);
        break;

      case SUBSYS_TYPE_BOOKMARK:
      case SUBSYS_TYPE_TAG:
        status = bm_freedir(ino, cookie);
        break;
    }

    send_reply(err, req, -status);
}

void
fs_op_rename (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name,
    fuse_ino_t  new_parent,
    char const *new_name,
    unsigned    flags
) {
    int status = -EINVAL;

    uint32_t rflags = 0;
#if defined(__linux__) && defined(_GNU_SOURCE)
    if (flags & RENAME_NOREPLACE) {
        rflags |=  BOOKMARKFS_BOOKMARK_RENAME_NOREPLACE;
        flags  &= ~RENAME_NOREPLACE;
    }
#endif
    if (flags != 0) {
        goto end;
    }
    status = -EPERM;

    unsigned subsys_type = INODE_SUBSYS_TYPE(parent);
    if (subsys_type != INODE_SUBSYS_TYPE(new_parent)) {
        goto end;
    }
    switch (subsys_type) {
      case SUBSYS_TYPE_INTERNAL:
        if (parent != new_parent) {
            break;
        }
        status = intfs_rename(parent, name, new_name, rflags);
        break;

      case SUBSYS_TYPE_BOOKMARK:
        status = bm_rename(parent, name, new_parent, new_name, rflags);
        break;
    }

  end:
    send_reply(err, req, -status);
}

void
fs_op_removexattr (
    fuse_req_t  req,
    fuse_ino_t  ino,
    char const *name
) {
    int status = -ENOATTR;

    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_rmxattr(ino, name);
        break;
    }

    send_reply(err, req, -status);
}

void
fs_op_rmdir (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name
) {
    do_delete(req, parent, name, BOOKMARK_FLAG(DELETE_DIR));
}

void
fs_op_setattr (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct stat           *stat_buf,
    int                    mask,
    struct fuse_file_info *fi
) {
    int status = -EPERM;

    bool is_tag = false;
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_TAG:
        is_tag = true;
        // fallthrough
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_setattr(ino, mask, is_tag, fi, stat_buf);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }

    send_reply(attr, req, stat_buf, FS_ENTRY_TIMEOUT);
}

void
fs_op_setxattr (
    fuse_req_t  req,
    fuse_ino_t  ino,
    char const *name,
    char const *val,
    size_t      val_len,
    int         flags
) {
    int status = -ERANGE;
    if (val_len > ctx.file_max) {
        goto end;
    }
    status = -ENOATTR;

    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_setxattr(ino, name, val, val_len, flags);
        break;
    }

  end:
    send_reply(err, req, -status);
}

void
fs_op_unlink (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name
) {
    do_delete(req, parent, name, 0);
}

void
fs_op_write (
    fuse_req_t             req,
    fuse_ino_t             ino,
    char const            *buf,
    size_t                 buf_len,
    off_t                  off,
    struct fuse_file_info *fi
) {
    debug_assert(off >= 0);
    int status = -EIO;

    struct fs_file_handle *fh = uint2ptr(fi->fh);
    switch (INODE_SUBSYS_TYPE(ino)) {
      case SUBSYS_TYPE_BOOKMARK:
        status = bm_write(req, fi->flags, buf, buf_len, off, fh);
        break;
    }
    if (status < 0) {
        send_reply(err, req, -status);
        return;
    }
}
