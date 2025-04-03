/**
 * bookmarkfs/src/hashmap.c
 *
 * A simple hashmap implementation using hopscotch hashing
 * for collision resolution.
 *
 * The original paper for hopscotch hashing:
 * <http://mcg.cs.tau.ac.il/papers/disc2008-hopscotch.pdf>
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

#include "hashmap.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "xstd.h"

#if defined(UINT64_MAX) && (UINT64_MAX == ULONG_MAX)
#  define HASHMAP_WORD_SIZE  64
#  define HOP_IDX_WIDTH      6   // log2(64)
#elif (UINT32_MAX == ULONG_MAX)
#  define HASHMAP_WORD_SIZE  32
#  define HOP_IDX_WIDTH      5   // log2(32)
#else
#  error "unsupported sizeof(unsigned long)"
#endif

#define EXP_MIN  8
#define EXP_MAX  ( HASHMAP_WORD_SIZE - HOP_IDX_WIDTH - 1 )

/**
 * Alloc an extra `exp - 1` buckets, so that we don't have to rehash
 * if an accidental collision happens on the last home bucket.
 */
#define BUCKET_CNT(exp)         ( ((size_t)1 << (exp)) + ((exp) - 1) )

#define BUCKET_HOP_MASK(exp)    ( (1ul << (exp)) - 1 )
#define BUCKET_HASH_MASK(exp)   ( ~BUCKET_HOP_MASK(exp) )

#define HASH_TO_IDX(hash, exp)  ( (hash) >> (HASHMAP_WORD_SIZE - (exp)) )
#define PACK_ID(hash_i, hop_i)  ( ((hash_i) << HOP_IDX_WIDTH) + (hop_i) )

#define BIT_SET(b, i)    ( (b) |=  (1ul << (i)) )
#define BIT_UNSET(b, i)  ( (b) &= ~(1ul << (i)) )

struct bucket {
    /**
     * Lower `exp` bits is the "hop" information of the bucket,
     * the remaining bits is part of the hashcode.
     *
     * The "hop" information is a bitmask indicating whether a
     * neighborhood bucket hashes to this bucket.
     *
     * During lookup, the hashcode fragment can be used instead of
     * full hashcode without losing information, since the stripped part
     * is the home bucket index (they are always the same).
     *
     * Compared to the naive approach that uses separate machine words:
     * - The bad:
     *   - Extra cycles introduced by twiddling bits
     *   - Smaller hop size
     *     - Less efficient insertion on a heavily loaded table
     *     - Worse collision resistance
     * - The good:
     *   - Smaller memory footprint
     *   - Better spatial locality during lookup on a heavily loaded table
     */
    unsigned long bits;

    void *entry;
};

struct hashmap {
    struct bucket *buckets;

    size_t num_buckets;
    size_t num_used;

    unsigned exp;

    hashmap_comp_func *entry_comp;
    hashmap_hash_func *entry_hash;
};

// Forward declaration start
static int count_tz   (unsigned long);
static int find_entry (struct hashmap const *, void const *, struct bucket **);
static int make_room  (struct bucket *, struct bucket const *, unsigned);
static int rehash     (struct hashmap *, unsigned);
// Forward declaration end

static int
count_tz (
    unsigned long val
) {
    if (val == 0) {
        return HASHMAP_WORD_SIZE;
    }

#if HASHMAP_WORD_SIZE == 64
#ifdef HAVE___BUILTIN_CTZL
    return __builtin_ctzl(val);
#else
    // Count trailing zeroes with de Bruijn sequence.
    // Interestingly, gcc (but not clang) understands this,
    // and can treat it as if it *is* __builtin_ctzl().
    // Also applies to the 32-bit variant.
    static int lut[] = {
         0,  1, 48,  2, 57, 49, 28,  3, 61, 58, 50, 42, 38, 29, 17,  4,
        62, 55, 59, 36, 53, 51, 43, 22, 45, 39, 33, 30, 24, 18, 12,  5,
        63, 47, 56, 27, 60, 41, 37, 16, 54, 35, 52, 21, 44, 32, 23, 11,
        46, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19,  9, 13,  8,  7,  6,
    };
    return lut[((val & -val) * UINT64_C(0x03f79d71b4cb0a89)) >> 58];
#endif  /* defined(HAVE___BUILTIN_CTZL) */
#else  /* HASHMAP_WORD_SIZE == 32 */
#ifdef HAVE___BUILTIN_CTZ
    return __builtin_ctz(val);
#else
    static int lut[] = {
         0,  1, 28,  2, 29, 14, 24,  3, 30, 22, 20, 15, 25, 17,  4,  8,
        31, 27, 13, 23, 21, 19, 16,  7, 26, 12, 18,  6, 11,  5, 10,  9,
    };
    return lut[((val & -val) * UINT32_C(0x077cb531)) >> 27];
#endif  /* defined(HAVE___BUILTIN_CTZ) */
#endif  /* HASHMAP_WORD_SIZE == 64 */
}

