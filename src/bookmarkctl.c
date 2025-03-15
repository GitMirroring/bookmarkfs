/**
 * bookmarkfs/src/bookmarkctl.c
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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include "frontend_util.h"
#include "fsck_util.h"
#include "ioctl.h"
#include "macros.h"
#include "version.h"
#include "xattr.h"
#include "xstd.h"

#define BMCTL_XATTR_GET_NOEOL   (1u << 0)
#define BMCTL_XATTR_GET_BINARY  (1u << 1)
#define BMCTL_XATTR_GET_MULTI   (1u << 2)
#define BMCTL_XATTR_GET_QUIET   (1u << 3)

struct xattr_get_ctx {
    char const *prefix;
    char        sep;
    char        eol;
    uint32_t    flags;
};

// Forward declaration start
static int  dispatch_subcmds  (int, char *[]);
static void print_help        (void);
static void print_version     (void);
static int  subcmd_fsck       (int, char *[]);
static int  subcmd_permd      (int, char *[]);
static int  subcmd_xattr_get  (int, char *[]);
static int  subcmd_xattr_list (int, char *[]);
static int  subcmd_xattr_set  (int, char *[]);
static int  xattr_get_cb      (void *, void *, size_t);
static int  xattr_get_one     (char const *, char *[], int,
                               struct xattr_get_ctx *);
static int  xattr_list_cb     (void *, void *, size_t);
// Forward declaration end

static int
dispatch_subcmds (
    int   argc,
    char *argv[]
) {
    if (--argc < 1) {
        log_puts("command not given; run 'bookmarkctl help' for usage");
        return -1;
    }
    char const *cmd = *(++argv);

    int status = 0;
    if (0 == strcmp("permd", cmd)) {
        status = subcmd_permd(argc, argv);
    } else if (0 == strcmp("xattr-get", cmd)) {
        status = subcmd_xattr_get(argc, argv);
    } else if (0 == strcmp("xattr-set", cmd)) {
        status = subcmd_xattr_set(argc, argv);
    } else if (0 == strcmp("xattr-list", cmd)) {
        status = subcmd_xattr_list(argc, argv);
    } else if (0 == strcmp("fsck", cmd)) {
        status = subcmd_fsck(argc, argv);
    } else if (0 == strcmp("help", cmd)) {
        print_help();
    } else if (0 == strcmp("version", cmd)) {
        print_version();
    } else {
        log_printf("bad command '%s'; run 'bookmarkctl help' for usage", cmd);
        status = -1;
    }
    return status;
}

static void
print_help (void)
{
    puts("Usage: bookmarkctl <cmd> [args]\n"
            "\n"
            "Main commands:\n"
            "  permd         Permute directory entries\n"
            "  fsck          Check filesystem\n"
            "  xattr-list    List extended attribute names\n"
            "  xattr-get     Get extended attribute value\n"
            "  xattr-set     Set extended attribute value\n"
            "\n"
            "Other commands:\n"
            "  help       Print help message\n"
            "  version    Print version information\n"
            "\n"
            "See the bookmarkctl(1) manpage for more information,\n"
            "or run 'info bookmarkfs' for the full user manual.\n"
            "\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.");
}

static void
print_version (void)
{
    printf("bookmarkctl (BookmarkFS) %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
}

static int
subcmd_fsck (
    int   argc,
    char *argv[]
) {
    if (--argc != 1) {
        if (argc == 0) {
            log_puts("fsck: pathname must be specified");
        } else {
            log_puts("fsck: too many arguments");
        }
        return -1;
    }
    char const *path = *(++argv);

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        log_printf("open(): %s: %s", path, strerror(errno));
        return -1;
    }

    int status = -1;
    do {
        struct bookmarkfs_fsck_data fsck_data;
        status = ioctl(dirfd, BOOKMARKFS_IOC_FSCK_NEXT, &fsck_data);
        if (status < 0) {
            log_printf("ioctl(): %s", strerror(errno));
            break;
        }
        if (0 != explain_fsck_result(status, &fsck_data)) {
            break;
        }
    } while (status != BOOKMARKFS_FSCK_RESULT_END);

    close(dirfd);
    return status;

}

static int
subcmd_permd (
    int   argc,
    char *argv[]
) {
    int op = -1;

    OPT_START(argc, argv, "sba")
    OPT_OPT('s') {
        op = BOOKMARKFS_PERMD_OP_SWAP;
        break;
    }
    OPT_OPT('b') {
        op = BOOKMARKFS_PERMD_OP_MOVE_BEFORE;
        break;
    }
    OPT_OPT('a') {
        op = BOOKMARKFS_PERMD_OP_MOVE_AFTER;
        break;
    }
    OPT_END

    if (op < 0) {
        log_puts("permd: no operation specified");
        return -1;
    }
    struct bookmarkfs_permd_data permd_data;
    permd_data.op = op;

    if (argc != 3) {
        if (argc < 3) {
            log_puts("permd: dentry names and dir path must be specified");
        } else {
            log_puts("permd: too many arguments");
        }
        return -1;
    }
    char const *name1 = argv[0];
    char const *name2 = argv[1];
    char const *path  = argv[2];

#define COPY_NAME(dst, src)                                        \
    if ((dst) + sizeof(dst) == stpncpy(dst, src, sizeof(dst))) {   \
        log_printf("permd: %s: %s", src, strerror(ENAMETOOLONG));  \
        return -1;                                                 \
    }
    COPY_NAME(permd_data.name1, name1);
    COPY_NAME(permd_data.name2, name2);

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        log_printf("open(): %s: %s", path, strerror(errno));
        return -1;
    }

    int status = ioctl(dirfd, BOOKMARKFS_IOC_PERMD, &permd_data);
    if (status < 0) {
        log_printf("ioctl(): %s", strerror(errno));
    }
    close(dirfd);
    return status;
}

static int
subcmd_xattr_get (
    int   argc,
    char *argv[]
) {
    struct xattr_get_ctx ctx = {
        .sep = '\t',
        .eol = '\n',
    };

    OPT_START(argc, argv, "n:Nbmqs:")
    OPT_OPT('n') {
        ctx.eol = optarg[0];
        break;
    }
    OPT_OPT('N') {
        ctx.flags |= BMCTL_XATTR_GET_NOEOL;
        break;
    }
    OPT_OPT('b') {
        ctx.flags |= BMCTL_XATTR_GET_BINARY;
        break;
    }
    OPT_OPT('m') {
        ctx.flags |= BMCTL_XATTR_GET_MULTI;
        break;
    }
    OPT_OPT('q') {
        ctx.flags |= BMCTL_XATTR_GET_QUIET;
        break;
    }
    OPT_OPT('s') {
        ctx.sep = optarg[0];
        break;
    }
    OPT_NOVAL
    OPT_END

    if (argc < 2) {
        log_puts("xattr-get: xattr name and file path must be specified");
        return -1;
    }

    if (ctx.flags & BMCTL_XATTR_GET_MULTI) {
        return xattr_get_one(argv[argc - 1], argv, argc - 1, &ctx);
    }
    for (int i = 1; i < argc; ++i) {
        char const *path = argv[i];
        if (0 != xattr_get_one(path, argv, 1, &ctx)) {
            return -1;
        }
    }
    return 0;
}

static int
subcmd_xattr_list (
    int   argc,
    char *argv[]
) {
    if (--argc != 1) {
        if (argc < 1) {
            log_puts("xattr-set: file path must be provided");
        } else {
            log_puts("xattr-set: too many arguments");
        }
        return -1;
    }
    char const *path = *(++argv);

    int fd = bookmarkfs_xattr_open(path);
    if (fd < 0) {
        return -1;
    }
    int status = bookmarkfs_xattr_list(fd, xattr_list_cb, NULL);
    close(fd);
    return status;
}

static int
subcmd_xattr_set (
    int   argc,
    char *argv[]
) {
    char *val = NULL;

    OPT_START(argc, argv, "v:")
    OPT_OPT('v') {
        val = optarg;
        break;
    }
    OPT_NOVAL
    OPT_END

    if (argc != 2) {
        if (argc < 2) {
            log_puts("xattr-set: xattr name and file path must be specified");
        } else {
            log_puts("xattr-set: too many arguments");
        }
        return -1;
    }
    char const *name = argv[0];
    char const *path = argv[1];

    int fd = bookmarkfs_xattr_open(path);
    if (fd < 0) {
        return -1;
    }
    int status = -1;

    size_t  val_size = 0;
    char   *buf      = val;
    if (buf == NULL) {
        size_t buf_size = 4096;
        buf = xmalloc(buf_size);
        do {
            if (val_size == buf_size) {
                buf_size += buf_size >> 1;
                buf = xrealloc(buf, buf_size);
            }
            val_size += fread(buf + val_size, 1, buf_size - val_size, stdin);
            if (ferror(stdin)) {
                log_printf("fread(): %s", strerror(errno));
                goto end;
            }
        } while (!feof(stdin));
    } else {
        val_size = strlen(buf);
    }
    status = bookmarkfs_xattr_set(fd, name, buf, val_size);

  end:
    if (val == NULL) {
        free(buf);
    }
    close(fd);
    return status;
}

static int
xattr_get_cb (
    void   *user_data,
    void   *buf,
    size_t  buf_len
) {
    struct xattr_get_ctx const *ctx = user_data;

    uint32_t flags = ctx->flags;
    if (!(flags & BMCTL_XATTR_GET_QUIET)) {
        if (0 > printf("%s%c", ctx->prefix, ctx->sep)) {
            log_printf("printf(): %s", strerror(errno));
            return -1;
        }
    }
    if (!(flags & BMCTL_XATTR_GET_BINARY)) {
        for (unsigned char *s = buf, *end = s; s < end; ++s) {
            if (iscntrl(*s)) {
                *s = '?';
            }
        }
    }
    if (buf_len != fwrite(buf, 1, buf_len, stdout)) {
        log_printf("fwrite(): %s", strerror(errno));
        return -1;
    }
    if (!(flags & BMCTL_XATTR_GET_NOEOL)) {
        if (EOF == fputc(ctx->eol, stdout)) {
            log_printf("fputc(): %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int
xattr_get_one (
    char const           *path,
    char                 *names[],
    int                   names_cnt,
    struct xattr_get_ctx *ctx
) {
    int fd = bookmarkfs_xattr_open(path);
    if (fd < 0) {
        return -1;
    }

    int status;
    for (int i = 0; i < names_cnt; ++i) {
        char const *name = names[i];

        ctx->prefix = (ctx->flags & BMCTL_XATTR_GET_MULTI) ? name : path;
        status = bookmarkfs_xattr_get(fd, name, xattr_get_cb, ctx);
        if (status < 0) {
            goto end;
        }
    }

  end:
    close(fd);
    return status;
}

static int
xattr_list_cb (
    void   *UNUSED_VAR(user_data),
    void   *buf,
    size_t  buf_len
) {
    if (0 > printf("%.*s\n", (int)buf_len, (char *)buf)) {
        log_printf("printf(): %s", strerror(errno));
        return -1;
    }
    return 0;
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
