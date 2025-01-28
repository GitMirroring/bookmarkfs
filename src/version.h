/**
 * bookmarkfs/src/version.h
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

#ifndef BOOKMARKFS_VERSION_H_
#define BOOKMARKFS_VERSION_H_

#include <stdint.h>

#define BOOKMARKFS_VER_MAJOR  0
#define BOOKMARKFS_VER_MINOR  1
#define BOOKMARKFS_VER_PATCH  0

#define bookmarkfs_make_vernum(major, minor, patch)  \
    ( ((major) << 16) | ((minor) << 8) | ((patch) << 0) )

#define BOOKMARKFS_VERNUM  \
    bookmarkfs_make_vernum(BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR,  \
            BOOKMARKFS_VER_PATCH)

#define bookmarkfs_vernum(vernum)           ( ((vernum) << 8 ) >> 8    )
#define bookmarkfs_vernum_to_major(vernum)  ( ((vernum) >> 16) &  0xff )
#define bookmarkfs_vernum_to_minor(vernum)  ( ((vernum) >> 8 ) &  0xff )
#define bookmarkfs_vernum_to_patch(vernum)  ( ((vernum) >> 0 ) &  0xff )

#define BOOKMARKFS_FEAT_DEBUG             ( 1u << 24 )
#define BOOKMARKFS_FEAT_NATIVE_WATCHER    ( 1u << 25 )
#define BOOKMARKFS_FEAT_SANDBOX           ( 1u << 26 )
#define BOOKMARKFS_FEAT_SANDBOX_LANDLOCK  ( 1u << 27 )

/**
 * Obtain version and feature information of the utility library.
 *
 * Returns a combination of the version number
 * and BOOKMARKFS_FEAT_xxx flags.
 */
uint32_t
bookmarkfs_lib_version (void);

#endif  /* !defined(BOOKMARKFS_VERSION_H_) */
