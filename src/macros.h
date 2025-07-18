/**
 * bookmarkfs/src/macros.h
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

#ifndef BOOKMARKFS_MACROS_H_
#define BOOKMARKFS_MACROS_H_

#ifndef STRINGIFY
#  define STRINGIFY(arg)        STRINGIFY_IMPL_(arg)
#  define STRINGIFY_IMPL_(arg)  #arg
#endif

#define STR_WITHLEN(str)  str, strlen(str)

#define TENTH_ARG_(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, ...)  a10
#define NUM_ARGS(...)  TENTH_ARG_(__VA_ARGS__, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define CONCAT_1_(f, s, a1)              ( f(a1) )
#define CONCAT_2_(f, s, a1, a2)          ( f(a1) s f(a2) )
#define CONCAT_3_(f, s, a1, a2, a3)      ( f(a1) s f(a2) s f(a3) )
#define CONCAT_4_(f, s, a1, a2, a3, a4)  ( f(a1) s f(a2) s f(a3) s f(a4) )
#define CONCAT_n_(n)          CONCAT_##n##_
#define CONCAT(f, s, n, ...)  CONCAT_n_(n)(f, s, __VA_ARGS__)

#define BITWISE_OR(f, ...)  CONCAT(f, |, NUM_ARGS(__VA_ARGS__), __VA_ARGS__)

#define BOOKMARK_FLAG_NAME_(name)  BOOKMARKFS_BOOKMARK_##name
#define BOOKMARK_FLAG(...)         BITWISE_OR(BOOKMARK_FLAG_NAME_, __VA_ARGS__)

#define BOOKMARKFS_FEATURE_STRING_EX(which, name, off, on)  \
    &(off name on name)  \
        [(sizeof(STRINGIFY(ENABLE_##which)) == 2) * (sizeof(off name) - 1)]
#define BOOKMARKFS_FEATURE_STRING(which, name)  \
    BOOKMARKFS_FEATURE_STRING_EX(which, name, "  - ", "  + ")

#endif  /* !defined(BOOKMARKFS_MACROS_H_) */
