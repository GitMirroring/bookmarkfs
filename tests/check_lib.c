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

#include "check_util.h"
#include "frontend_util.h"
#include "hash.h"
#include "prng.h"

// Forward declaration start
static int dispatch_subcmds (int, char *[]);
static int subcmd_hash      (int, char *[]);
static int subcmd_prng      (int, char *[]);
// Forward declaration end

static int
dispatch_subcmds (
    int   argc,
    char *argv[]
) {
    if (--argc < 1) {
        log_puts("subcmd not given");
        return -1;
    }
    char const *cmd = *(++argv);

    int status = -1;
    if (0 == strcmp("hash", cmd)) {
        status = subcmd_hash(argc, argv);
    } else if (0 == strcmp("prng", cmd)) {
        status = subcmd_prng(argc, argv);
    } else if (0 == strcmp("watcher", cmd)) {
        status = check_watcher(argc, argv);
#ifdef ENABLE_SANDBOX
    } else if (0 == strcmp("sandbox", cmd)) {
        status = check_sandbox(argc, argv);
#endif
    } else if (0 == strcmp("hashmap", cmd)) {
        status = check_hashmap(argc, argv);
    } else {
        log_printf("bad subcmd '%s'", cmd);
    }
    return status;
}

static int
subcmd_hash (
    int   argc,
    char *argv[]
) {
    unsigned long long seed = 0;

    OPT_START(argc, argv, "s:")
    OPT_OPT('s') {
        seed = strtoull(optarg, NULL, 16);
        break;
    }
    OPT_END

    hash_seed(seed);
    for (struct hash_ctx *ctx = hash_init(); ; ) {
        char buf[4096];
        ssize_t len = xread(STDIN_FILENO, buf, sizeof(buf));
        if (len < 0) {
            return -1;
        }
        hash_update(ctx, buf, len);
        if ((size_t)len < sizeof(buf)) {
            printf("%016" PRIx64 "\n", hash_digest(ctx));
            return 0;
        }
    }
}

static int
subcmd_prng (
    int   argc,
    char *argv[]
) {
    int n = 0;

    OPT_START(argc, argv, "n:")
    OPT_OPT('n') {
        n = atoi(optarg);
        break;
    }
    OPT_END

    if (0 != prng_seed_from_env()) {
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
    if (0 != dispatch_subcmds(argc, argv)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
