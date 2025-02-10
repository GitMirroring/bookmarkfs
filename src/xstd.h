/**
* bookmarkfs/src/xstd.h
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

#ifndef BOOKMARKFS_XSTD_H_
#define BOOKMARKFS_XSTD_H_

#include <stdio.h>

#include "defs.h"

#ifdef HAVE___BUILTIN_EXPECT
#  define likely(e)    __builtin_expect(!!(e), 1)
#  define unlikely(e)  __builtin_expect(!!(e), 0)
#else
#  define likely(e)    (e)
#  define unlikely(e)  (e)
#endif  /* defined(HAVE___BUILTIN_EXPECT) */

#ifdef BOOKMARKFS_DEBUG
#  define xassert(e)  if (unlikely(!(e))) xabort_(#e, FILE_NAME_, __LINE__)
#else
#  define xassert(e)  if (unlikely(!(e))) xabort_(FILE_NAME_, __LINE__)
#endif

#ifdef BOOKMARKFS_DEBUG
#  define debug_assert(e)  xassert(e)
#  define unreachable()    xassert(0)
#else  /* !defined(BOOKMARKFS_DEBUG) */
#  define debug_assert(e)  if (!(e)) { unreachable(); }
#  ifdef HAVE___BUILTIN_UNREACHABLE
#    define unreachable()  __builtin_unreachable()
#  else
#    define unreachable()
#  endif
#endif  /* defined(BOOKMARKFS_DEBUG) */

#define log_printf_(f, ...)  \
    fprintf(stderr, "%s:" f "\n", FILE_NAME_, __VA_ARGS__)

#ifdef BOOKMARKFS_DEBUG
#  define log_printf(f, ...)  log_printf_("%d: " f, __LINE__, __VA_ARGS__)
#else
#  define log_printf(f, ...)  log_printf_(" " f, __VA_ARGS__);
#endif
#define log_puts(s)  log_printf("%s", s)

#ifdef BOOKMARKFS_DEBUG
#  define debug_printf(f, ...)  log_printf("[debug] " f, __VA_ARGS__)
#  define debug_puts(s)         log_puts("[debug] " s)
#else
#  define debug_printf(f, ...)
#  define debug_puts(s)
#endif

#if defined(__FreeBSD__) || (defined(__linux__) && defined(_GNU_SOURCE))
#  define xasprintf(strp, ...)  xassert(0 <= asprintf(strp, __VA_ARGS__))
#else
#  define xasprintf(strp, ...)                           \
    do {                                                 \
        int len_ = snprintf(NULL, 0, __VA_ARGS__);       \
        xassert(len_ >= 0);                              \
        *(strp) = xmalloc(len_ + 1);                     \
        xassert(len_ == sprintf(*(strp), __VA_ARGS__));  \
    } while (0)
#endif

/**
 * Prints a message to standard error, and then aborts.
 */
BOOKMARKFS_INTERNAL
FUNCATTR_COLD FUNCATTR_NORETURN
void
xabort_ (
#ifdef BOOKMARKFS_DEBUG
    char const *assertion,
#endif
    char const *name,
    int         line
);

/**
 * Like calloc(), but never returns NULL.
 *
 * On failure, the calling process aborts.
 */
BOOKMARKFS_INTERNAL
FUNCATTR_MALLOC FUNCATTR_RETURNS_NONNULL
void *
xcalloc (
    size_t nmemb,
    size_t size
);

/**
 * Like fsync(), but retries on EINTR, and aborts on EIO.
 */
BOOKMARKFS_INTERNAL
int
xfsync (
    int fd
);

/**
 * Like malloc(), but never returns NULL.
 *
 * On failure, the calling process aborts.
 */
BOOKMARKFS_INTERNAL
FUNCATTR_MALLOC FUNCATTR_RETURNS_NONNULL
void *
xmalloc (
    size_t size
);

/**
 * Like pipe2(), using pipe()+fcntl() on platforms without pipe2().
 *
 * The only supported flag is O_CLOEXEC.
 */
BOOKMARKFS_INTERNAL
int
xpipe2 (
    int pipefd[2],
    int flags
);

/**
 * Like realloc(), but never returns NULL.
 *
 * On failure, the calling process aborts.
 */
BOOKMARKFS_INTERNAL
FUNCATTR_RETURNS_NONNULL
void *
xrealloc (
    void   *p,
    size_t  size
);

/**
 * Like strerror(), but MT-Safe.
 *
 * NOTE: strerror() is MT-Safe in some implementations (e.g., glibc >= 2.32),
 *       but POSIX does not enforce that.
 */
BOOKMARKFS_INTERNAL
FUNCATTR_RETURNS_NONNULL
char const *
xstrerror (
    int errnum
);

#endif  /* !defined(BOOKMARKFS_XSTD_H_) */
