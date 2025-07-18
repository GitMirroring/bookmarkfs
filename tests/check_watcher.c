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
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>

#include "check_util.h"
#include "frontend_util.h"
#include "sandbox.h"
#include "watcher.h"

// Forward declaration start
static int  do_check_watcher  (int, uint32_t);
static int  wait_for_watcher  (struct watcher *);
// Forward declaration end

static int
do_check_watcher (
    int      dirfd,
    uint32_t flags
) {
#define FILE1_NAME  "foo.tmp"
#define FILE2_NAME  "bar.tmp"

#define ASSERT_EQ(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) == r_, goto end;)
#define ASSERT_NE(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) != r_, goto end;)

    int status = -1;
    int fd     = -1;
    struct watcher *w = NULL;

    fd = openat(dirfd, FILE1_NAME, O_WRONLY | O_CREAT, 0600);
    ASSERT_NE(-1, fd);

    w = watcher_create(dirfd, FILE1_NAME, flags);
    if (w == NULL) {
        goto end;
    }
    // Check for spurious zero returns.
    ASSERT_EQ(-ETIMEDOUT, wait_for_watcher(w));

    ASSERT_NE(-1, write(fd, "foo", 3));
    ASSERT_EQ(0, wait_for_watcher(w));

    bool check_truncate = true;
#if defined(__FreeBSD__)
    // For kevent() EVFILT_VNODE, ftruncate() only triggers NOTE_ATTRIB,
    // which we don't want to watch.
    if (!(flags & WATCHER_FALLBACK)) {
        check_truncate = false;
    }
#endif
    if (check_truncate) {
        ASSERT_EQ(0, ftruncate(fd, 0));
        ASSERT_EQ(0, wait_for_watcher(w));
    }

    int fd2 = openat(dirfd, FILE2_NAME, O_WRONLY | O_CREAT, 0600);
    ASSERT_NE(-1, fd2);
    close(fd2);

    bool close_fd = false;
#if defined(__linux__)
    // FAN_DELETE_SELF won't fire if the watched file
    // is still opened somewhere.
    if (!(flags & WATCHER_FALLBACK)) {
        close_fd = true;
    }
#endif
    if (close_fd) {
        close(fd);
        fd = -1;
    }

    ASSERT_EQ(0, renameat(dirfd, FILE2_NAME, dirfd, FILE1_NAME));
    ASSERT_EQ(0, wait_for_watcher(w));

    ASSERT_EQ(0, renameat(dirfd, FILE1_NAME, dirfd, FILE2_NAME));
    ASSERT_EQ(-ENOENT, wait_for_watcher(w));

    // If the watched file has gone, but managed to come back,
    // the watcher should continue to work.
    ASSERT_EQ(0, renameat(dirfd, FILE2_NAME, dirfd, FILE1_NAME));
    ASSERT_EQ(0, wait_for_watcher(w));

    ASSERT_EQ(0, unlinkat(dirfd, FILE1_NAME, 0));
    ASSERT_EQ(-ENOENT, wait_for_watcher(w));

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

static int
wait_for_watcher (
    struct watcher *w
) {
#ifdef ENABLE_NATIVE_WATCHER
#  define TRY_INTERVAL  { .tv_nsec = 50 * 1000000 }
#  define MAX_TRIES     10
#else
#  define TRY_INTERVAL  { .tv_sec = 2, .tv_nsec = 500 * 1000000 }
#  define MAX_TRIES     2
#endif

    struct timespec const ts = TRY_INTERVAL;
    for (int i = 0; i < MAX_TRIES; ++i) {
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);

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
    if (--argc < 1) {
        log_puts("path not specified");
        return -1;
    }
    char const *path = *(++argv);

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        log_printf("open: %s: %s", path, strerror(errno));
        return -1;
    }

    uint32_t sandbox_flags = 0;
#ifndef ENABLE_SANDBOX_LANDLOCK
    sandbox_flags |= SANDBOX_NO_LANDLOCK;
#endif
#if defined(__FreeBSD__)
    // Do not enable sandbox on FreeBSD,
    // since the watcher sandbox only grants read access to dirfd,
    // and cap_enter() applies to the entire process.
    sandbox_flags |= SANDBOX_NOOP;
#endif

    uint32_t flags = sandbox_flags << WATCHER_SANDBOX_FLAGS_OFFSET;
    int status = do_check_watcher(dirfd, flags);
    close(dirfd);
    return status;
}
