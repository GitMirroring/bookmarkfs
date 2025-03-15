/**
 * bookmarkfs/src/xattr.h
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
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

#ifndef BOOKMARKFS_XATTR_H_
#define BOOKMARKFS_XATTR_H_

#include <stddef.h>

typedef int (bookmarkfs_xattr_cb) (
    void   *user_data,
    void   *buf,
    size_t  buf_len
);

int
bookmarkfs_xattr_get (
    int                  fd,
    char const          *name,
    bookmarkfs_xattr_cb *callback,
    void                *user_data
);

int
bookmarkfs_xattr_list (
    int                  fd,
    bookmarkfs_xattr_cb *callback,
    void                *user_data
);

int
bookmarkfs_xattr_open (
    char const *path
);

int
bookmarkfs_xattr_set (
    int         fd,
    char const *name,
    void const *buf,
    size_t      buf_len
);

#endif  /* !defined(BOOKMARKFS_XATTR_H_) */
