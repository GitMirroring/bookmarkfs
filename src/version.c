/**
 * bookmarkfs/src/version.c
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

#include "version.h"

uint32_t
bookmarkfs_lib_version (void)
{
    uint32_t vernum = BOOKMARKFS_VERNUM;
#ifdef BOOKMARKFS_DEBUG
    vernum |= BOOKMARKFS_FEAT_DEBUG;
#endif
#ifdef BOOKMARKFS_SANDBOX
    vernum |= BOOKMARKFS_FEAT_SANDBOX;
#ifdef BOOKMARKFS_SANDBOX_LANDLOCK
    vernum |= BOOKMARKFS_FEAT_SANDBOX_LANDLOCK;
#endif
#endif  /* defined(BOOKMARKFS_SANDBOX) */
#ifdef BOOKMARKFS_NATIVE_WATCHER
    vernum |= BOOKMARKFS_FEAT_NATIVE_WATCHER;
#endif
    return vernum;
}
