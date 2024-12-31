/**
 * bookmarkfs/src/lib.h
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

#ifndef BOOKMARKFS_LIB_H_
#define BOOKMARKFS_LIB_H_

#include "defs.h"

BOOKMARKFS_INTERNAL
int
bookmarkfs_lib_init (void);

BOOKMARKFS_INTERNAL
FUNCATTR_COLD
void
bookmarkfs_print_lib_version (
    char const *prefix
);

#endif  /* !defined(BOOKMARKFS_LIB_H_) */
