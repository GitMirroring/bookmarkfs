/**
 * bookmarkfs/src/json.c
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

#include "json.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "prng.h"
#include "xstd.h"

#define DUMP_BUFSIZE  ( 32 * 1024 )
struct dump_ctx {
    int    fd;
    size_t blksize;
    size_t data_len;
    char   buf[DUMP_BUFSIZE];
};

// Forward declaration start
static int  dump_cb   (char const *, size_t, void *);
static int  write_iov (int, struct iovec *, int);
// Forward declaration end

static int
dump_cb (
    char const *buf,
    size_t      buf_len,
    void       *user_data
) {
    struct dump_ctx *ctx = user_data;

    size_t new_len = ctx->data_len + buf_len;
    if (new_len <= DUMP_BUFSIZE) {
        memcpy(ctx->buf + ctx->data_len, buf, buf_len);
        ctx->data_len = new_len;
        return 0;
    }
    new_len %= ctx->blksize;
    buf_len -= new_len;

    struct iovec bufv[] = {
        { .iov_base = ctx->buf,    .iov_len = ctx->data_len },
        { .iov_base = (char *)buf, .iov_len = buf_len       },
    };
    if (0 != write_iov(ctx->fd, bufv, 2)) {
        return -1;
    }
    memcpy(ctx->buf, buf + buf_len, new_len);
    ctx->data_len = new_len;
    return 0;
}

static int
write_iov (
    int           fd,
    struct iovec *bufv,
    int           bufcnt
) {
    while (1) {
        ssize_t nbytes = writev(fd, bufv, bufcnt);
        if (unlikely(nbytes < 0)) {
            int err = errno;
            log_printf("writev(): %s", xstrerror(err));

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

        while ((size_t)nbytes >= bufv->iov_len) {
            nbytes -= (bufv++)->iov_len;
            if (--bufcnt == 0) {
                return 0;
            }
        }
        bufv->iov_base = (char *)(bufv->iov_base) + nbytes;
        bufv->iov_len -= nbytes;
    }
}

size_t
json_array_search (
    json_t const *haystack,
    json_t const *needle
) {
    size_t idx;
#ifdef BOOKMARKFS_DEBUG
    size_t cnt = json_array_size(haystack);
#endif
    for (idx = 0; needle != json_array_get(haystack, idx); ++idx) {
#ifdef BOOKMARKFS_DEBUG
        xassert(idx < cnt);
#endif
    }
    return idx;
}

int
json_dump_file_at (
    json_t const *json,
    int           dirfd,
    char const   *name,
    size_t        flags
) {
#define TMPNAME_MAX  ( NAME_MAX + 21 )  // strlen(".0123456789abcdef.tmp")
    char buf[TMPNAME_MAX + 1];

  again:  ;
    int name_len = snprintf(buf, TMPNAME_MAX + 1, "%s.%016" PRIx64 ".tmp",
            name, prng_rand());
    if (unlikely(name_len > TMPNAME_MAX || name_len < 0)) {
        errno = ENAMETOOLONG;
        goto open_fail;
    }
    char const *tmp_name = buf;
    if (name_len > NAME_MAX) {
        tmp_name += NAME_MAX - name_len;
    }

    int fd = openat(dirfd, tmp_name,
            O_CREAT | O_WRONLY | O_EXCL | O_RESOLVE_BENEATH, 0600);
    if (fd < 0) {
        if (unlikely(errno == EEXIST)) {
            goto again;
        }

      open_fail:
        log_printf("openat(): %s", xstrerror(errno));
        return -1;
    }
    int status = -1;

    if (0 != json_dumpfd_ex(json, fd, flags)) {
        goto fail;
    }
    if (0 != renameat(dirfd, tmp_name, dirfd, name)) {
        log_printf("renameat(): %s", xstrerror(errno));
        goto fail;
    }
    if (unlikely(0 != xfsync(dirfd))) {
        goto fail;
    }
    status = 0;
    goto end;

  fail:
    unlinkat(dirfd, tmp_name, 0);

  end:
    close(fd);
    return status;
}

int
json_dumpfd_ex (
    json_t const *json,
    int           fd,
    size_t        flags
) {
    struct stat stat_buf;
    if (unlikely(0 != fstat(fd, &stat_buf))) {
        log_printf("fstat(): %s", xstrerror(errno));
        return -1;
    }
    size_t blksize = stat_buf.st_blksize;
    if (unlikely(blksize == 0 || DUMP_BUFSIZE % blksize != 0)) {
        debug_printf("unexpected st_blksize: %zu", blksize);
        blksize = DUMP_BUFSIZE;
    }

    struct dump_ctx ctx;
    ctx.fd       = fd,
    ctx.blksize  = blksize,
    ctx.data_len = 0;
    if (0 != json_dump_callback(json, dump_cb, &ctx, flags)) {
        log_puts("json_dump_callback() failed");
        return -1;
    }
    if (ctx.data_len > 0) {
        struct iovec buf = {
            .iov_base = ctx.buf,
            .iov_len  = ctx.data_len,
        };
        if (0 != write_iov(fd, &buf, 1)) {
            return -1;
        }
    }
    if (unlikely(0 != xfsync(fd))) {
        return -1;
    }
    return 0;
}

json_t *
json_load_file_at (
    int         dirfd,
    char const *name,
    size_t      flags
) {
    int fd = openat(dirfd, name, O_RDONLY | O_RESOLVE_BENEATH);
    if (fd < 0) {
        log_printf("openat(): %s", xstrerror(errno));
        return NULL;
    }
    json_t *result = NULL;

    struct stat stat_buf;
    if (unlikely(0 != fstat(fd, &stat_buf))) {
        log_printf("fstat(): %s", xstrerror(errno));
        goto end;
    }
    void *buf = mmap(NULL, stat_buf.st_size, PROT_READ | PROT_MAX(PROT_READ),
            MAP_PRIVATE, fd, 0);
    if (unlikely(buf == MAP_FAILED)) {
        log_printf("mmap(): %s", xstrerror(errno));
        goto end;
    }
    xassert(0 == posix_madvise(buf, stat_buf.st_size, POSIX_MADV_SEQUENTIAL));

    json_error_t err;
    // XXX: If the file is truncated before json_loadb() returns,
    // SIGBUS may be delivered, terminating the process.
    //
    // A possible workaround is to siglongjmp() back from a signal handler,
    // and release any memory allocated from within json_loadb().
    //
    // We choose not to implement such hacks, since the common practice
    // (that Chromium and we ourselves are doing for bookmark files)
    // is to write to a temporary file and then rename to the target path,
    // which does not cause the aforementioned problem.
    result = json_loadb(buf, stat_buf.st_size, flags, &err);
    if (result == NULL) {
        log_printf("json_loadb(): %s", err.text);
    }
    munmap(buf, stat_buf.st_size);

  end:
    close(fd);
    return result;
}
