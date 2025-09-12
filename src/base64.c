/**
 * bookmarkfs/src/base64.c
 *
 * Base64 data encoding/decoding (RFC 4648).
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
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

#include "base64.h"

#include "xstd.h"

int
base64url_decode_nopad (
    uint8_t    *restrict dst,
    char const *restrict src,
    size_t               src_len
) {
    debug_assert(src_len % 4 == 0);

    static uint8_t const lut[256] = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
        64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 63,
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    };
    for (char const *end = src + src_len; src < end; src += 4, dst += 3) {
        uint32_t a = lut[(unsigned char)src[0]];
        uint32_t b = lut[(unsigned char)src[1]];
        uint32_t c = lut[(unsigned char)src[2]];
        uint32_t d = lut[(unsigned char)src[3]];
        if (unlikely((a | b | c | d) & 64)) {
            return -1;
        }

        uint32_t bits = (a << 18) | (b << 12) | (c << 6) | (d << 0);
        dst[0] = (bits >> 16) & 0xff;
        dst[1] = (bits >>  8) & 0xff;
        dst[2] = (bits >>  0) & 0xff;
    }
    return 0;
}

void
base64url_encode_nopad (
    char          *restrict dst,
    uint8_t const *restrict src,
    size_t                  src_len
) {
    debug_assert(src_len % 3 == 0);

    static char const lut[64] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
        'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
        'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_',
    };
    for (uint8_t const *end = src + src_len; src < end; src += 3, dst += 4) {
        uint32_t bits = 0;
        bits |= src[0] << 16;
        bits |= src[1] <<  8;
        bits |= src[2] <<  0;

        dst[0] = lut[(bits >> 18) & 0x3f];
        dst[1] = lut[(bits >> 12) & 0x3f];
        dst[2] = lut[(bits >>  6) & 0x3f];
        dst[3] = lut[(bits >>  0) & 0x3f];
    }
}
