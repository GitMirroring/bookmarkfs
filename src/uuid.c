/**
 * bookmarkfs/src/uuid.c
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

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
#  include <nettle/base16.h>
#endif

#include "prng.h"

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE

void
uuid_bin2hex (
    char          *restrict out,
    uint8_t const *restrict in
) {
    size_t const parts[] = { 4, 2, 2, 2, 6 };
    for (int i = 0; i < 5; ++i) {
        size_t src_len = parts[i];
        base16_encode_update(out, src_len, in);

        in  += src_len;
        out += src_len << 1;
        if (i < 4) {
            *(out++) = '-';
        }
    }
}

int
uuid_hex2bin (
    uint8_t    *restrict out,
    char const *restrict in
) {
    struct base16_decode_ctx ctx;
    base16_decode_init(&ctx);

    size_t const parts[] = { 8, 4, 4, 4, 12 };
    for (int i = 0; i < 5; ++i) {
        size_t src_len = parts[i];
        size_t dest_len;
        if (!base16_decode_update(&ctx, &dest_len, out, src_len, in)) {
            return -1;
        }

        out += src_len >> 1;
        in  += src_len;
        if (i < 4 && *(in++) != '-') {
            return -1;
        }
    }

    if (!base16_decode_final(&ctx)) {
        return -1;
    }
    return 0;
}

#else  /* !defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

/**
 * This implementation is meant to get rid of the Nettle dependency
 * when building the Chromium backend without write support.
 *
 * NOTE: Result is incompatible with the above Nettle-based implementations.
 *       It also requires the input to be NUL-terminated.
 */
int
uuid_hex2bin (
    uint8_t    *restrict out,
    char const *restrict in
) {
    uint16_t parts[8];
    int num_matched = sscanf(in,
            "%4"  SCNx16 "%4"  SCNx16 "-%4" SCNx16 "-%4" SCNx16
            "-%4" SCNx16 "-%4" SCNx16 "%4"  SCNx16 "%4"  SCNx16,
            &parts[0], &parts[1], &parts[2], &parts[3],
            &parts[4], &parts[5], &parts[6], &parts[7]);
    if (num_matched != 8) {
        return -1;
    }

    memcpy(out, parts, UUID_LEN);
    return 0;
}

#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

void
uuid_generate_random (
    uint8_t *out
) {
    uint64_t const buf[] = { prng_rand(), prng_rand() };
    memcpy(out, buf, UUID_LEN);

    out[7] &= 0x0f;
    out[7] |= 0x40;
    out[8] &= 0x3f;
    out[8] |= 0x80;
}
