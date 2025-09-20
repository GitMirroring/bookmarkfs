/**
 * bookmarkfs/src/hash.c
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "hash.h"

#ifdef ENABLE_BOOKMARKFS_DEBUG
#  define XXH_DEBUGLEVEL  1
#endif
#ifdef ENABLE_XXHASH_INLINE
#  define XXH_INLINE_ALL
#  define XXH_IMPLEMENTATION
#endif
#include <xxhash.h>

#include "xstd.h"

static uint64_t seed;

struct hash_ctx *
hash_init (void)
{
    XXH3_state_t *state = XXH3_createState();
    xassert(state != NULL);

    xassert(0 == XXH3_64bits_reset_withSeed(state, seed));
    return (struct hash_ctx *)state;
}

void
hash_update (
    struct hash_ctx *ctx,
    void const      *src,
    size_t           src_len
) {
    XXH3_state_t *state = (XXH3_state_t *)ctx;

    xassert(0 == XXH3_64bits_update(state, src, src_len));
}

uint64_t
hash_digest (
    struct hash_ctx *ctx
) {
    XXH3_state_t *state = (XXH3_state_t *)ctx;

    uint64_t result = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return result;
}

uint64_t
hash_digest_one (
    void const *src,
    size_t      src_len
) {
    return XXH3_64bits_withSeed(src, src_len, seed);
}

void
hash_seed (
    uint64_t s
) {
    seed = s;
}
