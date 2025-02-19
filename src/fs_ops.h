/**
 * bookmarkfs/src/fs_ops.h
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

#ifndef BOOKMARKFS_FS_OPS_H_
#define BOOKMARKFS_FS_OPS_H_

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>

#include "backend.h"

struct fs_flags {
    unsigned accmode     : 9;
    unsigned ctime       : 1;
    unsigned eol         : 1;
    unsigned exclusive   : 1;
    unsigned has_keyword : 1;
    unsigned readonly    : 1;
};

void
fs_init_backend (
    struct bookmarkfs_backend const *backend_impl,
    void                            *backend_ctx
);

void
fs_init_fuse (
    struct fuse_session *session
);

void
fs_init_metadata (
    uint64_t    bookmarks_root_id,
    uint64_t    tags_root_id,
    char const *xattr_names
);

void
fs_init_opts (
    struct fs_flags flags,
    size_t          file_max
);

void
fs_op_create (
    fuse_req_t             req,
    fuse_ino_t             parent,
    char const            *name,
    mode_t                 mode,
    struct fuse_file_info *fi
);

void
fs_op_destroy (
    void *user_data
);

void
fs_op_fsync (
    fuse_req_t             req,
    fuse_ino_t             ino,
    int                    data_sync,
    struct fuse_file_info *fi
);

void
fs_op_fsyncdir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    int                    data_sync,
    struct fuse_file_info *fi
);

void
fs_op_getattr (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
);

void
fs_op_getxattr (
    fuse_req_t  req,
    fuse_ino_t  ino,
    char const *name,
    size_t      buf_max
);

void
fs_op_init (
    void                  *user_data,
    struct fuse_conn_info *conn
);

void
fs_op_ioctl (
    fuse_req_t             req,
    fuse_ino_t             ino,
    unsigned               cmd,
    void                  *arg,
    struct fuse_file_info *fi,
    unsigned               flags,
    void const            *ibuf,
    size_t                 ibuf_len,
    size_t                 obuf_len
);

void
fs_op_link (
    fuse_req_t  req,
    fuse_ino_t  ino,
    fuse_ino_t  new_parent,
    char const *new_name
);

void
fs_op_listxattr (
    fuse_req_t req,
    fuse_ino_t ino,
    size_t     buf_max
);

void
fs_op_lookup (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name
);

void
fs_op_mkdir (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name,
    mode_t      mode
);

void
fs_op_open (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
);

void
fs_op_opendir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
);

void
fs_op_read (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fuse_file_info *fi
);

void
fs_op_readdir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fuse_file_info *fi
);

void
fs_op_readdirplus (
    fuse_req_t             req,
    fuse_ino_t             ino,
    size_t                 buf_max,
    off_t                  off,
    struct fuse_file_info *fi
);

void
fs_op_release (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
);

void
fs_op_releasedir (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct fuse_file_info *fi
);

void
fs_op_rename (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name,
    fuse_ino_t  new_parent,
    char const *new_name,
    unsigned    flags
);

void
fs_op_removexattr (
    fuse_req_t  req,
    fuse_ino_t  ino,
    char const *name
);

void
fs_op_rmdir (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name
);

void
fs_op_setattr (
    fuse_req_t             req,
    fuse_ino_t             ino,
    struct stat           *attr,
    int                    attr_mask,
    struct fuse_file_info *fi
);

void
fs_op_setxattr (
    fuse_req_t  req,
    fuse_ino_t  ino,
    char const *name,
    char const *val,
    size_t      val_len,
    int         flags
);

void
fs_op_unlink (
    fuse_req_t  req,
    fuse_ino_t  parent,
    char const *name
);

void
fs_op_write (
    fuse_req_t             req,
    fuse_ino_t             ino,
    char const            *buf,
    size_t                 buf_len,
    off_t                  off,
    struct fuse_file_info *fi
);

#endif  /* !defined(BOOKMARKFS_FS_OPS_H_) */
