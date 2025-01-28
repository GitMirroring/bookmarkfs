/**
 * bookmarkfs/src/prng.h
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

#ifndef BOOKMARKFS_PRNG_H_
#define BOOKMARKFS_PRNG_H_

#include <stdint.h>

/**
 * Returns a pseudo-random 64-bit unsigned integer.
 * The PRNG should be seeded before calling this function.
 *
 * This function is MT-Unsafe.
 */
uint64_t
prng_rand (void);

/**
 * Seed the PRNG with the given values.
 * If `s` is NULL, the values will be read from /dev/urandom.
 *
 * Returns 0 on success, -1 on failure.
 * If `s` is not NULL, this function never fails.
 */
int
prng_seed (
    uint64_t const s[4]
);

#endif  /* !defined(BOOKMARKFS_PRNG_H_) */