/**
 * Like hashmap_search(), but assumes that the entry exists in hashmap.
 */
static int
find_entry (
    struct hashmap const  *map,
    void const            *entry,
    struct bucket        **home_ptr
) {
    unsigned       exp       = map->exp;
    unsigned long  hashcode  = map->entry_hash(entry);
    size_t         hash_idx  = HASH_TO_IDX(hashcode, exp);
    struct bucket *home      = map->buckets + hash_idx;
    unsigned long  hop       = home->bits;
    unsigned long  hash_mask = BUCKET_HASH_MASK(exp);

    debug_assert(exp < EXP_MAX);
    for (unsigned hop_idx; ; BIT_UNSET(hop, hop_idx)) {
        hop_idx = count_tz(hop);
        debug_assert(hop_idx < exp);

        struct bucket *b = home + hop_idx;
        if ((b->bits & hash_mask) != (hashcode << map->exp)) {
            continue;
        }
        if (entry != map->buckets[b - map->buckets].entry) {
            continue;
        }

        *home_ptr = home;
        return hop_idx;
    }
}

/**
 * Find an empty slot to insert from bucket range [home, end).
 *
 * If the empty slot is not in the neighborhood,
 * attempt to swap it forward.
 *
 * Returns the index of empty slot if found, or -1 if not.
 */
static int
make_room (
    struct bucket       *home,
    struct bucket const *end,
    unsigned             exp
) {
    struct bucket *b;
    for (b = home; b < end; ++b) {
        // Linear probe for the first empty slot.
        if (b->entry == NULL) {
            break;
        }
    }
    if (unlikely(b == end)) {
        // Reaching end of buckets, but no empty slot found
        return -1;
    }

    unsigned long hash_mask = BUCKET_HASH_MASK(exp);
    for (struct bucket *swp; home + exp <= b; b = swp) {
        // Swap empty slot forward.
        for (swp = b - (exp - 1); swp < b; ++swp) {
            size_t hop_idx  = count_tz(swp->bits);
            size_t distance = b - swp;
            if (hop_idx >= distance) {
                continue;
            }
            debug_assert(hop_idx < HASHMAP_WORD_SIZE);
            BIT_SET(swp->bits, distance);
            BIT_UNSET(swp->bits, hop_idx);

            swp += hop_idx;
            b->bits  ^= (b->bits ^ swp->bits) & hash_mask;
            b->entry  = swp->entry;
            break;
        }
        if (unlikely(swp == b)) {
            // Not able to swap empty slot to the neighborhood of home bucket.
            b->entry = NULL;
            return -1;
        }
    }
    return b - home;
}

static int
rehash (
    struct hashmap *map,
    unsigned        new_exp
) {
    if (unlikely(new_exp > EXP_MAX)) {
        log_printf("%p: new size exceeds max limit", (void *)map);
        return -1;
    }

    size_t new_nbuckets = BUCKET_CNT(new_exp);
    struct bucket *new_buckets = xcalloc(new_nbuckets, sizeof(struct bucket));

    struct bucket *old_b_end    = map->buckets + map->num_buckets;
    unsigned long  new_hop_mask = BUCKET_HOP_MASK(new_exp);
    for (struct bucket *old_b = map->buckets; old_b < old_b_end; ++old_b) {
        void *old_e = old_b->entry;
        if (old_e == NULL) {
            continue;
        }

        // Cannot trivially deduce hashcode from old hash fragment,
        // since we have to find its home bucket.
        unsigned long  hashcode     = map->entry_hash(old_e);
        size_t         new_hash_idx = HASH_TO_IDX(hashcode, new_exp);
        struct bucket *new_home     = new_buckets + new_hash_idx;

        int hop_idx = make_room(new_home, new_buckets + new_nbuckets, new_exp);
        if (unlikely(hop_idx < 0)) {
            log_printf("%p: rehash failed", (void *)map);
            free(new_buckets);
            return -1;
        }
        BIT_SET(new_home->bits, hop_idx);

        struct bucket *new_b = new_home + hop_idx;
        new_b->bits  = (new_b->bits & new_hop_mask) | (hashcode << new_exp);
        new_b->entry = old_e;
    }

    free(map->buckets);
    map->buckets     = new_buckets;
    map->num_buckets = new_nbuckets;
    map->exp         = new_exp;
    return 0;
}

