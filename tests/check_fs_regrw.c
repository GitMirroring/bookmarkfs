/**
 * bookmarkfs/tests/check_fs_regrw.c
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

#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include "check_util.h"
#include "frontend_util.h"
#include "prng.h"

// Forward declaration start
static int   check_file_data   (int, void *, off_t);
static off_t check_rand_rw     (int, uint64_t *, size_t);
static int   do_check_fs_regrw (char const *, size_t);
// Forward declaration end

static int
check_file_data (
    int    fd,
    void  *buf,
    off_t  file_size
) {
    void *file_data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_data == MAP_FAILED) {
        log_puts("mmap() failed");
        return -1;
    }
    int result = memcmp(file_data, buf, file_size);

    munmap(file_data, file_size);
    return result;
}

static off_t
check_rand_rw (
    int       fd,
    uint64_t *buf,
    size_t    items
) {
#define MAX_CNT  2048
    uint64_t tmp_buf[MAX_CNT];

    off_t file_size = 0;
    for (size_t i = 0; i < items; ++i) {
        uint64_t bits = prng_rand();
        size_t   off  = bits % items;
        size_t   cnt  = (bits >> 32) % MAX_CNT + 1;
        if (off + cnt > items) {
            cnt = items - off;
        }
        ssize_t obj_bytes = sizeof(uint64_t) * cnt;
        off_t   off_bytes = sizeof(uint64_t) * off;

        if (off_bytes + obj_bytes <= file_size) {
            if (obj_bytes != pread(fd, tmp_buf, obj_bytes, off_bytes)) {
                log_puts("pread() failed");
                return -1;
            }
            if (0 != memcmp(tmp_buf, buf + off, obj_bytes)) {
                log_puts("data not match");
                return -1;
            }
        }

        for (size_t j = off; j < off + cnt; ++j) {
            buf[j] = prng_rand();
        }
        if (obj_bytes != pwrite(fd, buf + off, obj_bytes, off_bytes)) {
            log_puts("pwrite() failed");
            return -1;
        }
        if (file_size < off_bytes + obj_bytes) {
            file_size = off_bytes + obj_bytes;
        }
    }
    return file_size;
}

static int
do_check_fs_regrw (
    char const *path,
    size_t      file_max
) {
#define ASSERT_EQ(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) == r_, goto end;)
#define ASSERT_NE(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) != r_, goto end;)

    void *buf = mmap(NULL, file_max, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        log_puts("failed to allocate memory");
        return -1;
    }
    int status = -1;

    // Random read/write testing is less useful without O_DIRECT,
    // due to the page cache.
#ifndef O_DIRECT
#  define O_DIRECT  0
#endif
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0600);
    ASSERT_NE(-1, fd);

    struct stat stat_buf;
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_EQ(0, stat_buf.st_size);

    off_t nbytes = check_rand_rw(fd, buf, file_max / sizeof(uint64_t));
    ASSERT_NE(-1, nbytes);
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_EQ(nbytes, stat_buf.st_size);

    // File data must be valid URI to be persisted to bookmark storage.
    memcpy(buf, "foo:bar/", 8);
    uint8_t lut[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
        "0123456789" "-_";
    for (uint8_t *b = (uint8_t *)buf + 8; b < (uint8_t *)buf + nbytes; ++b) {
        *b = lut[*b & 0x3f];
    }
    ASSERT_EQ(nbytes, pwrite(fd, buf, nbytes, 0));
    ASSERT_EQ(0, fsync(fd));

    // Re-open the file and check if data is still valid.
    close(fd);
    fd = open(path, O_RDONLY | O_DIRECT);
    ASSERT_NE(-1, fd);
    ASSERT_EQ(0, fstat(fd, &stat_buf));
    ASSERT_EQ(nbytes, stat_buf.st_size);
    ASSERT_EQ(0, check_file_data(fd, buf, nbytes));

    status = 0;

  end:
    if (fd >= 0) {
        close(fd);
    }
    munmap(buf, file_max);
    return status;
}

int
check_fs_regrw (
    int   argc,
    char *argv[]
) {
    char const *seed = NULL;
    int file_max = -1;

    OPT_START(argc, argv, "n:s:")
    OPT_OPT('n') {
        file_max = atoi(optarg);
        break;
    }
    OPT_OPT('s') {
        seed = optarg;
        break;
    }
    OPT_END

    if (file_max <= 0 || file_max % sizeof(uint64_t) != 0) {
        log_printf("bad size %d", file_max);
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
    return do_check_fs_regrw(path, file_max);
}
