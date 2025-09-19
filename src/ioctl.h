/**
 * bookmarkfs/src/ioctl.h
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

#ifndef BOOKMARKFS_IOCTL_H_
#define BOOKMARKFS_IOCTL_H_

#include <sys/ioctl.h>

#ifdef BUILDING_BOOKMARKFS
#  include "common.h"
#else
#  include <bookmarkfs/common.h>
#endif

#define BOOKMARKFS_IOC_MAGIC_     0xbf
#define BOOKMARKFS_IOC_(rw, ...)  \
    ( (unsigned)_IO##rw(BOOKMARKFS_IOC_MAGIC_, __VA_ARGS__) )
#define BOOKMARKFS_IOC_RW_(rw, nr, name)  \
    BOOKMARKFS_IOC_(rw, nr, struct bookmarkfs_##name##_data)

#define BOOKMARKFS_IOC_FSCK_REWIND  BOOKMARKFS_IOC_(,      0)
#define BOOKMARKFS_IOC_FSCK_NEXT    BOOKMARKFS_IOC_RW_(R,  1, fsck)
#define BOOKMARKFS_IOC_FSCK_APPLY   BOOKMARKFS_IOC_RW_(WR, 2, fsck)
#define BOOKMARKFS_IOC_PERMD        BOOKMARKFS_IOC_RW_(W,  3, permd)

#endif  /* !defined(BOOKMARKFS_IOCTL_H_) */
