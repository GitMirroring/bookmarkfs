/**
 * bookmarkfs/tests/check_fs_dents.c
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check_util.h"
#include "frontend_util.h"
#include "ioctl.h"
#include "prng.h"

#define ITEM_DELETED  ( 1u << 0 )
#define ITEM_DIRTY    ( 1u << 1 )
#define ITEM_MARKED   ( 1u << 2 )

struct check_item {
    int      id;
    unsigned flags;
    int      ref;
};

// Forward declaration start
static int dent_check        (int, struct check_item *, int, int);
static int dent_delete       (int, struct check_item *);
static int dent_new          (int, struct check_item *);
static int dent_permute      (int, struct check_item *, struct check_item *);
static int do_check_fs_dents (int, int);
// Forward declaration end

static int
dent_check (
    int                dirfd,
    struct check_item *items,
    int                n,
    int                ignore_dirty
) {
    char buf[4096];
    struct check_item const *last_found = NULL;
    for (ssize_t off = 0, len = 0; ; ) {
        if (off == len) {
            len = xgetdents(dirfd, buf, sizeof(buf));
            if (len < 0) {
                log_printf("getdents(): %s", strerror(errno));
                return -1;
            }
            if (len == 0) {
                break;
            }
            off = 0;
        }
        struct dirent *dent = (struct dirent *)(buf + off);
        off += dent->d_reclen;

        int id = atoi(dent->d_name);
        if (id < 0 || id >= n) {
            return -1;
        }
        struct check_item *found = items + items[id].ref;
        if (found->flags & (ITEM_DELETED | ITEM_MARKED)) {
            return -1;
        }
        if (ignore_dirty && found->flags & ITEM_DIRTY) {
            continue;
        }
        if (last_found != NULL && found <= last_found) {
            return -1;
        }
        last_found = found;
        found->flags |= ITEM_MARKED;
    }

    for (last_found = items + n; items < last_found; ++items) {
        if (ignore_dirty && items->flags & ITEM_DIRTY) {
            continue;
        }
        if (!(items->flags & (ITEM_DELETED | ITEM_MARKED))) {
            return -1;
        }
        items->flags &= ~ITEM_MARKED;
    }
    return 0;
}

static int
dent_delete (
    int                dirfd,
    struct check_item *item
) {
    if (item->flags & ITEM_DELETED) {
        return 0;
    }

    char name[16];
    sprintf(name, "%d", item->id);
    if (0 != unlinkat(dirfd, name, 0)) {
        log_printf("unlinkat(): %s", strerror(errno));
        return -1;
    }
    item->flags |= ITEM_DELETED;
    return 0;
}

static int
dent_new (
    int                dirfd,
    struct check_item *item
) {
    char name[16];
    sprintf(name, "%d", item->id);
    int fd = openat(dirfd, name, O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        log_printf("openat(): %s", strerror(errno));
        return -1;
    }
    close(fd);
    item->flags &= ~ITEM_DELETED;
    return 0;
}

static int
dent_permute (
    int                dirfd,
    struct check_item *item1,
    struct check_item *item2
) {
    int op = BOOKMARKFS_PERMD_OP_SWAP;
    struct check_item *itemx = item2;
    if ((item1->flags & ITEM_DELETED) || (item2->flags & ITEM_DELETED)) {
        if (item1->flags & ITEM_DELETED) {
            struct check_item *item_tmp = item1;
            item1 = item2;
            item2 = item_tmp;
        }
        if (item1 > item2) {
            op = BOOKMARKFS_PERMD_OP_MOVE_BEFORE;
            for (itemx = item2 + 1; itemx->flags & ITEM_DELETED; ++itemx);
        } else {
            op = BOOKMARKFS_PERMD_OP_MOVE_AFTER;
            for (itemx = item2 - 1; itemx->flags & ITEM_DELETED; --itemx);
        }
    }

    struct bookmarkfs_permd_data permd_data;
    permd_data.op = op;
    sprintf(permd_data.name1, "%d", item1->id);
    sprintf(permd_data.name2, "%d", itemx->id);
    if (0 != ioctl(dirfd, BOOKMARKFS_IOC_PERMD, &permd_data)) {
        log_printf("ioctl(): %s", strerror(errno));
        return -1;
    }

    struct check_item item_tmp = *item1;
    item1->id    = item2->id;
    item1->flags = item2->flags | ITEM_DIRTY;
    item2->id    = item_tmp.id;
    item2->flags = item_tmp.flags | ITEM_DIRTY;
    return 0;
}

static int
do_check_fs_dents (
    int dirfd,
    int n
) {
#define ASSERT_EQ(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) == r_, goto end;)
#define ASSERT_NE(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) != r_, goto end;)

    struct check_item *items = calloc(n, sizeof(struct check_item));
    if (items == NULL) {
        return -1;
    }
    int status = -1;

    for (int i = 0; i < n; ++i) {
        struct check_item *item = items + i;
        item->ref = item->id = i;
        ASSERT_EQ(0, dent_new(dirfd, item));
    }

    ASSERT_EQ(0, lseek(dirfd, 0, SEEK_SET));
    for (int i = 0; i < n / 4; ++i) {
        uint64_t bits = prng_rand();
        struct check_item *i1 = &items[bits % n];
        struct check_item *i2 = &items[(bits >> 32) % n];

        if (i1 == i2 || bits >> 63) {
            ASSERT_EQ(0, dent_delete(dirfd, i1));
            ASSERT_EQ(0, dent_delete(dirfd, i2));
#if !defined(__FreeBSD__)
        } else {
            if (i1->flags & ITEM_DELETED && i2->flags & ITEM_DELETED) {
                continue;
            }
            ASSERT_EQ(0, dent_permute(dirfd, i1, i2));
            items[i1->id].ref = i1 - items;
            items[i2->id].ref = i2 - items;
#endif
        }
    }

    ASSERT_EQ(0, dent_check(dirfd, items, n, 1));
    ASSERT_EQ(0, lseek(dirfd, 0, SEEK_SET));
    ASSERT_EQ(0, dent_check(dirfd, items, n, 0));
    status = 0;

  end:
    free(items);
    return status;
}

int
check_fs_dents (
    int   argc,
    char *argv[]
) {
    char const *seed = NULL;
    int n = -1;

    OPT_START(argc, argv, "n:s:")
    OPT_OPT('n') {
        n = atoi(optarg);
        break;
    }
    OPT_OPT('s') {
        seed = optarg;
        break;
    }
    OPT_END

    if (n <= 0) {
        log_printf("bad size %d", n);
        return -1;
    }
    if (argc < 1) {
        log_puts("path not given");
        return -1;
    }
    char const *path = argv[0];

    if (0 != prng_seed_from_hex(seed)) {
        return -1;
    }
    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        log_printf("open: %s: %s", path, strerror(errno));
        return -1;
    }
    int status = do_check_fs_dents(dirfd, n);
    close(dirfd);
    return status;
}
