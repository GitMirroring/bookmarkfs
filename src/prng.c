/**
 * bookmarkfs/src/prng.c
 *
 * Non-cryptographic pseudo-random number generator.
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

#include "prng.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <sys/random.h>

#include "xstd.h"

// Forward declaration start
static uint64_t rotl64 (uint64_t, unsigned);
// Forward declaration end

static uint64_t state[4];

static uint64_t
rotl64 (
    uint64_t val,
    unsigned n
) {
    return (val << n) | (val >> (64 - n));
}

/**
 * The xoshiro256++ PRNG.
 *
 * See: <https://prng.di.unimi.it/xoshiro256plusplus.c>
 */
uint64_t
prng_rand (void)
{
    uint64_t result = rotl64(state[0] + state[3], 23) + state[0];

    uint64_t tmp = state[1] << 17;
    state[2] ^= state[0];
    state[3] ^= state[1];
    state[1] ^= state[2];
    state[0] ^= state[3];
    state[2] ^= tmp;
    state[3] = rotl64(state[3], 45);

    return result;
}

int
prng_seed (
    uint64_t const s[4]
) {
    if (s != NULL) {
        memcpy(state, s, sizeof(state));
        return 0;
    }

    ssize_t nbytes = getrandom(state, sizeof(state), 0);
    if (unlikely(nbytes < 0)) {
        log_printf("getrandom(): %s", xstrerror(errno));
        return -1;
    }

    // For reads up to 256 bytes, getrandom() always returns
    // as many bytes as requested.
    // This is guaranteed on both Linux and FreeBSD.
    debug_assert(nbytes == sizeof(state));
    debug_printf("prng seed: "
            "%016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64,
            state[0], state[1], state[2], state[3]);
    return 0;
}

void
prng_state (
    uint64_t dst[4]
) {
    memcpy(dst, state, sizeof(state));
}
