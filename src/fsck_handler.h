/**
 * bookmarkfs/src/fsck_handler.h
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

#ifndef BOOKMARKFS_FSCK_HANDLER_H_
#define BOOKMARKFS_FSCK_HANDLER_H_

#include <stdint.h>

#ifdef BUILDING_BOOKMARKFS
#  include "common.h"
#else
#  include <bookmarkfs/common.h>
#endif

// init() flags
#define BOOKMARKFS_FSCK_HANDLER_INTERACTIVE  ( 1u << 0 )
#define BOOKMARKFS_FSCK_HANDLER_READONLY     ( 1u << 1 )

#define BOOKMARKFS_FSCK_HANDLER_INFO_HELP     ( 1u << 0 )
#define BOOKMARKFS_FSCK_HANDLER_INFO_VERSION  ( 1u << 1 )

enum {
    BOOKMARKFS_FSCK_NEXT,
    BOOKMARKFS_FSCK_APPLY,
    BOOKMARKFS_FSCK_USER_INPUT,
    BOOKMARKFS_FSCK_SAVE,
    BOOKMARKFS_FSCK_STOP,
    BOOKMARKFS_FSCK_REWIND,
    BOOKMARKFS_FSCK_SKIP,
    BOOKMARKFS_FSCK_SKIP_CHILDREN,
    BOOKMARKFS_FSCK_RESET,
};

union bookmarkfs_fsck_handler_data;

typedef int (bookmarkfs_fsck_handler_create_func) (
    struct bookmarkfs_conf_opt const  *opts,
    uint32_t                           flags,
    void                             **handler_ctx_ptr
);

typedef void (bookmarkfs_fsck_handler_destroy_func) (
    void *handler_ctx
);

typedef void (bookmarkfs_fsck_handler_info_func) (
    uint32_t flags
);

typedef int (bookmarkfs_fsck_handler_run_func) (
    void                               *handler_ctx,
    int                                 why,
    union bookmarkfs_fsck_handler_data *data
);

struct bookmarkfs_fsck_handler {
    bookmarkfs_fsck_handler_info_func *info;

    bookmarkfs_fsck_handler_create_func  *create;
    bookmarkfs_fsck_handler_destroy_func *destroy;

    bookmarkfs_fsck_handler_run_func *run;
};

struct bookmarkfs_fsck_handler_entry {
    uint64_t parent_id;

    struct bookmarkfs_fsck_data data;
};

union bookmarkfs_fsck_handler_data {
    struct bookmarkfs_fsck_handler_entry entry;
    char *str;
};

#endif  /* !defined(BOOKMARKFS_FSCK_HANDLER_H_) */
