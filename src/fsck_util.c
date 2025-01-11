/**
 * bookmarkfs/src/fsck_util.c
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

#include "fsck_util.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "xstd.h"

int
explain_fsck_result (
    enum bookmarkfs_fsck_result        result,
    struct bookmarkfs_fsck_data const *data
) {
    char name_buf[sizeof(data->name)];
    escape_control_chars(name_buf, sizeof(name_buf), data->name, '?');

#define PRINT_FSCK_RESULT(s, ...)  \
    printf("bookmark %" PRIu64 " name '%.*s' " s "\n", data->id,  \
            (int)sizeof(name_buf), name_buf, __VA_ARGS__);
    switch (result) {
      case BOOKMARKFS_FSCK_RESULT_END:
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE:
        PRINT_FSCK_RESULT("duplicates with %" PRIu64, data->extra);
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_BADCHAR:
        PRINT_FSCK_RESULT("contains a bad character at offset %" PRIu64,
                data->extra);
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_BADLEN:
        PRINT_FSCK_RESULT("has invalid length %" PRIu64, data->extra);
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_DOTDOT:
        PRINT_FSCK_RESULT("%s", "is invalid (must not be '.' or '..')");
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_INVALID:
        PRINT_FSCK_RESULT("is invalid (reason number %" PRIu64 ")",
                data->extra);
        break;

      default:
        log_printf("unknown fsck result code: %d", result);
        return -1;
    }
    return 0;
}

int
escape_control_chars (
    char *restrict       dst,
    size_t               dst_max,
    char const *restrict src,
    char                 ch
) {
    int cnt = 0;
    for (char *end = stpncpy(dst, src, dst_max); dst < end; ++dst) {
        if (iscntrl(*dst)) {
            *dst = ch;
            ++cnt;
        }
    }
    return cnt;
}
