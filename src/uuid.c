/**
 * bookmarkfs/src/uuid.c
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "uuid.h"

#include <string.h>

#include "base16.h"
#include "prng.h"

void
uuid_bin2hex (
    char          *restrict dst,
    uint8_t const *restrict src
) {
    size_t const parts[] = { 4, 2, 2, 2, 6 };
    for (int i = 0; i < 5; ++i) {
        size_t src_len = parts[i];
        base16_encode(dst, src, src_len);

        src += src_len;
        dst += src_len << 1;
        if (i < 4) {
            *(dst++) = '-';
        }
    }
}

int
uuid_hex2bin (
    uint8_t    *restrict dst,
    char const *restrict src
) {
    size_t const parts[] = { 8, 4, 4, 4, 12 };
    for (int i = 0; i < 5; ++i) {
        size_t src_len = parts[i];
        if (0 != base16_decode(dst, src, src_len)) {
            return -1;
        }

        dst += src_len >> 1;
        src += src_len;
        if (i < 4 && *(src++) != '-') {
            return -1;
        }
    }
    return 0;
}

#ifndef TESTING_BOOKMARKFS

void
uuid_generate_random (
    uint8_t *dst
) {
    uint64_t const buf[] = { prng_rand(), prng_rand() };
    memcpy(dst, buf, UUID_LEN);

    dst[7] &= 0x0f;
    dst[7] |= 0x40;
    dst[8] &= 0x3f;
    dst[8] |= 0x80;
}

#endif  /* !defined(TESTING_BOOKMARKFS) */
