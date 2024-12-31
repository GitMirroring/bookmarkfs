/**
 * bookmarkfs/src/fsck_util.h
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

#ifndef BOOKMARKFS_FSCK_UTIL_H_
#define BOOKMARKFS_FSCK_UTIL_H_

#include <stddef.h>

#include "ioctl.h"

int
explain_fsck_result (
    enum bookmarkfs_fsck_result        result,
    struct bookmarkfs_fsck_data const *data
);

/**
 * Copy a NUL-terminated string from `src` to `dst`,
 * replacing all ASCII control characters to `ch`.
 *
 * Returns the number of characters replaced.
 */
int
escape_control_chars (
    char *restrict       dst,
    size_t               dst_max,
    char const *restrict src,
    char                 ch
);

#endif  /* defined(BOOKMARKFS_FSCK_UTIL_H_) */
