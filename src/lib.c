/**
 * bookmarkfs/src/lib.c
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

#include "lib.h"

#include <stdio.h>

#include "hash.h"
#include "prng.h"
#include "version.h"

int
bookmarkfs_lib_init (void)
{
    if (0 != prng_seed(NULL)) {
        return -1;
    }
    hash_seed(prng_rand());

    return 0;
}

void
bookmarkfs_print_lib_version (
    char const *prefix
) {
    char features[] = "----";
    uint32_t libver = bookmarkfs_lib_version();
    if (libver & BOOKMARKFS_FEAT_DEBUG) {
        features[0] = '+';
    }
    if (libver & BOOKMARKFS_FEAT_NATIVE_WATCHER) {
        features[1] = '+';
    }
    if (libver & BOOKMARKFS_FEAT_SANDBOX) {
        features[2] = '+';
    }
#ifdef __linux__
    if (libver & BOOKMARKFS_FEAT_SANDBOX_LANDLOCK) {
        features[3] = '+';
    }
#endif
    printf("%sbookmarkfs-util %d.%d.%d\n", prefix,
            bookmarkfs_vernum_to_major(libver),
            bookmarkfs_vernum_to_minor(libver),
            bookmarkfs_vernum_to_patch(libver));
    printf("  %c debug\n",            features[0]);
    printf("  %c native-watcher\n",   features[1]);
    printf("  %c sandbox\n",          features[2]);
#ifdef __linux__
    printf("  %c sandbox-landlock\n", features[3]);
#endif
}
