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

#include <stddef.h>
#include <stdint.h>

struct hash_ctx;

struct hash_ctx *
hash_init (void);

void
hash_update (
    struct hash_ctx *ctx,
    void const      *src,
    size_t           src_len
);

uint64_t
hash_digest (
    struct hash_ctx *ctx
);

uint64_t
hash_digest_one (
    void const *src,
    size_t      src_len
);

void
hash_seed (
    uint64_t seed
);

#endif  /* !defined(BOOKMARKFS_HASH_H_) */
