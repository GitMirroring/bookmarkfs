/**
 * bookmarkfs/src/mkfs.c
 *
 * Chromium backend for BookmarkFS.
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

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

#include "frontend_util.h"
#include "macros.h"
#include "version.h"
#include "xstd.h"

struct mkfs_ctx {
    struct bookmarkfs_backend_conf backend_conf;

    bookmarkfs_backend_mkfs_func *mkfs_func;
    void                         *backend_handle;
    char const                   *backend_name;

    struct {
        unsigned no_mkfs       : 1;
        unsigned print_help    : 1;
        unsigned print_version : 1;
    } flags;
};

// Forward declaration start
static int  parse_opts    (struct mkfs_ctx *, int, char *[]);
static int  init_backend  (struct mkfs_ctx *);
static void print_help    (void);
static void print_version (void);
// Forward declaration end

static int
parse_opts (
    struct mkfs_ctx *ctx,
    int              argc,
    char            *argv[]
) {
    enum {
        BOOKMARKFS_OPT_BACKEND,
        BOOKMARKFS_OPT_FORCE,

        BOOKMARKFS_OPT_END_,
    };

#define BOOKMARKFS_OPT(name, token)  [BOOKMARKFS_OPT_##name] = (token)
    char const *const opts[] = {
        BOOKMARKFS_OPT(BACKEND, "backend"),
        BOOKMARKFS_OPT(FORCE,   "force"),

        BOOKMARKFS_OPT(END_, NULL),
    };

    getopt_foreach(argc, argv, ":o:hV") {
      case 'o':
        SUBOPT_START(opts)
        SUBOPT_OPT(BOOKMARKFS_OPT_BACKEND) SUBOPT_HAS_VAL {
            char const *name = SUBOPT_VAL;
            if (name[0] == '\0') {
                log_puts("backend name must not be empty");
                return -1;
            }
            ctx->backend_name = name;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_FORCE) SUBOPT_NO_VAL {
            ctx->backend_conf.flags |= BOOKMARKFS_BACKEND_MKFS_FORCE;
        }
        SUBOPT_OPT_FALLBACK() {
            char *opt = SUBOPT_STR;
            if (opt[0] == '@') {
                bookmarkfs_opts_add(&ctx->backend_conf.opts, opt + 1);
            } else {
                return SUBOPT_ERR_BAD_KEY();
            }
        }
        SUBOPT_END

      case 'h':
        ctx->flags.no_mkfs    = 1;
        ctx->flags.print_help = 1;
        return 0;

      case 'V':
        ctx->flags.no_mkfs       = 1;
        ctx->flags.print_version = 1;
        return 0;

      case ':':
        log_printf("no value provided for option '-%c'", optopt);
        return -1;

      case '?':
        log_printf("invalid option '-%c'", optopt);
        return -1;

      default:
        unreachable();
    }

    if (ctx->backend_name == NULL) {
        log_puts("backend not specified");
        return -1;
    }

    argc -= optind;
    if (argc != 1) {
        if (argc == 0) {
            log_puts("bookmark filepath must be specified");
        } else {
            log_puts("too many arguments");
        }
        return -1;
    }

    argv += optind;
    ctx->backend_conf.store_path = argv[0];
    return 0;
}

static int
init_backend (
    struct mkfs_ctx *ctx
) {
    if (ctx->backend_name == NULL) {
        if (ctx->flags.print_help) {
            print_help();
        } else {
            print_version();
        }
        return 0;
    }

    struct bookmarkfs_backend const *impl
        = bookmarkfs_load_backend(ctx->backend_name, &ctx->backend_handle);
    if (impl == NULL || ctx->backend_handle == NULL) {
        return -1;
    }

    uint32_t flags = BOOKMARKFS_FRONTEND_MKFS;
    if (ctx->flags.no_mkfs) {
        if (impl->backend_info != NULL) {
            if (ctx->flags.print_help) {
                flags |= BOOKMARKFS_BACKEND_INFO_HELP;
            } else {
                flags |= BOOKMARKFS_BACKEND_INFO_VERSION;
            }
            impl->backend_info(flags);
        }
        return 0;
    }

    if (impl->backend_init != NULL) {
        if (0 != impl->backend_init(flags)) {
            return -1;
        }
    }
    if (impl->backend_mkfs == NULL) {
        log_puts("backend does not implement mkfs");
        return -1;
    }
    ctx->mkfs_func = impl->backend_mkfs;
    return 0;
}

static void
print_help (void)
{
    puts("Usage: mkfs.bookmarkfs [options] <pathname>\n"
            "\n"
            "Common options:\n"
            "  -o backend=<name>      Backend used by the filesystem\n"
            "  -o @<key>[=<value>]    Backend-specific option\n"
            "  -o force               Overwrite existing files\n"
            "\n"
            "Other options:\n"
            "  -h    Print help message and exit\n"
            "  -V    Print version information and exit\n"
            "\n"
            "See the mkfs.bookmarkfs(1) manpage for more information,\n"
            "or run 'info bookmarkfs' for the full user manual.\n"
            "\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.");
}

static void
print_version (void)
{
    printf("mkfs.bookmarkfs (BookmarkFS) %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
    puts(BOOKMARKFS_FEATURE_STRING(DEBUG, "debug"));
}

int
main (
    int   argc,
    char *argv[]
) {
    int status = EXIT_FAILURE;

    struct mkfs_ctx ctx = {
        .backend_conf = {
            .version = BOOKMARKFS_VERNUM,
        },
    };
    if (0 != parse_opts(&ctx, argc, argv)) {
        goto end;
    }
    if (0 != init_backend(&ctx)) {
        goto end;
    }
    if (ctx.mkfs_func != NULL) {
        if (0 != ctx.mkfs_func(&ctx.backend_conf)) {
            goto end;
        }
    }
    status = EXIT_SUCCESS;

  end:
    bookmarkfs_opts_free(ctx.backend_conf.opts);
    bookmarkfs_unload(ctx.backend_handle);
    return status;
}
