/**
 * bookmarkfs/tests/check_lib.h
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef BOOKMARKFS_CHECK_LIB_H_
#define BOOKMARKFS_CHECK_LIB_H_

#include <stdint.h>

#include "xstd.h"

#define ASSERT_EXPR_INT(expr, r, cond, action_if_false)        \
    do {                                                       \
        int r = (expr);                                        \
        if (cond) {                                            \
            break;                                             \
        }                                                      \
        log_printf("assertion failed: (%d == %s)", r, #expr);  \
        action_if_false                                        \
    } while (0)

int
check_sandbox (
    int   argc,
    char *argv[]
);

int
check_watcher (
    int   argc,
    char *argv[]
);

int
prng_seed_from_hex (
    uint64_t   *buf,
    char const *str
);

#endif  /* !defined(BOOKMARKFS_CHECK_LIB_H_) */
