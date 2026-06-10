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

#include <stdlib.h>
#include <string.h>

#include "check_util.h"

// Forward declaration start
static int dispatch_subcmds (int, char *[]);
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
    if (0 == strcmp("watcher", cmd)) {
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
