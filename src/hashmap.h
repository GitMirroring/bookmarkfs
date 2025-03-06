/**
 * bookmarkfs/src/hashmap.h
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

#ifndef BOOKMARKFS_HASHMAP_H_
#define BOOKMARKFS_HASHMAP_H_

#include <stdint.h>

struct hashmap;

union hashmap_key {
    void const *ptr;
    uint64_t    u64;
};

typedef void (hashmap_walk_func) (
    void *user_data,
    void *entry
);

typedef unsigned long (hashmap_hash_func) (
    void const *entry
);

typedef int (hashmap_comp_func) (
    union hashmap_key  key,
    void const        *entry
);

struct hashmap *
hashmap_create (
    hashmap_comp_func *entry_comp,
    hashmap_hash_func *entry_hash
);

void
hashmap_destroy (
    struct hashmap *map
);

void
hashmap_foreach (
    struct hashmap const *map,
    hashmap_walk_func    *walk_func,
    void                 *user_data
);

void *
hashmap_search (
    struct hashmap const *map,
    union hashmap_key     key,
    unsigned long         hashcode,
    unsigned long        *entry_id_ptr
);

void
hashmap_insert (
    struct hashmap *map,
    unsigned long   hashcode,
    void           *entry
);

void
hashmap_delete (
    struct hashmap *map,
    void const     *entry,
    long            entry_id
);

#endif  /* !defined(BOOKMARKFS_HASHMAP_H_) */
