/**
* bookmarkfs/tests/check_watcher.c
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
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>

#include "check_lib.h"
#include "frontend_util.h"
#include "sandbox.h"
#include "watcher.h"

// Forward declaration start
static int  do_check_watcher  (int, uint32_t);
static void msecs_to_timespec (struct timespec *, unsigned long);
static int  wait_for_watcher  (struct watcher *, struct timespec const *, int);
// Forward declaration end

static int
do_check_watcher (
    int      dirfd,
    uint32_t flags
) {
#define FILE1_NAME  "foo.tmp"
#define FILE2_NAME  "bar.tmp"

#define ASSERT_EQ(val, expr)  ASSERT_EXPR((val) == (expr), goto end;)
#define ASSERT_NE(val, expr)  ASSERT_EXPR((val) != (expr), goto end;)

    struct watcher *w = watcher_create(dirfd, FILE1_NAME, flags);
    if (w == NULL) {
        return -1;
    }

    unsigned long msecs = 100;
    int           tries = 5;
    if (flags & WATCHER_FALLBACK) {
        msecs = 2500;
        tries = 2;
    }
    struct timespec ts;
    msecs_to_timespec(&ts, msecs);

    int status = -1;
    int fd     = -1;

    fd = openat(dirfd, FILE1_NAME, O_WRONLY | O_CREAT, 0600);
    ASSERT_NE(-1, fd);
    // Lazy-init watcher.
    ASSERT_EQ(0, wait_for_watcher(w, &ts, tries));

    ASSERT_NE(-1, write(fd, "foo", 3));
    ASSERT_EQ(0, wait_for_watcher(w, &ts, tries));

    bool check_truncate = true;
#if defined(__FreeBSD__)
    // For kevent() EVFILT_VNODE, ftruncate() only triggers NOTE_ATTRIB,
    // which we don't want to watch.
    check_truncate = flags & WATCHER_FALLBACK;
#endif
    if (check_truncate) {
        ASSERT_EQ(0, ftruncate(fd, 0));
        ASSERT_EQ(0, wait_for_watcher(w, &ts, tries));
    }

    int fd2 = openat(dirfd, FILE2_NAME, O_WRONLY | O_CREAT, 0600);
    ASSERT_NE(-1, fd2);
    close(fd2);

    // FAN_DELETE_SELF won't fire if the watched file
    // is still opened somewhere.
    close(fd);
    fd = -1;
    ASSERT_EQ(0, renameat(dirfd, FILE2_NAME, dirfd, FILE1_NAME));
    ASSERT_EQ(0, wait_for_watcher(w, &ts, tries));

    ASSERT_EQ(0, renameat(dirfd, FILE1_NAME, dirfd, FILE2_NAME));
    ASSERT_EQ(-ENOENT, wait_for_watcher(w, &ts, tries));

    // If the watched file has gone, but managed to come back,
    // the watcher should continue to work.
    ASSERT_EQ(0, renameat(dirfd, FILE2_NAME, dirfd, FILE1_NAME));
    ASSERT_EQ(0, wait_for_watcher(w, &ts, tries));

    ASSERT_EQ(0, unlinkat(dirfd, FILE1_NAME, 0));
    ASSERT_EQ(-ENOENT, wait_for_watcher(w, &ts, tries));

    status = 0;

  end:
    if (fd >= 0) {
        close(fd);
    }
    unlinkat(dirfd, FILE1_NAME, 0);
    unlinkat(dirfd, FILE2_NAME, 0);
    watcher_destroy(w);
    return status;
}

static void
msecs_to_timespec (
    struct timespec *ts_buf,
    unsigned long    millisecs
) {
    ts_buf->tv_sec  = millisecs / 1000;
    ts_buf->tv_nsec = (millisecs % 1000) * 1000000;
}

static int
wait_for_watcher (
    struct watcher        *w,
    struct timespec const *ts,
    int                    retry
) {
    for (int i = 0; i < retry; ++i) {
        clock_nanosleep(CLOCK_MONOTONIC, 0, ts, NULL);

        int status = watcher_poll(w);
        if (status != -EAGAIN) {
            return status;
        }
    }
    return -ETIMEDOUT;
}

int
check_watcher (
    int   argc,
    char *argv[]
) {
    uint32_t flags = 0;
#if defined(__FreeBSD__)
    // Do not enable sandbox on FreeBSD,
    // since the watcher sandbox only grants read access to dirfd,
    // and cap_enter() applies to the entire process.
    flags |= SANDBOX_NOOP << WATCHER_SANDBOX_FLAGS_OFFSET;
#endif

    getopt_foreach(argc, argv, ":f") {
      case 'f':
        flags |= WATCHER_FALLBACK;
        break;

      default:
        return -1;
    }
    argc -= optind;
    if (argc < 1) {
        return -1;
    }
    argv += optind;

    int dirfd = open(argv[0], O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        return -1;
    }
    int status = do_check_watcher(dirfd, flags);
    close(dirfd);
    return status;
}
