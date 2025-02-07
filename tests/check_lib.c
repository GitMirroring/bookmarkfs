/**
 * bookmarkfs/tests/check_lib.c
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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "frontend_util.h"
#include "hash.h"
#include "prng.h"

// Forward declaration start
static int    dispatch_subcmds (int, char *[]);
static size_t hash_cb          (void *, void const **);
static int    subcmd_hash      (int, char *[]);
static int    subcmd_prng      (int, char *[]);
// Forward declaration end

static int
dispatch_subcmds (
    int   argc,
    char *argv[]
) {
    if (--argc < 1) {
        return -1;
    }
    char const *cmd = *(++argv);

    int status = -1;
    if (0 == strcmp("hash", cmd)) {
        status = subcmd_hash(argc, argv);
    } else if (0 == strcmp("prng", cmd)) {
        status = subcmd_prng(argc, argv);
    }
    return status;
}

static size_t
hash_cb (
    void        *UNUSED_VAR(user_data),
    void const **buf_ptr
) {
    static char buf[4096];

    *buf_ptr = buf;
    return fread(buf, 1, sizeof(buf), stdin);
}

static int
subcmd_hash (
    int   argc,
    char *argv[]
) {
    unsigned long long seed = 0;

    getopt_foreach(argc, argv, ":s:") {
      case 's':
        seed = strtoull(optarg, NULL, 16);
        break;

      default:
        return -1;
    }

    hash_seed(seed);
    printf("%016" PRIx64 "\n", hash_digestcb(hash_cb, NULL));
    return 0;
}

static int
subcmd_prng (
    int   argc,
    char *argv[]
) {
    static uint64_t seed[4];
    int cnt = 0;
    int n = 0;

    getopt_foreach(argc, argv, ":s:n:") {
      case 's':
        cnt = sscanf(optarg,
                "%16" SCNx64 "%16" SCNx64 "%16" SCNx64 "%16" SCNx64,
                &seed[0], &seed[1], &seed[2], &seed[3]);
        if (cnt != 4) {
            return -1;
        }
        break;

      case 'n':
        n = atoi(optarg);
        break;

      default:
        return -1;
    }

    if (0 != prng_seed(cnt == 0 ? NULL : seed)) {
        return -1;
    }
    for (; n > 0; --n) {
        printf("%016" PRIx64 "\n", prng_rand());
    }
    return 0;
}

int
main (
    int   argc,
    char *argv[]
) {
    int status = EXIT_FAILURE;
    if (0 != dispatch_subcmds(argc, argv)) {
        goto end;
    }
    status = EXIT_SUCCESS;

  end:
    return status;
}
