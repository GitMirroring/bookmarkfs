/**
 * bookmarkfs/tests/check_fs_times.c
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
#include <unistd.h>

#include "check_util.h"
#include "frontend_util.h"
#include "prng.h"

// Forward declaration start
static int  compare_timespec  (struct timespec, struct timespec);
static int  do_check_fs_times (int, int);
static void usecs_to_timespec (struct timespec *, uint32_t);
// Forward declaration end

static int
compare_timespec (
    struct timespec ts1,
    struct timespec ts2
) {
#define timespec_to_usecs(ts)  ( (ts).tv_sec * 1000000 + (ts).tv_nsec / 1000 )
    int64_t usecs1 = timespec_to_usecs(ts1);
    int64_t usecs2 = timespec_to_usecs(ts2);

    if (usecs1 == usecs2) {
        return 0;
    }
    return usecs2 > usecs1 ? 1 : -1;
}

static int
do_check_fs_times (
    int dirfd,
    int rounds
) {
#define FILE1_NAME  "foo.tmp"
#define FILE2_NAME  "bar.tmp"

#define ASSERT_EQ(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) == r_, goto end;)
#define ASSERT_NE(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) != r_, goto end;)

    int status = -1;

    struct timespec now;
    ASSERT_EQ(0, clock_gettime(CLOCK_REALTIME, &now));

    int fd = openat(dirfd, FILE1_NAME, O_WRONLY | O_CREAT | O_EXCL);
    ASSERT_NE(-1, fd);

    struct stat stat_buf;
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));

    ASSERT_EQ(0, fstat(dirfd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));
    now = stat_buf.st_mtim;

    ASSERT_EQ(11, write(fd, "foo:bar/baz", 11));
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));

    ASSERT_EQ(0, renameat(dirfd, FILE1_NAME, dirfd, FILE2_NAME));
    ASSERT_EQ(0, fstat(dirfd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));
    now = stat_buf.st_mtim;

    ASSERT_EQ(0, ftruncate(fd, 10));
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));

    struct timespec times[2];
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_nsec = UTIME_NOW;
    ASSERT_EQ(0, futimens(dirfd, times));
    ASSERT_EQ(0, fstat(dirfd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));
    now = stat_buf.st_mtim;

    times[0].tv_nsec = UTIME_NOW;
    times[1].tv_nsec = UTIME_OMIT;
    ASSERT_EQ(0, futimens(fd, times));
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_atim));

    ASSERT_EQ(0, clock_gettime(CLOCK_REALTIME, &now));
    ASSERT_NE(-1, compare_timespec(stat_buf.st_atim, now));

    for (int i = 0; i < rounds; ++i) {
        uint64_t bits = prng_rand();
        usecs_to_timespec(&times[0], bits & 0xffffffff);
        usecs_to_timespec(&times[1], bits >> 32);

        ASSERT_EQ(0, futimens(fd, times));
        ASSERT_EQ(0, fstat(fd, &stat_buf));
        ASSERT_EQ(0, compare_timespec(times[0], stat_buf.st_atim));
        ASSERT_EQ(0, compare_timespec(times[1], stat_buf.st_mtim));
    }

    ASSERT_EQ(0, unlinkat(dirfd, FILE2_NAME, 0));
    ASSERT_EQ(0, fstat(dirfd, &stat_buf));
    ASSERT_NE(-1, compare_timespec(now, stat_buf.st_mtim));

    ASSERT_EQ(0, clock_gettime(CLOCK_REALTIME, &now));
    ASSERT_NE(-1, compare_timespec(stat_buf.st_mtim, now));

    status = 0;

  end:
    if (fd >= 0) {
        close(fd);
    }
    return status;
}

static void
usecs_to_timespec (
    struct timespec *ts_buf,
    uint32_t         usecs
) {
    ts_buf->tv_sec  = usecs / 1000000;
    ts_buf->tv_nsec = (usecs % 1000000) * 1000;
}

int
check_fs_times (
    int   argc,
    char *argv[]
) {
    char const *seed = NULL;
    int rounds = -1;

    OPT_START(argc, argv, "r:s:")
    OPT_OPT('r') {
        rounds = atoi(optarg);
        break;
    }
    OPT_OPT('s') {
        seed = optarg;
        break;
    }
    OPT_END

    if (rounds < 0) {
        log_printf("bad rounds cnt %d", rounds);
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
    int status = do_check_fs_times(dirfd, rounds);
    close(dirfd);
    return status;
}
