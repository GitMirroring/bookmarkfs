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
print_fsck_result (
    enum bookmarkfs_fsck_result        result,
    struct bookmarkfs_fsck_data const *data
) {
    char *result_str;
    switch (result) {
      case BOOKMARKFS_FSCK_RESULT_END:
        return 0;

#define FSCK_RESULT(result)                  \
      case BOOKMARKFS_FSCK_RESULT_##result:  \
        result_str = #result;                \
        break;
      FSCK_RESULT(NAME_DUPLICATE)
      FSCK_RESULT(NAME_BADCHAR)
      FSCK_RESULT(NAME_BADLEN)
      FSCK_RESULT(NAME_DOTDOT)
      FSCK_RESULT(NAME_INVALID)

      default:
        log_printf("unknown fsck result code: %d", result);
        return -1;
    }

    char name_buf[sizeof(data->name)];
    translit_control_chars(name_buf, sizeof(name_buf), data->name, '?');
    printf("%" PRIu64 "\t%s\t%" PRIu64 "\t%.*s\n", data->id, result_str,
            data->extra, (int)sizeof(name_buf), name_buf);
    return 0;
}

int
translit_control_chars (
    char *restrict       dst,
    size_t               dst_max,
    char const *restrict src,
    char                 ch
) {
    int cnt = 0;
    for (char *end = stpncpy(dst, src, dst_max); dst < end; ++dst) {
        if (iscntrl((unsigned char)(*dst))) {
            *dst = ch;
            ++cnt;
        }
    }
    return cnt;
}
