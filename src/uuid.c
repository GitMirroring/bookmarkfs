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
#include "xstd.h"

void
uuid_bin2hex (
    char          *restrict dst,
    uint8_t const *restrict src
) {
    base16_encode(dst, src, 4);
    *(dst += 8) = '-';
    base16_encode(++dst, src += 4, 2);
    *(dst += 4) = '-';
    base16_encode(++dst, src += 2, 2);
    *(dst += 4) = '-';
    base16_encode(++dst, src += 2, 2);
    *(dst += 4) = '-';
    base16_encode(++dst, src += 2, 6);
}

int
uuid_hex2bin (
    uint8_t    *restrict dst,
    char const *restrict src
) {
#define ASSERT_EQ(v, e)  if (unlikely((v) != (e))) return -1

    ASSERT_EQ(0, base16_decode(dst, src, 8));
    ASSERT_EQ('-', *(src += 8));
    ASSERT_EQ(0, base16_decode(dst += 4, ++src, 4));
    ASSERT_EQ('-', *(src += 4));
    ASSERT_EQ(0, base16_decode(dst += 2, ++src, 4));
    ASSERT_EQ('-', *(src += 4));
    ASSERT_EQ(0, base16_decode(dst += 2, ++src, 4));
    ASSERT_EQ('-', *(src += 4));
    ASSERT_EQ(0, base16_decode(dst += 2, ++src, 12));

    return 0;
}

#ifndef TESTING_BOOKMARKFS

void
uuid_generate_random (
    uint8_t *dst
) {
    uint64_t const buf[] = { prng_rand(), prng_rand() };
    memcpy(dst, buf, UUID_LEN);

    dst[6] &= 0x0f;
    dst[6] |= 0x40;
    dst[8] &= 0x3f;
    dst[8] |= 0x80;
}

#endif  /* !defined(TESTING_BOOKMARKFS) */
