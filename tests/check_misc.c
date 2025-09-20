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

#include <sys/uio.h>
#include <unistd.h>

#include "base16.h"
#include "base64.h"
#include "check_util.h"
#include "frontend_util.h"
#include "md5.h"
#include "uuid.h"

typedef ssize_t (buf_rw_cb) (
    void       *user_data,
    void       *dst,
    void const *src,
    size_t      src_len,
    size_t     *src_off_ptr
);

// Forward declaration start
static ssize_t base16_dec_cb (void *, void *, void const *, size_t, size_t *);
static ssize_t base16_enc_cb (void *, void *, void const *, size_t, size_t *);
static ssize_t base64_dec_cb (void *, void *, void const *, size_t, size_t *);
static ssize_t base64_enc_cb (void *, void *, void const *, size_t, size_t *);
static ssize_t md5_digest_cb (void *, void *, void const *, size_t, size_t *);
static ssize_t uuid_dec_cb   (void *, void *, void const *, size_t, size_t *);
static ssize_t uuid_enc_cb   (void *, void *, void const *, size_t, size_t *);

static int buf_rw_all       (char *, char *, size_t, buf_rw_cb *, void *);
static int dispatch_subcmds (int, char *[]);
static int subcmd_base16    (int, char *[]);
static int subcmd_base64    (int, char *[]);
static int subcmd_md5       (void);
static int subcmd_uuid      (int, char *[]);
// Forward declaration end

static ssize_t
base16_dec_cb (
    void       *UNUSED_VAR(user_data),
    void       *dst,
    void const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    size_t rem_len = src_len % 2;
    *src_off_ptr = rem_len;

    if (0 != base16_decode(dst, src, src_len - rem_len)) {
        return -1;
    }
    return src_len / 2;
}

static ssize_t
base16_enc_cb (
    void       *UNUSED_VAR(user_data),
    void       *dst,
    void const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    *src_off_ptr = 0;

    base16_encode(dst, src, src_len);
    return src_len * 2;
}

static ssize_t
base64_dec_cb (
    void       *UNUSED_VAR(user_data),
    void       *dst,
    void const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    size_t rem_len = src_len % 4;
    *src_off_ptr = rem_len;

    if (0 != base64url_decode_nopad(dst, src, src_len - rem_len)) {
        return -1;
    }
    return src_len / 4 * 3;
}

static ssize_t
base64_enc_cb (
    void       *UNUSED_VAR(user_data),
    void       *dst,
    void const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    src_len += *src_off_ptr;
    size_t rem_len = src_len % 3;
    *src_off_ptr = rem_len;

    base64url_encode_nopad(dst, src, src_len - rem_len);
    return src_len / 3 * 4;
}

static ssize_t
md5_digest_cb (
    void       *user_data,
    void       *dst,
    void const *src,
    size_t      src_len,
    size_t     *src_off_ptr
) {
    struct md5_ctx *ctx = user_data;

    debug_assert(*src_off_ptr == 0);
    if (src_len > 0) {
        md5_update(ctx, src, src_len);
        return 0;
    } else {
        md5_digest(ctx, dst);
        return MD5_DIGEST_SIZE;
    }
}

static ssize_t
uuid_dec_cb (
    void       *UNUSED_VAR(user_data),
    void       *dst,
    void const *src,
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

    if (0 != uuid_hex2bin(dst, src)) {
        return -1;
    }
    return UUID_LEN;
}

static ssize_t
uuid_enc_cb (
    void       *UNUSED_VAR(user_data),
    void       *dst,
    void const *src,
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
    for (size_t off = 0; ; ) {
        ssize_t src_len = xread(STDIN_FILENO, src + off, src_max - off);
        if (src_len < 0) {
            return -1;
        }
        ssize_t dst_len = callback(user_data, dst, src, src_len, &off);
        if (dst_len < 0) {
            return -1;
        }
        struct iovec iov = { dst, dst_len };
        if (0 != xwritev(STDOUT_FILENO, &iov, 1)) {
            return -1;
        }
        if (src_len == 0) {
            return off == 0 ? 0 : -1;
        }
        if ((size_t)src_len > off) {
            memmove(src, src + src_len - off, off);
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
    } else if (0 == strcmp("md5", cmd)) {
        status = subcmd_md5();
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
subcmd_md5 (void)
{
    struct md5_ctx ctx;
    md5_init(&ctx);

    char src[4096], dst[MD5_DIGEST_SIZE];
    return buf_rw_all(dst, src, sizeof(src), md5_digest_cb, &ctx);
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
    if (0 != dispatch_subcmds(argc, argv)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
