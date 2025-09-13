/**
 * bookmarkfs/src/uuid.h
 *
 * Utilities for manipulating RFC 4122 UUIDs.
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

#ifndef BOOKMARKFS_UUID_H_
#define BOOKMARKFS_UUID_H_

#include <stdint.h>

#define UUID_LEN      16
#define UUID_HEX_LEN  36

/**
 * Convert a UUID from binary to hexadecimal representation.
 *
 * NOTE: This function does not validate the UUID value.
 */
void
uuid_bin2hex (
    char          *restrict dst,  // UUID_HEX_LEN
    uint8_t const *restrict src   // UUUD_LEN
);

/**
 * Convert a UUID from hexadecimal to binary representation.
 *
 * Returns 0 on success, -1 on failure.
 *
 * NOTE: Like uuid_bin2hex(), this function does not validate
 *       the UUID value, and only fails on bad input string format.
 */
int
uuid_hex2bin (
    uint8_t    *restrict dst,  // UUID_LEN
    char const *restrict src   // UUID_HEX_LEN
);

/**
 * Pseudo-randomly generate a version 4 UUID (binary representation).
 *
 * Internally uses the PRNG from the bookmarkfs-util library,
 * which should be seeded before calling this function.
 */
void
uuid_generate_random (
    uint8_t *dst  // UUID_LEN
);

#endif  /* !defined(BOOKMARKFS_UUID_H_) */
