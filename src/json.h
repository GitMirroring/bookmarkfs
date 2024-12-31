/**
 * bookmarkfs/src/json.h
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

#ifndef BOOKMARKFS_JSON_H_
#define BOOKMARKFS_JSON_H_

#include <string.h>

#include <jansson.h>

// The following helper macros are useful with literal keys,
// since the compiler can optimize out the strlen() call.
#define json_object_sget(obj, key)  json_object_getn(obj, key, strlen(key))
#define json_object_sset(obj, key, value)  \
    json_object_setn_nocheck(obj, key, strlen(key), value)
#define json_object_sset_new(obj, key, value)  \
    json_object_setn_new_nocheck(obj, key, strlen(key), value)
#define json_sstring(str)  json_stringn_nocheck(str, strlen(str))

#define json_object_foreach_iter(obj, iter)                 \
    for (void *iter = json_object_iter(obj); iter != NULL;  \
            iter = json_object_iter_next(obj, iter))

#define json_object_sset_copy(obj, key, value)  \
    json_object_sset_new(obj, key, json_copy(value))

/**
 * Find the offset of `needle` in `haystack`.
 *
 * If `haystack` is not an array, or `needle` cannot be found
 * within `haystack`, function behavior is undefined.
 */
size_t
json_array_search (
    json_t const *haystack,
    json_t const *needle
);

/**
 * Like json_dump_file(), but opens file with openat().
 *
 * If `name` is not the last path component,
 * function behavior is undefined.
 *
 * Writes to a temporary file first, so that failing do dump
 * does not corrupt existing file.
 */
int
json_dump_file_at (
    json_t const *json,
    int           dirfd,
    char const   *name,
    size_t        flags
);

/**
 * Like json_dumpfd(), using a buffer to reduce system call overhead.
 *
 * NOTE: As of Jansson 2.14, json_dumpfd() does not buffer writes.
 */
int
json_dumpfd_ex (
    json_t const *json,
    int           fd,
    size_t        flags
);

/**
 * Like json_load_file(), but opens file with openat().
 */
json_t *
json_load_file_at (
    int         dirfd,
    char const *name,
    size_t      flags
);

#endif  /* !defined(BOOKMARKFS_JSON_H_) */
