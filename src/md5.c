/**
 * bookmarkfs/src/md5.c
 *
 * MD5 message digest (RFC 1321).
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

#include "md5.h"

#include <string.h>

#if defined(__linux__)
#  include <endian.h>
#elif defined(__FreeBSD__)
#  include <sys/endian.h>
#endif

// Forward declaration start
static void md5_update_one (uint32_t *, uint8_t const *);
// Forward declaration end

static void
md5_update_one (
    uint32_t      *state,
    uint8_t const *block
) {
    static uint32_t const t0[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static unsigned const t1[16] = {
        7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21,
    };

    // A temporary buffer is required even on little-endian hosts,
    // since we may be dealing with unaligned uint32_t loads.
    uint32_t buf[16];
    memcpy(buf, block, MD5_BLOCK_SIZE);
    for (int i = 0; i < 16; ++i) {
        buf[i] = le32toh(buf[i]);
    }

    uint32_t s0 = state[0];
    uint32_t s1 = state[1];
    uint32_t s2 = state[2];
    uint32_t s3 = state[3];

#define ROTL32(v, n)  ( (v) << (n) | (v) >> (32 - (n)) )
#define MD5_OP(e, j)                           \
    uint32_t tmp = (e) + s0 + t0[i] + buf[j];  \
    s0 = s3; s3 = s2; s2 = s1;                 \
    s1 += ROTL32(tmp, t1[i / 16 * 4 + i % 4])

    for (int i = 0; i < 16; ++i) {
        MD5_OP((s1 & s2) | (~s1 & s3), i);
    }
    for (int i = 16; i < 32; ++i) {
        MD5_OP((s1 & s3) | (s2 & ~s3), (i * 5 + 1) % 16);
    }
    for (int i = 32; i < 48; ++i) {
        MD5_OP(s1 ^ s2 ^ s3, (i * 3 + 5) % 16);
    }
    for (int i = 48; i < 64; ++i) {
        MD5_OP(s2 ^ (s1 | ~s3), (i * 7) % 16);
    }

    state[0] += s0;
    state[1] += s1;
    state[2] += s2;
    state[3] += s3;
}

void
md5_digest (
    struct md5_ctx *ctx,
    uint8_t        *dst
) {
#define MD5_LAST_SIZE  ( MD5_BLOCK_SIZE - sizeof(uint64_t) )

    size_t off = ctx->off;
    ctx->buf[off++] = 0x80;
    if (off > MD5_LAST_SIZE) {
        memset(ctx->buf + off, 0, MD5_BLOCK_SIZE - off);
        md5_update_one(ctx->state, ctx->buf);
        off = 0;
    }
    memset(ctx->buf + off, 0, MD5_LAST_SIZE - off);

    *(uint64_t *)(ctx->buf + MD5_LAST_SIZE) = htole64(ctx->len * 8);
    md5_update_one(ctx->state, ctx->buf);

    for (int i = 0; i < 4; ++i) {
        ctx->state[i] = htole32(ctx->state[i]);
    }
    memcpy(dst, ctx->state, MD5_DIGEST_SIZE);
}

void
md5_init (
    struct md5_ctx *ctx
) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->len = 0;
    ctx->off = 0;
}

void
md5_update (
    struct md5_ctx *ctx,
    uint8_t const  *src,
    size_t          src_len
) {
    ctx->len += src_len;

    size_t off = ctx->off;
    if (off > 0) {
        size_t rem_len = MD5_BLOCK_SIZE - off;
        if (src_len < rem_len) {
            memcpy(ctx->buf + off, src, src_len);
            ctx->off = off + src_len;
            return;
        }
        memcpy(ctx->buf + off, src, rem_len);
        md5_update_one(ctx->state, ctx->buf);
        src     += rem_len;
        src_len -= rem_len;
    }

    off = src_len % MD5_BLOCK_SIZE;
    for (uint8_t const *end = src + src_len - off; src < end; ) {
        md5_update_one(ctx->state, src);
        src += MD5_BLOCK_SIZE;
    }
    ctx->off = off;
    memcpy(ctx->buf, src, off);
}
