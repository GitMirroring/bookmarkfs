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

#include <limits.h>
#include <stdint.h>

#include <sys/ioctl.h>

#define BOOKMARKFS_IOC_MAGIC_     0xbf
#define BOOKMARKFS_IOC_(rw, ...)  _IO##rw(BOOKMARKFS_IOC_MAGIC_, __VA_ARGS__)
#define BOOKMARKFS_IOC_RW_(rw, nr, name)  \
    BOOKMARKFS_IOC_(rw, nr, struct bookmarkfs_##name##_data)

#define BOOKMARKFS_IOC_FSCK_REWIND  BOOKMARKFS_IOC_(,      0)
#define BOOKMARKFS_IOC_FSCK_NEXT    BOOKMARKFS_IOC_RW_(R,  1, fsck)
#define BOOKMARKFS_IOC_FSCK_APPLY   BOOKMARKFS_IOC_RW_(WR, 2, fsck)
#define BOOKMARKFS_IOC_PERMD        BOOKMARKFS_IOC_RW_(W,  3, permd)

enum bookmarkfs_fsck_result {
    BOOKMARKFS_FSCK_RESULT_END = 0,  // must be 0
    BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE,
    BOOKMARKFS_FSCK_RESULT_NAME_BADCHAR,
    BOOKMARKFS_FSCK_RESULT_NAME_BADLEN,
    BOOKMARKFS_FSCK_RESULT_NAME_INVALID,
};

/**
 * Predefined reason codes for BOOKMARKFS_FSCK_RESULT_NAME_INVALID.
 */
enum {
    BOOKMARKFS_NAME_INVALID_REASON_DOTDOT = 256,
    BOOKMARKFS_NAME_INVALID_REASON_NOTUTF8,
};

enum bookmarkfs_permd_op {
    BOOKMARKFS_PERMD_OP_SWAP,
    BOOKMARKFS_PERMD_OP_MOVE_BEFORE,
    BOOKMARKFS_PERMD_OP_MOVE_AFTER,
};

struct bookmarkfs_fsck_data {
    uint64_t id;
    uint64_t extra;
    char     name[NAME_MAX + 1];
};

struct bookmarkfs_permd_data {
    enum bookmarkfs_permd_op op;

    char name1[NAME_MAX + 1];
    char name2[NAME_MAX + 1];
};

#endif  /* !defined(BOOKMARKFS_IOCTL_H_) */
