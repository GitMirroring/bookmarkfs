/**
 * bookmarkfs/src/fsck_ops.h
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

#ifndef BOOKMARKFS_FSCK_OPS_H_
#define BOOKMARKFS_FSCK_OPS_H_

#include <stdint.h>

#include "backend.h"
#include "fsck_handler.h"

// init() flags
#define BOOKMARKFS_FSCK_OP_RECURSIVE  ( 1u << 16 )

typedef int (bookmarkfs_fsck_apply_func) (
    void                                 *fsck_ctx,
    struct bookmarkfs_fsck_handler_entry *entry
);

typedef int (bookmarkfs_fsck_control_func) (
    void *fsck_ctx,
    int   control
);

typedef int (bookmarkfs_fsck_create_func) (
    struct bookmarkfs_backend const  *backend,
    char                             *path,
    struct bookmarkfs_conf_opt       *opts,
    uint32_t                          flags,
    void                            **fsck_ctx_ptr
);

typedef void (bookmarkfs_fsck_destroy_func) (
    void *fsck_ctx
);

typedef void (bookmarkfs_fsck_info_func) (
    struct bookmarkfs_backend const *backend,
    uint32_t                         flags
);

typedef int (bookmarkfs_fsck_next_func) (
    void                                 *fsck_ctx,
    struct bookmarkfs_fsck_handler_entry *entry
);

typedef int (bookmarkfs_fsck_sandbox_func) (
    void *fsck_ctx
);

struct bookmarkfs_fsck_ops {
    bookmarkfs_fsck_info_func *info;

    bookmarkfs_fsck_create_func  *create;
    bookmarkfs_fsck_sandbox_func *sandbox;
    bookmarkfs_fsck_destroy_func *destroy;

    bookmarkfs_fsck_next_func    *next;
    bookmarkfs_fsck_control_func *control;
    bookmarkfs_fsck_apply_func   *apply;
};

extern struct bookmarkfs_fsck_ops const fsck_offline_ops;
extern struct bookmarkfs_fsck_ops const fsck_online_ops;

#endif  /* !defined(BOOKMARKFS_FSCK_OPS_H_) */
