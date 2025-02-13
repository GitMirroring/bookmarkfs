/**
 * bookmarkfs/src/backend.h
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

#ifndef BOOKMARKFS_BACKEND_H_
#define BOOKMARKFS_BACKEND_H_

#include <stdint.h>
#include <time.h>

#include <sys/types.h>

#ifdef BUILDING_BOOKMARKFS
#  include "common.h"
#else
#  include <bookmarkfs/common.h>
#endif

// backend_create() flags
#define BOOKMARKFS_BACKEND_READONLY     ( 1u << 0 )
#define BOOKMARKFS_BACKEND_CTIME        ( 1u << 1 )
#define BOOKMARKFS_BACKEND_NO_SANDBOX   ( 1u << 2 )
#define BOOKMARKFS_BACKEND_NO_LANDLOCK  ( 1u << 3 )
#define BOOKMARKFS_BACKEND_FSCK_ONLY    ( 1u << 4 )

#define BOOKMARKFS_BACKEND_INFO_HELP     ( 1u << 0 )
#define BOOKMARKFS_BACKEND_INFO_VERSION  ( 1u << 1 )

#define BOOKMARKFS_BACKEND_MKFS_FORCE  ( 1u << 0 )

// backend_create() response flags
#define BOOKMARKFS_BACKEND_EXCLUSIVE    ( 1u << 0 )
#define BOOKMARKFS_BACKEND_HAS_KEYWORD  ( 1u << 1 )

// backend_init() flags
#define BOOKMARKFS_BACKEND_LIB_READY  ( 1u << 0 )

#define BOOKMARKFS_FRONTEND_FSCK   ( 1u << 16 )
#define BOOKMARKFS_FRONTEND_MOUNT  ( 1u << 17 )
#define BOOKMARKFS_FRONTEND_MKFS   ( 1u << 18 )

#define BOOKMARKFS_BOOKMARK_CREATE_DIR        ( 1u << 0 )
#define BOOKMARKFS_BOOKMARK_DELETE_DIR        ( 1u << 0 )
#define BOOKMARKFS_BOOKMARK_LIST_WITHSTAT     ( 1u << 0 )
#define BOOKMARKFS_BOOKMARK_RENAME_NOREPLACE  ( 1u << 0 )
#define BOOKMARKFS_BOOKMARK_SET_TIME          ( 1u << 0 )

#define BOOKMARKFS_BOOKMARK_TYPE_BITS   3
#define BOOKMARKFS_BOOKMARK_TYPE_SHIFT  ( 32 - BOOKMARKFS_BOOKMARK_TYPE_BITS )
#define BOOKMARKFS_BOOKMARK_TYPE_MASK   \
    ( ((UINT32_C(1) << BOOKMARKFS_BOOKMARK_TYPE_BITS) - 1)  \
        << BOOKMARKFS_BOOKMARK_TYPE_SHIFT )
#define BOOKMARKFS_BOOKMARK_TYPE(t)  \
    ( BOOKMARKFS_BOOKMARK_TYPE_##t << BOOKMARKFS_BOOKMARK_TYPE_SHIFT )
#define BOOKMARKFS_BOOKMARK_IS_TYPE(f, t)  \
    ( ((f) & BOOKMARKFS_BOOKMARK_TYPE_MASK) == BOOKMARKFS_BOOKMARK_TYPE(t) )

#define BOOKMARKFS_MAX_ID  \
    ( (UINT64_C(1) << (64 - BOOKMARKFS_BOOKMARK_TYPE_BITS)) - 1 )

enum bookmarkfs_bookmark_type {
    BOOKMARKFS_BOOKMARK_TYPE_BOOKMARK = 0,  // must be 0
    BOOKMARKFS_BOOKMARK_TYPE_TAG,
    BOOKMARKFS_BOOKMARK_TYPE_KEYWORD,
};

enum bookmarkfs_cookie_type {
    BOOKMARKFS_COOKIE_TYPE_WATCH,
    BOOKMARKFS_COOKIE_TYPE_LIST,
};

struct bookmarkfs_backend_conf;
struct bookmarkfs_backend_create_resp;
struct bookmarkfs_bookmark_entry;
struct bookmarkfs_bookmark_stat;

typedef int (bookmarkfs_backend_create_func) (
    struct bookmarkfs_backend_conf const  *conf,
    struct bookmarkfs_backend_create_resp *resp
);

typedef void (bookmarkfs_backend_destroy_func) (
    void *backend_ctx
);

typedef void (bookmarkfs_backend_info_func) (
    uint32_t flags
);

typedef int (bookmarkfs_backend_init_func) (
    uint32_t flags
);

typedef int (bookmarkfs_backend_mkfs_func) (
    struct bookmarkfs_backend_conf const *conf
);

typedef int (bookmarkfs_backend_sandbox_func) (
    void                                  *backend_ctx,
    struct bookmarkfs_backend_create_resp *resp
);

typedef int (bookmarkfs_bookmark_check_cb) (
    void       *user_data,
    int         result,
    uint64_t    id,
    uint64_t    extra,
    char const *name
);

typedef int (bookmarkfs_bookmark_check_func) (
    void                               *backend_ctx,
    uint64_t                            parent_id,
    struct bookmarkfs_fsck_data const  *fsck_data,
    uint32_t                            flags,
    bookmarkfs_bookmark_check_cb       *callback,
    void                               *user_data,
    void                              **cookie_ptr
);

typedef int (bookmarkfs_bookmark_create_func) (
    void                            *backend_ctx,
    uint64_t                         parent_id,
    char const                      *name,
    uint32_t                         flags,
    struct bookmarkfs_bookmark_stat *stat_buf
);

typedef int (bookmarkfs_bookmark_delete_func) (
    void       *backend_ctx,
    uint64_t    parent_id,
    char const *name,
    uint32_t    flags
);

typedef int (bookmarkfs_bookmark_get_cb) (
    void       *user_data,
    void const *value,
    size_t      value_len
);

typedef int (bookmarkfs_bookmark_get_func) (
    void                        *backend_ctx,
    uint64_t                     id,
    char const                  *attr_key,
    bookmarkfs_bookmark_get_cb  *callback,
    void                        *user_data,
    void                       **cookie_ptr
);

typedef int (bookmarkfs_bookmark_list_cb) (
    void                                   *user_data,
    struct bookmarkfs_bookmark_entry const *entry
);

typedef int (bookmarkfs_bookmark_list_func) (
    void                         *backend_ctx,
    uint64_t                      id,
    off_t                         off,
    uint32_t                      flags,
    bookmarkfs_bookmark_list_cb  *callback,
    void                         *user_data,
    void                        **cookie_ptr
);

typedef int (bookmarkfs_bookmark_lookup_func) (
    void                            *backend_ctx,
    uint64_t                         id,
    char const                      *name,
    uint32_t                         flags,
    struct bookmarkfs_bookmark_stat *stat_buf
);

typedef int (bookmarkfs_bookmark_permute_func) (
    void                     *backend_ctx,
    uint64_t                  parent_id,
    enum bookmarkfs_permd_op  op,
    char const               *name1,
    char const               *name2,
    uint32_t                  flags
);

typedef int (bookmarkfs_bookmark_rename_func) (
    void       *backend_ctx,
    uint64_t    old_parent_id,
    char const *old_name,
    uint64_t    new_parent_id,
    char const *new_name,
    uint32_t    flags
);

typedef int (bookmarkfs_bookmark_set_func) (
    void       *backend_ctx,
    uint64_t    id,
    char const *attr_key,
    uint32_t    flags,
    void const *val,
    size_t      val_len
);

typedef int (bookmarkfs_bookmark_sync_func) (
    void *backend_ctx
);

typedef void (bookmarkfs_cookie_free_func) (
    void                        *backend_ctx,
    void                        *cookie,
    enum bookmarkfs_cookie_type  cookie_type
);

struct bookmarkfs_backend {
    bookmarkfs_backend_create_func  *backend_create;
    bookmarkfs_backend_destroy_func *backend_destroy;
    bookmarkfs_backend_info_func    *backend_info;
    bookmarkfs_backend_init_func    *backend_init;
    bookmarkfs_backend_mkfs_func    *backend_mkfs;
    bookmarkfs_backend_sandbox_func *backend_sandbox;

    bookmarkfs_bookmark_check_func  *bookmark_check;
    bookmarkfs_bookmark_get_func    *bookmark_get;
    bookmarkfs_bookmark_list_func   *bookmark_list;
    bookmarkfs_bookmark_lookup_func *bookmark_lookup;

    bookmarkfs_bookmark_create_func  *bookmark_create;
    bookmarkfs_bookmark_delete_func  *bookmark_delete;
    bookmarkfs_bookmark_permute_func *bookmark_permute;
    bookmarkfs_bookmark_rename_func  *bookmark_rename;
    bookmarkfs_bookmark_set_func     *bookmark_set;
    bookmarkfs_bookmark_sync_func    *bookmark_sync;

    bookmarkfs_cookie_free_func *cookie_free;
};

struct bookmarkfs_backend_conf {
    uint32_t  version;
    uint32_t  flags;
    char     *store_path;

    struct bookmarkfs_conf_opt *opts;
};

struct bookmarkfs_backend_create_resp {
    char const *name;
    void       *backend_ctx;
    uint64_t    bookmarks_root_id;
    uint64_t    tags_root_id;
    char const *bookmark_attrs;
    uint32_t    flags;
};

struct bookmarkfs_bookmark_stat {
    uint64_t id;
    ssize_t  value_len;

    struct timespec atime;
    struct timespec mtime;
};

struct bookmarkfs_bookmark_entry {
    char const *name;
    off_t       off;

    struct bookmarkfs_bookmark_stat stat;
};

#endif  /* !defined(BOOKMARKFS_BACKEND_H_) */
