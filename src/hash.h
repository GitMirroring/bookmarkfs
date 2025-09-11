/**
 * bookmarkfs/src/hash.h
 *
 * Non-cryptographic hash function.
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

#ifndef BOOKMARKFS_HASH_H_
#define BOOKMARKFS_HASH_H_

#include <stdint.h>

#include <sys/uio.h>

typedef size_t (hash_digestcb_func) (
    void        *user_data,
    void const **buf_ptr
);

uint64_t
hash_digest (
    void const *input,
    size_t      len
);

uint64_t
hash_digestv (
    struct iovec const *bufv,
    int                 bufcnt
);

uint64_t
hash_digestcb (
    hash_digestcb_func *callback,
    void               *user_data
);

void
hash_seed (
    uint64_t seed
);

#endif  /* !defined(BOOKMARKFS_HASH_H_) */
