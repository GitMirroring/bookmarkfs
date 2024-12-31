/**
 * bookmarkfs/src/backend_util.h
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

#ifndef BOOKMARKFS_BACKEND_UTIL_H_
#define BOOKMARKFS_BACKEND_UTIL_H_

#include <stdint.h>
#include <string.h>

#include "defs.h"

#define BACKEND_OPT_ERR_BAD_KEY_  0
#define BACKEND_OPT_ERR_BAD_VAL_  1
#define BACKEND_OPT_ERR_HAS_VAL_  2
#define BACKEND_OPT_ERR_NO_VAL_   3
#define BACKEND_OPT_ERR_(err_type)  \
    print_backend_opt_err_(BACKEND_OPT_ERR_##err_type##_, key_, val_)

#define BACKEND_OPT_START(opts)                      \
    for (; (opts) != NULL; (opts) = (opts)->next) {  \
        char const *key_ = (opts)->key;              \
        char const *val_ = (opts)->val;              \
        if (0);
#define BACKEND_OPT_END                              \
        else return BACKEND_OPT_ERR_(BAD_KEY);       \
    }
#define BACKEND_OPT_KEY(name)  else if (0 == strcmp(name, key_))
#define BACKEND_OPT_VAL(name)  else if (0 == strcmp(name, val_))
#define BACKEND_OPT_VAL_START  \
    if (val_ == NULL) return BACKEND_OPT_ERR_(NO_VAL);
#define BACKEND_OPT_VAL_END    else return BACKEND_OPT_ERR_(BAD_VAL);
#define BACKEND_OPT_BAD_VAL()  BACKEND_OPT_ERR_(BAD_VAL)
#define BACKEND_OPT_NO_VAL     \
    if (val_ != NULL) return BACKEND_OPT_ERR_(HAS_VAL);
#define BACKEND_OPT_VAL_STR    val_

#define FILENAME_BADCHAR  -1
#define FILENAME_BADLEN   -2
#define FILENAME_DOTDOT   -3

/**
 * Opens the parent directory of a file, and stores its basename
 * to `basename_ptr`.
 *
 * NOTE: The function may modify `path`, and the pointer stored to
 *       `basename_ptr` is always within the address range of `path`.
 *
 * Returns the directory file descriptor if successful.
 * On error, -1 is returned, and errno is set.
 */
BOOKMARKFS_INTERNAL
int
basename_opendir (
    char  *path,
    char **basename_ptr
);

BOOKMARKFS_INTERNAL
FUNCATTR_COLD
int
print_backend_opt_err_ (
    int         err_type,
    char const *key,
    char const *val
);

/**
 * Check if a string is a valid path component.
 * If the string contains any NUL byte, the result is unspecified.
 *
 * Returns 0 if name is valid, FILENAME_* if not.
 * If returning FILENAME_BADCHAR, stores the pointer to the first
 * bad character to `end_ptr` unless it is NULL.
 */
BOOKMARKFS_INTERNAL
int
validate_filename (
    char const  *str,
    size_t       str_len,
    char const **end_ptr
);

BOOKMARKFS_INTERNAL
int
validate_filename_fsck (
    char const *str,
    size_t      str_len,
    int        *result_ptr,
    uint64_t   *extra_ptr
);

#endif  /* defined(BOOKMARKFS_BACKEND_UTIL_H_) */
