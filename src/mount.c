/**
 * bookmarkfs/src/mount.c
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

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "frontend_util.h"
#include "fs_ops.h"
#include "lib.h"
#include "macros.h"
#include "prng.h"
#include "version.h"
#include "xstd.h"

// The upper and default values are chosen according to Chromium's
// `url::kMaxURLChars` and `content::kMaxURLDisplayChars` constants.
//
// See Chromium source code:
// - /url/url_constants.h
// - /content/public/common/content_constants.cc
#define FILE_MAX_LOWER    1024
#define FILE_MAX_UPPER    ( 2 * 1024 * 1024 )
#define FILE_MAX_DEFAULT  ( 32 * 1024 )

struct mount_ctx {
    struct bookmarkfs_backend const *backend_impl;
    void                            *backend_ctx;
    void                            *backend_handle;
    struct fuse_session             *session;
};

struct mount_info {
    struct bookmarkfs_backend_conf backend_conf;

    char const *backend_name;
    char const *fs_name;
    char const *mount_target;

    struct fs_flags  fs_flags;
    size_t           file_max;
    struct fuse_args args;

    struct {
        unsigned is_foreground : 1;
        unsigned no_mount      : 1;
        unsigned no_sandbox    : 1;
        unsigned print_help    : 1;
        unsigned print_version : 1;
    } flags;
};

// Forward declaration start
static void destroy_ctx   (struct mount_ctx const *);
static int  enter_sandbox (struct mount_ctx const *,
                           struct mount_info const *);
static int  init_all      (struct mount_ctx *, int, char *[]);
static int  init_backend  (struct mount_ctx *, struct mount_info *);
static int  init_fuse     (struct mount_ctx *, struct mount_info *);
static int  parse_opts    (struct mount_info *, int, char *[]);
static void print_help    (void);
static void print_version (void);
static int  run_fuse      (struct fuse_session *);
// Forward declaration end

static void
destroy_ctx (
    struct mount_ctx const *ctx
) {
    if (ctx->session != NULL) {
        fuse_remove_signal_handlers(ctx->session);
        fuse_session_unmount(ctx->session);
        fuse_session_destroy(ctx->session);
    }
    if (ctx->backend_impl != NULL) {
        ctx->backend_impl->backend_free(ctx->backend_ctx);
    }
    bookmarkfs_unload(ctx->backend_handle);
}

static int
enter_sandbox (
    struct mount_ctx  const *ctx,
    struct mount_info const *info
) {
    if (info->flags.no_sandbox) {
        return 0;
    }

    struct bookmarkfs_backend_create_resp resp = {
        .bookmarks_root_id = UINT64_MAX,
        .tags_root_id      = UINT64_MAX,
    };
    if (0 != ctx->backend_impl->backend_sandbox(ctx->backend_ctx, &resp)) {
        return -1;
    }
    debug_puts("sandbox entered");

    fs_init_metadata(resp.bookmarks_root_id, resp.tags_root_id, NULL);
    return 0;
}

static int
init_all (
    struct mount_ctx *ctx,
    int               argc,
    char             *argv[]
) {
    struct mount_info info = {
        .backend_conf = {
            .version = BOOKMARKFS_VERNUM,
            .flags   = BOOKMARKFS_BACKEND_READONLY,
        },
        .fs_flags = {
            .accmode  = 0700,
            .readonly = 1,
        },
        .file_max = FILE_MAX_DEFAULT,
    };

    int status = -1;
    if (0 != parse_opts(&info, argc, argv)) {
        goto end;
    }
    if (0 != init_backend(ctx, &info)) {
        if (info.flags.no_mount) {
            status = 0;
        }
        goto end;
    }
    if (0 != init_fuse(ctx, &info)) {
        goto end;
    }
    if (0 != enter_sandbox(ctx, &info)) {
        goto end;
    }
    status = 0;

  end:
    bookmarkfs_opts_free(info.backend_conf.opts);
    fuse_opt_free_args(&info.args);
    return status;
}

static int
init_backend (
    struct mount_ctx  *ctx,
    struct mount_info *info
) {
    if (info->backend_name == NULL) {
        if (info->flags.print_help) {
            print_help();
        } else {
            print_version();
        }
        return -1;
    }

    struct bookmarkfs_backend const *impl
        = bookmarkfs_load_backend(info->backend_name, &ctx->backend_handle);
    if (impl == NULL || ctx->backend_handle == NULL) {
        return -1;
    }

    uint32_t flags = BOOKMARKFS_FRONTEND_MOUNT;
    if (info->flags.no_mount) {
        if (impl->backend_info != NULL) {
            if (info->flags.print_help) {
                flags |= BOOKMARKFS_BACKEND_INFO_HELP;
            } else {
                flags |= BOOKMARKFS_BACKEND_INFO_VERSION;
            }
            impl->backend_info(flags);
        }
        return -1;
    }

    if (0 != bookmarkfs_lib_init()) {
        return -1;
    }
    if (impl->backend_init != NULL) {
        flags |= BOOKMARKFS_BACKEND_LIB_READY;
        if (0 != impl->backend_init(flags)) {
            return -1;
        }
    }

    struct bookmarkfs_backend_create_resp resp = {
        .bookmarks_root_id = UINT64_MAX,
        .tags_root_id      = UINT64_MAX,
        .bookmark_attrs    = "",
    };
    if (0 != impl->backend_create(&info->backend_conf, &resp)) {
        debug_puts("backend_create() failed");
        return -1;
    }
    ctx->backend_impl        = impl;
    ctx->backend_ctx         = resp.backend_ctx;
    info->backend_name       = resp.name;
    info->fs_flags.exclusive = !!(resp.flags & BOOKMARKFS_BACKEND_EXCLUSIVE);
    info->fs_flags.has_keyword
        = !!(resp.flags & BOOKMARKFS_BACKEND_HAS_KEYWORD);

    fs_init_backend(impl, resp.backend_ctx);
    fs_init_metadata(resp.bookmarks_root_id, resp.tags_root_id,
            resp.bookmark_attrs);
    fs_init_opts(info->fs_flags, info->file_max);
    return 0;
}

static int
init_fuse (
    struct mount_ctx  *ctx,
    struct mount_info *info
) {
    struct fuse_args *args = &info->args;

    char const *fsname = info->fs_name;
    if (fsname == NULL) {
        fsname = info->backend_name;
    }
    char const *rw = info->fs_flags.readonly ? "ro": "rw";

    char *buf;
    xasprintf(&buf, "-odefault_permissions,fsname=%s,%s", fsname, rw);
    xassert(0 == fuse_opt_add_arg(args, buf));
    free(buf);

#define BOOKMARKFS_OP(name)  .name = fs_op_##name
    static struct fuse_lowlevel_ops const ops = {
        BOOKMARKFS_OP(init),
        BOOKMARKFS_OP(destroy),
        BOOKMARKFS_OP(lookup),

        BOOKMARKFS_OP(getattr),
        BOOKMARKFS_OP(setattr),
        BOOKMARKFS_OP(rename),
        BOOKMARKFS_OP(ioctl),

        BOOKMARKFS_OP(setxattr),
        BOOKMARKFS_OP(getxattr),
        BOOKMARKFS_OP(listxattr),
        BOOKMARKFS_OP(removexattr),

        BOOKMARKFS_OP(create),
        BOOKMARKFS_OP(open),
        BOOKMARKFS_OP(read),
        BOOKMARKFS_OP(write),
        BOOKMARKFS_OP(fsync),
        BOOKMARKFS_OP(release),
        BOOKMARKFS_OP(link),
        BOOKMARKFS_OP(unlink),

        BOOKMARKFS_OP(rmdir),
        BOOKMARKFS_OP(mkdir),
        BOOKMARKFS_OP(opendir),
        BOOKMARKFS_OP(readdir),
        BOOKMARKFS_OP(readdirplus),
        BOOKMARKFS_OP(fsyncdir),
        BOOKMARKFS_OP(releasedir),
    };
    struct fuse_session *session
        = fuse_session_new(args, &ops, sizeof(ops), NULL);
    if (session == NULL) {
        log_puts("fuse_session_new() failed");
        return -1;
    }

    if (0 != fuse_set_signal_handlers(session)) {
        log_puts("fuse_set_signal_handlers() failed");
        goto destroy;
    }
    if (0 != fuse_session_mount(session, info->mount_target)) {
        log_puts("fuse_session_mount() failed");
        goto destroy;
    }
    if (0 != fuse_daemonize(info->flags.is_foreground)) {
        log_puts("fuse_daemonize() failed");
        goto unmount;
    }

    ctx->session = session;
    fs_init_fuse(session);
    return 0;

  unmount:
    fuse_remove_signal_handlers(session);
    fuse_session_unmount(session);

  destroy:
    fuse_session_destroy(session);

    return -1;
}

static int
parse_opts (
    struct mount_info *info,
    int                argc,
    char              *argv[]
) {
    enum {
        // bookmarkfs options
        BOOKMARKFS_OPT_ACCMODE,
        BOOKMARKFS_OPT_BACKEND,
        BOOKMARKFS_OPT_CTIME,
        BOOKMARKFS_OPT_EOL,
        BOOKMARKFS_OPT_FILE_MAX,
        BOOKMARKFS_OPT_NO_LANDLOCK,
        BOOKMARKFS_OPT_NO_SANDBOX,

        // kernel options
        BOOKMARKFS_OPT_FSNAME,
        BOOKMARKFS_OPT_RO,
        BOOKMARKFS_OPT_RW,

        // ignored options
        BOOKMARKFS_OPT_ATIME,
        BOOKMARKFS_OPT_BLKDEV,
        BOOKMARKFS_OPT_BLKSIZE,
        BOOKMARKFS_OPT_DEV,
        BOOKMARKFS_OPT_DIRATIME,
        BOOKMARKFS_OPT_EXEC,
        BOOKMARKFS_OPT_FD,
        BOOKMARKFS_OPT_RELATIME,
        BOOKMARKFS_OPT_STRICTATIME,
        BOOKMARKFS_OPT_SUBTYPE,
        BOOKMARKFS_OPT_SUID,
        BOOKMARKFS_OPT_NOATIME,
        BOOKMARKFS_OPT_NODEV,
        BOOKMARKFS_OPT_NODIRATIME,
        BOOKMARKFS_OPT_NOEXEC,
        BOOKMARKFS_OPT_NORELATIME,
        BOOKMARKFS_OPT_NOSTRICTATIME,
        BOOKMARKFS_OPT_NOSUID,

        BOOKMARKFS_OPT_END_,
    };

#define BOOKMARKFS_OPT(name, token)  [BOOKMARKFS_OPT_##name] = (token)
    char const *const opts[] = {
        BOOKMARKFS_OPT(ACCMODE,     "accmode"),
        BOOKMARKFS_OPT(BACKEND,     "backend"),
        BOOKMARKFS_OPT(CTIME,       "ctime"),
        BOOKMARKFS_OPT(EOL,         "eol"),
        BOOKMARKFS_OPT(FILE_MAX,    "file_max"),
        BOOKMARKFS_OPT(NO_LANDLOCK, "no_landlock"),
        BOOKMARKFS_OPT(NO_SANDBOX,  "no_sandbox"),

        BOOKMARKFS_OPT(FSNAME, "fsname"),
        BOOKMARKFS_OPT(RO,     "ro"),
        BOOKMARKFS_OPT(RW,     "rw"),

        BOOKMARKFS_OPT(ATIME,         "atime"),
        BOOKMARKFS_OPT(BLKDEV,        "blkdev"),
        BOOKMARKFS_OPT(BLKSIZE,       "blksize"),
        BOOKMARKFS_OPT(DEV,           "dev"),
        BOOKMARKFS_OPT(DIRATIME,      "diratime"),
        BOOKMARKFS_OPT(EXEC,          "exec"),
        BOOKMARKFS_OPT(FD,            "fd"),
        BOOKMARKFS_OPT(RELATIME,      "relatime"),
        BOOKMARKFS_OPT(STRICTATIME,   "strictatime"),
        BOOKMARKFS_OPT(SUBTYPE,       "subtype"),
        BOOKMARKFS_OPT(SUID,          "suid"),
        BOOKMARKFS_OPT(NOATIME,       "noatime"),
        BOOKMARKFS_OPT(NODEV,         "nodev"),
        BOOKMARKFS_OPT(NODIRATIME,    "nodiratime"),
        BOOKMARKFS_OPT(NOEXEC,        "noexec"),
        BOOKMARKFS_OPT(NORELATIME,    "norelatime"),
        BOOKMARKFS_OPT(NOSTRICTATIME, "nostrictatime"),
        BOOKMARKFS_OPT(NOSUID,        "nosuid"),

        BOOKMARKFS_OPT(END_, NULL),
    };

    char const *fargs_common = "-onoatime,noexec,nosuid,"
#ifdef __linux__
            "nodev,"
#endif
            "subtype=bookmarkfs";
    struct fuse_args *fargs = &info->args;
    xassert(0 == fuse_opt_add_arg(fargs, ""));
    xassert(0 == fuse_opt_add_arg(fargs, fargs_common));

#define SUBOPT_PARSE_NUM(min, max, result)        \
    do {                                          \
        long num_ = strtol(SUBOPT_VAL, NULL, 0);  \
        if (num_ < (min) || num_ > (max)) {       \
            return SUBOPT_ERR_BAD_VAL();          \
        }                                         \
        (result) = num_;                          \
    } while (0)

    getopt_foreach(argc, argv, ":o:FhV") {
      case 'o':
        SUBOPT_START(opts)
        SUBOPT_OPT(BOOKMARKFS_OPT_ACCMODE) SUBOPT_HAS_VAL {
            SUBOPT_PARSE_NUM(0, 0777, info->fs_flags.accmode);
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_BACKEND) SUBOPT_HAS_VAL {
            char const *name = SUBOPT_VAL;
            if (name[0] == '\0') {
                log_puts("backend name must not be empty");
                return -1;
            }
            info->backend_name = name;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_CTIME) SUBOPT_NO_VAL {
            info->fs_flags.ctime = 1;
            info->backend_conf.flags |= BOOKMARKFS_BACKEND_CTIME;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_EOL) SUBOPT_NO_VAL {
            info->fs_flags.eol = 1;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_FILE_MAX) SUBOPT_HAS_VAL {
            SUBOPT_PARSE_NUM(FILE_MAX_LOWER, FILE_MAX_UPPER, info->file_max);
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_FSNAME) SUBOPT_HAS_VAL {
            info->fs_name = SUBOPT_VAL;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_NO_LANDLOCK) SUBOPT_NO_VAL {
            info->backend_conf.flags |= BOOKMARKFS_BACKEND_NO_LANDLOCK;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_NO_SANDBOX) SUBOPT_NO_VAL {
            info->flags.no_sandbox = 1;
            info->backend_conf.flags |= BOOKMARKFS_BACKEND_NO_SANDBOX;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_RO) SUBOPT_NO_VAL {
            info->fs_flags.readonly = 1;
            info->backend_conf.flags |= BOOKMARKFS_BACKEND_READONLY;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_RW) SUBOPT_NO_VAL {
            info->fs_flags.readonly = 0;
            info->backend_conf.flags &= ~BOOKMARKFS_BACKEND_READONLY;
        }
        SUBOPT_OPT_FALLBACK() {
            char *opt = SUBOPT_STR;
            if (opt[0] == '@') {
                bookmarkfs_opts_add(&info->backend_conf.opts, opt + 1);
            } else {
                xassert(0 == fuse_opt_add_opt(&fargs->argv[1], opt));
            }
        }
        SUBOPT_OPT_DEFAULT() {
            debug_printf("option '-o %s' ignored", SUBOPT_STR);
        }
        SUBOPT_END

      case 'F':
        info->flags.is_foreground = 1;
        break;

      case 'h':
        info->flags.print_help = 1;
        info->flags.no_mount   = 1;
        return 0;

      case 'V':
        info->flags.print_version = 1;
        info->flags.no_mount      = 1;
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

    if (info->backend_name == NULL) {
        log_puts("backend not specified");
        return -1;
    }

    argc -= optind;
    if (argc != 2) {
        if (argc < 2) {
            log_puts("mount source and target must be specified");
        } else {
            log_puts("too many arguments");
        }
        return -1;
    }

    argv += optind;
    info->backend_conf.store_path = argv[0];
    info->mount_target            = argv[1];
    return 0;
}

static void
print_help (void)
{
    puts("Usage: mount.bookmarkfs [options] <src> <target>\n"
            "\n"
            "Common options:\n"
            "  -o backend=<name>      Backend used by the filesystem\n"
            "  -o @<key>[=<value>]    Backend-specific option\n"
            "  -o accmode=<mode>      File access mode\n"
            "  -o ctime               Maintain file change time\n"
            "  -o eol                 Add a newline to the end of file\n"
            "  -o file_max=<bytes>    Max file size limit\n"
            "\n"
            "Other options:\n"
            "  -o no_sandbox     Disable sandbox\n"
#ifdef __linux__
            "  -o no_landlock    Disable Landlock features for sandbox\n"
#endif
            "\n"
            "  -F    Run in foreground, do not daemonize\n"
            "  -h    Print help message and exit\n"
            "  -V    Print version information and exit\n"
            "\n"
            "See the mount.bookmarkfs(1) manpage for more information,\n"
            "or run 'info bookmarkfs' for the full user manual.\n"
            "\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.");
}

static void
print_version (void)
{
    printf("mount.bookmarkfs (BookmarkFS) %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
    puts(BOOKMARKFS_FEATURE_STRING(DEBUG, "debug"));

    bookmarkfs_print_lib_version("\n");
}

static int
run_fuse (
    struct fuse_session *session
) {
    if (session == NULL) {
        return 0;
    }
    int status = fuse_session_loop(session);
    if (status != 0) {
        char const *desc;
        if (status < 0) {
            desc = xstrerror(-status);
        } else {
            // MT-unsafe
            desc = strsignal(status);
        }
        log_printf("fuse_session_loop(): %s", desc);
    }
    return status;
}

int
main (
    int   argc,
    char *argv[]
) {
    int status = EXIT_FAILURE;

    struct mount_ctx ctx = { 0 };
    if (0 != init_all(&ctx, argc, argv)) {
        goto end;
    }
    if (0 != run_fuse(ctx.session)) {
        goto end;
    }
    status = EXIT_SUCCESS;

  end:
    destroy_ctx(&ctx);
    return status;
}
