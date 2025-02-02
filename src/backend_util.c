/**
 * bookmarkfs/src/backend_util.c
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

#include "backend_util.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <fcntl.h>

#include "common.h"
#include "xstd.h"

int
basename_opendir (
    char  *path,
    char **basename_ptr
) {
    char *basename = strrchr(path, '/');
    if (basename != NULL) {
        *(basename++) = '\0';
    } else {
        basename = path;
        path     = ".";
    }
    *basename_ptr = basename;

    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        log_printf("open(): %s: %s", path, xstrerror(errno));
        return -1;
    }
    return fd;
}

int
print_backend_opt_err_ (
    int         err_type,
    char const *key,
    char const *val
) {
    switch (err_type) {
      case BACKEND_OPT_ERR_BAD_KEY_:
        log_printf("bad backend option '%s'", key);
        break;

      case BACKEND_OPT_ERR_BAD_VAL_:
        log_printf("bad value '%s' for backend option '%s'", val, key);
        break;

      case BACKEND_OPT_ERR_HAS_VAL_:
        log_printf("unexpected value for backend option '%s'", key);
        break;

      case BACKEND_OPT_ERR_NO_VAL_:
        log_printf("no value given for backend option '%s'", key);
        break;
    }

    return -1;
}

int
validate_filename (
    char const  *str,
    size_t       str_len,
    char const **end_ptr
) {
    switch (str_len) {
      case 2:
        if (str[1] != '.') {
            break;
        }
        // fallthrough
      case 1:
        if (str[0] != '.') {
            break;
        }
        return FILENAME_DOTDOT;

      default:
        if (str_len <= NAME_MAX) {
            break;
        }
        // fallthrough
      case 0:
        return FILENAME_BADLEN;
    }

    char const *end = memchr(str, '/', str_len);
    if (end != NULL) {
        if (end_ptr != NULL) {
            *end_ptr = end;
        }
        return FILENAME_BADCHAR;
    }
    return 0;
}

int
validate_filename_fsck (
    char const *str,
    size_t      str_len,
    int        *result_ptr,
    uint64_t   *extra_ptr
) {
    uint64_t extra;
    int      result;

    char const *end;
    int status = validate_filename(str, str_len, &end);
    switch (status) {
      case FILENAME_BADCHAR:
        extra  = end - str;
        result = BOOKMARKFS_FSCK_RESULT_NAME_BADCHAR;
        break;

      case FILENAME_BADLEN:
        extra  = str_len;
        result = BOOKMARKFS_FSCK_RESULT_NAME_BADLEN;
        break;

      case FILENAME_DOTDOT:
        extra  = str[1] == '\0';
        result = BOOKMARKFS_FSCK_RESULT_NAME_DOTDOT;
        break;

      default:
        debug_assert(status == 0);
        return 0;
    }

    *result_ptr = result;
    *extra_ptr  = extra;
    return status;
}
