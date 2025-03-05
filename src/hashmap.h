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

/**
 * Obtain the hashcode for an entry.
 *
 * This function is only called during rehash and entry delete.
 */
typedef unsigned long (hashmap_hash_func) (
    void const *entry
);

/**
 * Check if the given key matches an entry in the hashmap.
 *
 * Returns 0 if matches, non-zero if not.
 */
typedef int (hashmap_comp_func) (
    union hashmap_key  key,
    void const        *entry
);

/**
 * Creates a new hashmap with given callback functions.
 */
struct hashmap *
hashmap_create (
    hashmap_comp_func *entry_comp,
    hashmap_hash_func *entry_hash
);

void
hashmap_destroy (
    struct hashmap *map
);

/**
 * Apply the walk_func callback to each entry in hashmap,
 * passing user_data as its second argument.
 */
void
hashmap_foreach (
    struct hashmap const *map,
    hashmap_walk_func    *walk_func,
    void                 *user_data
);

/**
 * Search for an entry in the hashmap that matches the given key.
 *
 * If the key matches multiple entries,
 * it is unspecified which one will return.
 *
 * If entry_id_ptr is not NULL, on a successful lookup,
 * it will be set to a value that can be later be passed to
 * hashmap_entry_delete().
 *
 * Returns an entry if found, or NULL if not.
 */
void *
hashmap_search (
    struct hashmap const *map,
    union hashmap_key     key,
    unsigned long         hashcode,
    unsigned long        *entry_id_ptr
);

/**
 * Insert an entry into the hashmap.
 *
 * Invalidates all entry IDs given by previous hashmap_search() calls.
 */
void
hashmap_insert (
    struct hashmap *map,
    unsigned long   hashcode,
    void           *entry
);

/**
 * Remove an entry from the hashmap.
 * Undefined behavior if the entry does not exist in hashmap.
 *
 * The entry_id argument should either be the value given by the
 * hashmap_search() or hashmap_insert() function call where the
 * entry is returned from, or -1 (less efficient).
 */
void
hashmap_delete (
    struct hashmap *map,
    void const     *entry,
    long            entry_id
);

#endif  /* !defined(BOOKMARKFS_HASHMAP_H_) */
