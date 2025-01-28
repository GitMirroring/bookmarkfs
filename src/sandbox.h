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

/**
 * Only perform read operations on dirfd (and the files beneath).
 */
#define SANDBOX_READONLY     ( 1u << 0 )
/**
 * Do not use landlock for sandoxing.
 * Ignored on non-Linux platforms.
 */
#define SANDBOX_NO_LANDLOCK  ( 1u << 1 )
/**
 * sandbox_enter() does nothing and returns successfully.
 */
#define SANDBOX_NOOP         ( 1u << 2 )

/**
 * Instruct the current process (or thread, depending on
 * the implementation) to enter sandbox.
 * In sandbox, the process has limited access to system calls.
 *
 * If dirfd is non-negative, it should refer to a directory
 * which the process needs to access.
 *
 * Returns 0 on success, -1 on failure.
 */
int
sandbox_enter (
    int      dirfd,
    uint32_t flags
);

#endif  /* !defined(BOOKMARKFS_SANDBOX_H_) */
