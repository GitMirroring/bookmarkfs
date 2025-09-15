/**
 * bookmarkfs/src/md5.h
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

#ifndef BOOKMARKFS_MD5_H_
#define BOOKMARKFS_MD5_H_

#include <stddef.h>
#include <stdint.h>

#define MD5_BLOCK_SIZE   64
#define MD5_DIGEST_SIZE  16

struct md5_ctx {
    uint32_t state[4];
    uint64_t len;
    size_t   off;
    uint8_t  buf[MD5_BLOCK_SIZE];
};

void
md5_digest (
    struct md5_ctx *ctx,
    uint8_t        *dst   // MD5_DIGEST_SIZE
);

void
md5_init (
    struct md5_ctx *ctx
);

void
md5_update (
    struct md5_ctx *ctx,
    uint8_t const  *src,
    size_t          src_len
);

#endif  /* !defined(BOOKMARKFS_MD5_H_) */
