/**
 * bookmarkfs/tests/check_fs.c
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
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "check_util.h"
#include "frontend_util.h"

// Forward declaration start
static int dispatch_subcmds (int, char *[]);
static int subcmd_ismount   (int, char *[]);
static int subcmd_sleep     (int, char *[]);
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
    if (0 == strcmp("ismount", cmd)) {
        status = subcmd_ismount(argc, argv);
    } else if (0 == strcmp("sleep", cmd)) {
        status = subcmd_sleep(argc, argv);
    } else if (0 == strcmp("regrw", cmd)) {
        status = check_fs_regrw(argc, argv);
    } else if (0 == strcmp("dents", cmd)) {
        status = check_fs_dents(argc, argv);
    } else if (0 == strcmp("times", cmd)) {
        status = check_fs_times(argc, argv);
    } else {
        log_printf("bad subcmd '%s'", cmd);
    }
    return status;
}

static int
subcmd_ismount (
    int   argc,
    char *argv[]
) {
    if (--argc < 1) {
        log_puts("path not given");
        return -1;
    }
    char const *path = *(++argv);

    int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_PATH
    flags |= O_PATH;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        log_printf("open: %s: %s", path, strerror(errno));
        return -1;
    }
    int status = -1;

    struct stat stat_buf;
    if (0 != fstat(fd, &stat_buf)) {
        log_printf("fstat(): %s", strerror(errno));
        goto end;
    }
    dev_t dev = stat_buf.st_dev;

    if (0 != fstatat(fd, "..", &stat_buf, 0)) {
        log_printf("fstatat(): %s", strerror(errno));
        goto end;
    }
    if (dev != stat_buf.st_dev) {
        status = 0;
    }

  end:
    close(fd);
    return status;
}

/**
 * Sleep milliseconds.
 *
 * This sub-command exists because we cannot portably do so in POSIX shell.
 */
static int
subcmd_sleep (
    int   argc,
    char *argv[]
) {
    if (--argc < 1) {
        log_puts("duration not given");
        return -1;
    }
    char const *duration = *(++argv);

    int millis = atoi(duration);
    if (millis <= 0) {
        log_printf("bad duration %d", millis);
        return -1;
    }
    struct timespec ts = {
        .tv_sec  = millis / 1000,
        .tv_nsec = (millis % 1000) * 1000000
    };
    return clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
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