struct hashmap *
hashmap_create (
    hashmap_comp_func *entry_comp,
    hashmap_hash_func *entry_hash
) {
    size_t buckets_len = BUCKET_CNT(EXP_MIN);
    // XXX: According to the ISO C standard, null pointers have an
    // implementation-defined value, and should not be zero-initialized
    // with calloc() or memset().
    //
    // However, it is guaranteed to be all-bits-zero on most,
    // if not all, modern ABI standards that we know of.
    // POSIX also has such requirements since POSIX.1-2024.
    //
    // See:
    // - <https://github.com/ARM-software/abi-aa>
    // - <https://github.com/riscv-non-isa/riscv-elf-psabi-doc>
    // - <https://gitlab.com/x86-psABIs/x86-64-ABI>
    // - <https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/stddef.h.html>
    struct bucket *buckets = xcalloc(buckets_len, sizeof(struct bucket));

    struct hashmap *h = xmalloc(sizeof(*h));
    *h = (struct hashmap) {
        .buckets     = buckets,
        .num_buckets = buckets_len,
        .exp         = EXP_MIN,
        .entry_comp  = entry_comp,
        .entry_hash  = entry_hash,
    };
    return h;
}

void
hashmap_destroy (
    struct hashmap *map
) {
    if (map == NULL) {
        return;
    }

    free(map->buckets);
    free(map);
}

void
hashmap_foreach (
    struct hashmap const *map,
    hashmap_walk_func    *walk_func,
    void                 *user_data
) {
    struct bucket *end = map->buckets + map->num_buckets;
    for (struct bucket *b = map->buckets; b < end; ++b) {
        if (b->entry != NULL) {
            walk_func(user_data, b->entry);
        }
    }
}

void
hashmap_delete (
    struct hashmap *map,
    void const     *entry,
    long            entry_id
) {
    struct bucket *home;
    unsigned long  hop_idx;
    if (entry_id < 0) {
        hop_idx = find_entry(map, entry, &home);
    } else {
        home    = map->buckets + (entry_id >> HOP_IDX_WIDTH);
        hop_idx = entry_id & ((1 << HOP_IDX_WIDTH) - 1);
    }
    BIT_UNSET(home->bits, hop_idx);

    struct bucket *b = home + hop_idx;
    debug_assert(b->entry == entry);
    b->entry = NULL;

    size_t buckets_used = --map->num_used;
    if (map->exp <= EXP_MIN) {
        return;
    }
    // load factor < 0.125
    if (buckets_used < (map->num_buckets >> 3)) {
        debug_printf("%p: rehashing: %zu / %zu", (void *)map,
                buckets_used, map->num_buckets - (map->exp - 1));
        rehash(map, map->exp - 1);
    }
}

void
hashmap_insert (
    struct hashmap *map,
    unsigned long   hashcode,
    void           *entry
) {
    unsigned       exp      = map->exp;
    size_t         hash_idx = HASH_TO_IDX(hashcode, exp);
    struct bucket *home     = map->buckets + hash_idx;

    int hop_idx = make_room(home, map->buckets + map->num_buckets, exp);
    if (unlikely(hop_idx < 0)) {
        debug_printf("%p: rehashing: %zu / %zu", (void *)map,
                map->num_used, map->num_buckets - (exp - 1));
        if (unlikely(0 != rehash(map, ++exp))) {
            xassert(0 == rehash(map, ++exp));
        }
        hashmap_insert(map, hashcode, entry);
        return;
    }
    BIT_SET(home->bits, hop_idx);

    struct bucket *b = home + hop_idx;
    b->bits = (b->bits & BUCKET_HOP_MASK(exp)) | (hashcode << exp);

    ++map->num_used;
    debug_assert(entry != NULL);
    b->entry = entry;
}

void *
hashmap_search (
    struct hashmap const *map,
    union hashmap_key     key,
    unsigned long         hashcode,
    unsigned long        *entry_id_ptr
) {
    unsigned       exp       = map->exp;
    size_t         hash_idx  = HASH_TO_IDX(hashcode, exp);
    struct bucket *home      = map->buckets + hash_idx;
    unsigned long  hop       = home->bits;
    unsigned long  hash_mask = BUCKET_HASH_MASK(exp);

    debug_assert(exp <= EXP_MAX);
    for (unsigned hop_idx; ; BIT_UNSET(hop, hop_idx)) {
        hop_idx = count_tz(hop);
        if (hop_idx >= exp) {
            return NULL;
        }

        struct bucket *b = home + hop_idx;
        if ((b->bits & hash_mask) != (hashcode << exp)) {
            continue;
        }
        void *e = b->entry;
        debug_assert(e != NULL);
        if (0 != map->entry_comp(key, e)) {
            continue;
        }
        if (entry_id_ptr != NULL) {
            *entry_id_ptr = PACK_ID(hash_idx, hop_idx);
        }
        return e;
    }
}
