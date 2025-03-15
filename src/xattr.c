/**
 * bookmarkfs/src/xattr.c
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
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

#include "xattr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#if defined(__linux__)
#  include <sys/xattr.h>
#elif defined(__FreeBSD__)
#  include <sys/extattr.h>
#endif

#include "xstd.h"

#if defined(__linux__)
#  define XATTR_NAME_PREFIX  BOOKMARKFS_XATTR_PREFIX
#elif defined(__FreeBSD__)
#  define XATTR_NAME_PREFIX  ( &BOOKMARKFS_XATTR_PREFIX[strlen("user.")] )
#endif

// Forward declaration start
static ssize_t xattr_do_get         (int, char const *, void *, size_t);
static ssize_t xattr_do_list        (int, void *, size_t);
static int     xattr_do_set         (int, char const *, void const *, size_t);
static char *  xattr_normalize_name (char const *);
// Forward declaration end

static ssize_t
xattr_do_get (
    int         fd,
    char const *name,
    void       *buf,
    size_t      buf_len
) {
    ssize_t result;
#if defined(__linux__)
    result = fgetxattr(fd, name, buf, buf_len);
    if (result < 0) {
        log_printf("fgetxattr(): %s", strerror(errno));
    }
#elif defined(__FreeBSD__)
    result = extattr_get_fd(fd, EXTATTR_NAMESPACE_USER, name, buf, buf_len);
    if (result < 0) {
        log_printf("extattr_get_fd(): %s", strerror(errno));
    }
#else
#  error "not implemented"
#endif
    return result;
}

static ssize_t
xattr_do_list (
    int     fd,
    void   *buf,
    size_t  buf_len
) {
    ssize_t result;
#if defined(__linux__)
    result = flistxattr(fd, buf, buf_len);
    if (result < 0) {
        log_printf("flistxattr(): %s", strerror(errno));
    }
#elif defined(__FreeBSD__)
    result = extattr_list_fd(fd, EXTATTR_NAMESPACE_USER, buf, buf_len);
    if (result < 0) {
        log_printf("extattr_list_fd(): %s", strerror(errno));
    }
#else
#  error "not implemented"
#endif
    return result;
}

static int
xattr_do_set (
    int         fd,
    char const *name,
    void const *buf,
    size_t      buf_len
) {
#if defined(__linux__)
    if (0 != fsetxattr(fd, name, buf, buf_len, 0)) {
        log_printf("fsetxattr(): %s", strerror(errno));
        return -1;
    }
#elif defined(__FreeBSD__)
    if (0 > extattr_set_fd(fd, EXTATTR_NAMESPACE_USER, name, buf, buf_len)) {
        log_printf("extattr_set_fd(): %s", strerror(errno));
        return -1;
    }
#else
#  error "not implemented"
#endif
    return 0;
}

static char *
xattr_normalize_name (
    char const *name
) {
    char const *prefix     = XATTR_NAME_PREFIX;
    size_t      prefix_len = strlen(prefix);
    size_t      name_len   = strlen(name);

    char *new_name = xmalloc(prefix_len + name_len + 1);
    memcpy(new_name, prefix, prefix_len);
    memcpy(new_name + prefix_len, name, name_len + 1);
    return new_name;
}

int
bookmarkfs_xattr_get (
    int                  fd,
    char const          *name,
    bookmarkfs_xattr_cb *callback,
    void                *user_data
) {
    char *real_name = xattr_normalize_name(name);
    void *buf = NULL;
    ssize_t result;
    do {
        result = xattr_do_get(fd, real_name, NULL, 0);
        if (result < 0) {
            break;
        }

        buf = xrealloc(buf, result);
        result = xattr_do_get(fd, real_name, buf, result);
        if (unlikely(result < 0)) {
            if (result == -ERANGE) {
                continue;
            }
            break;
        }

        result = callback(user_data, buf, result);
    } while (0);

    free(real_name);
    free(buf);
    return result;
}

int
bookmarkfs_xattr_list (
    int                  fd,
    bookmarkfs_xattr_cb *callback,
    void                *user_data
) {
    char *buf = NULL;
    ssize_t result;
    do {
        result = xattr_do_list(fd, NULL, 0);
        if (result < 0) {
            goto end;
        }

        buf = xrealloc(buf, result);
        result = xattr_do_list(fd, buf, result);
        if (unlikely(result < 0)) {
            if (result == -ERANGE) {
                continue;
            }
            goto end;
        }
    } while (0);

    char const *prefix     = XATTR_NAME_PREFIX;
    size_t      prefix_len = strlen(prefix);
    for (char *curr = buf, *end = curr + result; curr < end; ) {
        char   *name;
        size_t  name_len;
#if defined(__linux__)
        name     = curr;
        name_len = strlen(name);
        curr += name_len + 1;
#elif defined(__FreeBSD__)
        name_len = (unsigned char)(*(curr++));
        name     = curr;
        curr += name_len;
#else
#  error "not implemented"
#endif
        if (name_len <= prefix_len || 0 != memcmp(prefix, name, prefix_len)) {
            continue;
        }
        result = callback(user_data, name + prefix_len, name_len - prefix_len);
        if (result != 0) {
            goto end;
        }
    }

  end:
    free(buf);
    return result;
}

int
bookmarkfs_xattr_open (
    char const *path
) {
    int flags = O_RDONLY;
#if defined(__FreeBSD__)
    // Linux does not accept O_PATH fd for f*xattr() calls.
    flags |= O_PATH;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        log_printf("open(): %s: %s", path, strerror(errno));
        return -1;
    }
    return fd;
}

int
bookmarkfs_xattr_set (
    int         fd,
    char const *name,
    void const *buf,
    size_t      buf_len
) {
    char *real_name = xattr_normalize_name(name);
    int status = xattr_do_set(fd, real_name, buf, buf_len);
    free(real_name);
    return status;
}
