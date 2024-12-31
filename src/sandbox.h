/**
 * bookmarkfs/src/sandbox.h
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

#ifndef BOOKMARKFS_SANDBOX_H_
#define BOOKMARKFS_SANDBOX_H_

#include <stdint.h>

#define SANDBOX_READONLY     ( 1u << 0 )
#define SANDBOX_NO_LANDLOCK  ( 1u << 1 )
#define SANDBOX_NOOP         ( 1u << 2 )

int
sandbox_enter (
    int      fusefd,
    int      dirfd,
    uint32_t flags
);

#endif  /* !defined(BOOKMARKFS_SANDBOX_H_) */
