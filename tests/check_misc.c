/**
* bookmarkfs/tests/check_misc.c
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

#include <unistd.h>

#include "base16.h"
#include "base64.h"
#include "check_util.h"
#include "frontend_util.h"
#include "uuid.h"

typedef ssize_t (buf_rw_cb) (
    void       *user_data,
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
);

// Forward declaration start
static ssize_t base16_dec_cb (void *, char *, char const *, size_t, size_t *);
static ssize_t base16_enc_cb (void *, char *, char const *, size_t, size_t *);
static ssize_t base64_dec_cb (void *, char *, char const *, size_t, size_t *);
static ssize_t base64_enc_cb (void *, char *, char const *, size_t, size_t *);
static ssize_t uuid_dec_cb   (void *, char *, char const *, size_t, size_t *);
static ssize_t uuid_enc_cb   (void *, char *, char const *, size_t, size_t *);

static int buf_rw_all       (char *, char *, size_t, buf_rw_cb *, void *);
static int dispatch_subcmds (int, char *[]);
static int subcmd_base16    (int, char *[]);
static int subcmd_base64    (int, char *[]);
static int subcmd_uuid      (int, char *[]);
// Forward declaration end

static ssize_t
base16_dec_cb (
    void       *UNUSED_VAR(user_data),
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    size_t rem_len = src_len % 2;
    *src_off_ptr = rem_len;

    if (0 != base16_decode((uint8_t *)dst, src, src_len - rem_len)) {
        return -1;
    }
    return src_len / 2;
}

static ssize_t
base16_enc_cb (
    void       *UNUSED_VAR(user_data),
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    *src_off_ptr = 0;

    base16_encode(dst, (uint8_t *)src, src_len);
    return src_len * 2;
}

static ssize_t
base64_dec_cb (
    void       *UNUSED_VAR(user_data),
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    size_t rem_len = src_len % 4;
    *src_off_ptr = rem_len;

    if (0 != base64url_decode_nopad((uint8_t *)dst, src, src_len - rem_len)) {
        return -1;
    }
    return src_len / 4 * 3;
}

static ssize_t
base64_enc_cb (
    void       *UNUSED_VAR(user_data),
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    size_t rem_len = src_len % 3;
    *src_off_ptr = rem_len;

    base64url_encode_nopad(dst, (uint8_t *)src, src_len - rem_len);
    return src_len / 3 * 4;
}

static ssize_t
uuid_dec_cb (
    void       *UNUSED_VAR(user_data),
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    if (src_len < UUID_HEX_LEN) {
        *src_off_ptr = src_len;
        return 0;
    }
    debug_assert(src_len == UUID_HEX_LEN);
    *src_off_ptr = 0;

    if (0 != uuid_hex2bin((uint8_t *)dst, src)) {
        return -1;
    }
    return UUID_LEN;
}

static ssize_t
uuid_enc_cb (
    void       *UNUSED_VAR(user_data),
    char       *dst,
    char const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    if (src_len < UUID_LEN) {
        *src_off_ptr = src_len;
        return 0;
    }
    debug_assert(src_len == UUID_LEN);
    *src_off_ptr = 0;

    uuid_bin2hex(dst, (uint8_t *)src);
    return UUID_HEX_LEN;
}

static int
buf_rw_all (
    char      *dst,
    char      *src,
    size_t     src_max,
    buf_rw_cb *callback,
    void      *user_data
) {
    for (size_t src_off = 0; ; ) {
        ssize_t src_len = read(STDIN_FILENO, src + src_off, src_max - src_off);
        if (src_len < 0) {
            return -1;
        }
        ssize_t dst_max = callback(user_data, dst, src, src_len, &src_off);
        if (dst_max < 0) {
            return -1;
        }
        for (size_t dst_off = 0; dst_max > 0; ) {
            ssize_t dst_len = write(STDOUT_FILENO, dst + dst_off, dst_max);
            if (dst_len <= 0) {
                return -1;
            }
            dst_off += dst_len;
            dst_max -= dst_len;
        }
        if (src_len == 0) {
            return src_off == 0 ? 0 : -1;
        }
        if ((size_t)src_len > src_off) {
            memmove(src, src + src_len - src_off, src_off);
        }
    }
}

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
    if (0 == strcmp("base64", cmd)) {
        status = subcmd_base64(argc, argv);
    } else if (0 == strcmp("base16", cmd)) {
        status = subcmd_base16(argc, argv);
    } else if (0 == strcmp("uuid", cmd)) {
        status = subcmd_uuid(argc, argv);
    } else {
        log_printf("bad subcmd '%s'", cmd);
    }
    return status;
}

static int
subcmd_base16 (
    int   argc,
    char *argv[]
) {
    int decode = 0;

    OPT_START(argc, argv, "d")
    OPT_OPT('d') {
        decode = 1;
        break;
    }
    OPT_END

    if (decode) {
        char src[4096], dst[2048];
        return buf_rw_all(dst, src, sizeof(src), base16_dec_cb, NULL);
    } else {
        char src[2048], dst[4096];
        return buf_rw_all(dst, src, sizeof(src), base16_enc_cb, NULL);
    }
}

static int
subcmd_base64 (
    int   argc,
    char *argv[]
) {
    int decode = 0;

    OPT_START(argc, argv, "d")
    OPT_OPT('d') {
        decode = 1;
        break;
    }
    OPT_END

    if (decode) {
        char src[4096], dst[3072];
        return buf_rw_all(dst, src, sizeof(src), base64_dec_cb, NULL);
    } else {
        char src[3072], dst[4096];
        return buf_rw_all(dst, src, sizeof(src), base64_enc_cb, NULL);
    }
}

static int
subcmd_uuid (
    int   argc,
    char *argv[]
) {
    int decode = 0;

    OPT_START(argc, argv, "d")
    OPT_OPT('d') {
        decode = 1;
        break;
    }
    OPT_END

    if (decode) {
        char src[UUID_HEX_LEN], dst[UUID_LEN];
        return buf_rw_all(dst, src, sizeof(src), uuid_dec_cb, NULL);
    } else {
        char src[UUID_LEN], dst[UUID_HEX_LEN];
        return buf_rw_all(dst, src, sizeof(src), uuid_enc_cb, NULL);
    }
}

int
main (
    int   argc,
    char *argv[]
) {
    int status = EXIT_FAILURE;
    if (0 != dispatch_subcmds(argc, argv)) {
        goto end;
    }
    status = EXIT_SUCCESS;

  end:
    return status;
}
