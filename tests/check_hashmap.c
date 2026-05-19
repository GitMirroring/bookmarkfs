/**
 * bookmarkfs/tests/check_hashmap.c
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <sys/mman.h>
#include <unistd.h>

#include "check_util.h"
#include "frontend_util.h"
#include "hashmap.h"
#include "prng.h"

struct check_item {
    unsigned long id;
    unsigned long val;
};

// Forward declaration start
static int  check_one_round  (struct hashmap *, struct check_item *, size_t);
static int  do_check_hashmap (size_t, int);
static int  item_comp_func   (union hashmap_key, void const *);
static unsigned long
            item_hash_func   (void const *);
static void item_walk_func   (void *, void *);
// Forward declaration end

static int
check_one_round (
    struct hashmap    *map,
    struct check_item *items,
    size_t             items_cnt
) {
    for (size_t i = 0; i < items_cnt; ++i) {
        struct check_item *item = items + (prng_rand() % items_cnt);
        unsigned long id  = item->id;
        unsigned long val = item->val;

        union hashmap_key key = { .u64 = id };
        unsigned long entry_id;
        void *found = hashmap_search(map, key, val, &entry_id);

        if (id & 1) {
            if (found != item) {
                log_puts("unexpected item in hashmap");
                return -1;
            }
            hashmap_update(map, item, NULL, entry_id);
        } else {
            if (found != NULL) {
                log_puts("unexpected item in hashmap");
                return -1;
            }
            hashmap_insert(map, val, item);
        }
        item->id = id ^ 1;
    }
    return 0;
}

int
do_check_hashmap (
    size_t items_cnt,
    int    rounds
) {
    size_t buf_size = sizeof(struct check_item) * items_cnt;
    struct check_item *items = mmap(NULL, buf_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (items == MAP_FAILED) {
        log_puts("failed to allocate memory");
        return -1;
    }
    struct hashmap *map = hashmap_create(item_comp_func, item_hash_func);

    for (size_t i = 0; i < items_cnt; ++i) {
        items[i] = (struct check_item) {
            .id  = i << 1,
            .val = prng_rand(),
        };
    }

    int status = -1;
    for (int i = 0; i < rounds; ++i) {
        if (0 != check_one_round(map, items, items_cnt)) {
            goto end;
        }
    }

    size_t cnt = 0;
    hashmap_foreach(map, item_walk_func, &cnt);

    for (size_t i = 0; i < items_cnt; ++i) {
        struct check_item *item = items + i;

        if (item->id & 1) {
            --cnt;
            hashmap_update(map, item, NULL, -1);
        }
    }
    if (cnt != 0) {
        log_printf("bad item cnt %zu, expected 0", cnt);
        goto end;
    }

    hashmap_foreach(map, item_walk_func, &cnt);
    if (cnt != 0) {
        log_printf("bad item cnt %zu, expected 0", cnt);
        goto end;
    }
    status = 0;

  end:
    munmap(items, buf_size);
    hashmap_destroy(map);
    return status;
}

static int
item_comp_func (
    union hashmap_key  key,
    void const        *entry
) {
    struct check_item const *item = entry;

    return (key.u64 >> 1) - (item->id >> 1);
}

static unsigned long
item_hash_func (
    void const *entry
) {
    struct check_item const *item = entry;

    return item->val;
}

static void
item_walk_func (
    void *user_data,
    void *UNUSED_VAR(entry)
) {
    size_t *cnt_ptr = user_data;

    ++(*cnt_ptr);
}

int
check_hashmap (
    int   argc,
    char *argv[]
) {
    int size_exp = -1;
    int rounds   = -1;

    OPT_START(argc, argv, "n:r:")
    OPT_OPT('n') {
        size_exp = atoi(optarg);
        break;
    }
    OPT_OPT('r') {
        rounds = atoi(optarg);
        break;
    }
    OPT_END

    if (size_exp < 10 || size_exp > 30) {
        log_printf("bad size %d", size_exp);
        return -1;
    }
    if (rounds < 0) {
        log_printf("bad rounds cnt %d", rounds);
        return -1;
    }

    if (0 != prng_seed_from_env()) {
        return -1;
    }
    return do_check_hashmap(1u << size_exp, rounds);
}
