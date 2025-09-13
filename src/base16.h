/**
 * bookmarkfs/src/base16.h
 *
 * Base16 data encoding/decoding (RFC 4648).
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

#ifndef BOOKMARKFS_BASE16_H_
#define BOOKMARKFS_BASE16_H_

#include <stddef.h>
#include <stdint.h>

int
base16_decode (
    uint8_t    *restrict dst,
    char const *restrict src,
    size_t               src_len
);

void
base16_encode (
    char          *restrict dst,
    uint8_t const *restrict src,
    size_t                  src_len
);

#endif  /* !defined(BOOKMARKFS_BASE16_H_) */
