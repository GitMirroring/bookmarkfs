/**
 * bookmarkfs/src/xstd.c
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

#include "xstd.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/syscall.h>
#include <unistd.h>

void
xabort_ (
#ifdef ENABLE_BOOKMARKFS_DEBUG
    char const *assertion,
#endif
    char const *name,
    int         line
) {
#ifdef ENABLE_BOOKMARKFS_DEBUG
    fprintf(stderr, "%s:%d: assertion (%s) failed\n", name, line, assertion);
#else
    fprintf(stderr, "%s:%d: assertion failed\n", name, line);
#endif
    abort();
}

void *
xcalloc (
    size_t nmemb,
    size_t size
) {
    void *p = calloc(nmemb, size);
    xassert(p != NULL);

    return p;
}

int
xfsync (
    int fd
) {
    while (unlikely(0 != fsync(fd))) {
        int err;
        log_printf("fsync(): %s", xstrerror_save(&err));

        switch (err) {
          case EIO:
#ifdef __FreeBSD__
          case EINTEGRITY:
#endif
            abort();

          case EINTR:
            continue;

          default:
            return -1;
        }
    }
    return 0;
}

ssize_t
xgetdents (
    int     fd,
    void   *buf,
    size_t  bufsz
) {
#if defined(__linux__)
    // Definition of `struct linux_dirent64` is equivalent to
    // the `struct dirent` defined in dirent.h,
    // since we mandate 64-bit off_t (, ino_t, ...).
    return syscall(SYS_getdents64, fd, buf, bufsz);
#elif defined(__FreeBSD__)
    return getdents(fd, buf, bufsz);
#else
#  error "not implemented"
#endif
}

void
xgetrealtime (
    struct timespec *ts
) {
    if (0 != clock_gettime(CLOCK_REALTIME, ts)) {
        log_printf("clock_gettime(): %s", xstrerror(errno));
        abort();
    }
}

void *
xmalloc (
    size_t size
) {
    void *p = malloc(size);
    xassert(p != NULL);

    return p;
}

int
xpipe2 (
    int pipefd[2],
    int flags
) {
#if defined(__FreeBSD__) || (defined(__linux__) && defined(_GNU_SOURCE))
    if (0 != pipe2(pipefd, flags)) {
        log_printf("pipe2(): %s", xstrerror(errno));
        return -1;
    }
    return 0;

#else
    if (0 != pipe(pipefd)) {
        log_printf("pipe(): %s", xstrerror(errno));
        return -1;
    }

    int extra_fdflags = 0;
    if (flags & O_CLOEXEC) {
        extra_fdflags |= FD_CLOEXEC;
    }
    for (int i = 0; i < 2; ++i) {
        int fd = pipefd[i];

        int fdflags = fcntl(fd, F_GETFD);
        if (fdflags < 0) {
            goto fail;
        }
        if (0 != fcntl(fd, F_SETFD, fdflags | extra_fdflags)) {
            goto fail;
        }
    }
    return 0;

  fail:
    log_printf("fcntl(): %s", xstrerror(errno));
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;

#endif
}

void *
xrealloc (
    void   *p,
    size_t  size
) {
    p = realloc(p, size);
    xassert(p != NULL);

    return p;
}

char const *
xstrerror (
    int err_num
) {
    locale_t loc = uselocale((locale_t)0);
    xassert(loc != (locale_t)0);

    locale_t loc_copy = loc;
    if (loc == LC_GLOBAL_LOCALE) {
        loc_copy = duplocale(loc);
        xassert(loc_copy != (locale_t)0);
    }
    char const *err_str = strerror_l(err_num, loc_copy);

    if (loc == LC_GLOBAL_LOCALE) {
        freelocale(loc_copy);
    }
    return err_str;
}

char const *
xstrerror_save (
    int *errnum_ptr
) {
    int err = errno;

    *errnum_ptr = err;
    return xstrerror(err);
}
