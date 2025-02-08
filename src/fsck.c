/**
 * bookmarkfs/src/fsck.c
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
#include <string.h>

#include <unistd.h>

#ifdef BOOKMARKFS_INTERACTIVE_FSCK
#  include <readline/history.h>
#  include <readline/readline.h>
#endif

#include "frontend_util.h"
#include "fsck_handler.h"
#include "fsck_ops.h"
#include "lib.h"
#include "xstd.h"

struct fsck_ctx {
    struct bookmarkfs_fsck_ops     const *ops;
    struct bookmarkfs_fsck_handler const *handler;

    void *ops_ctx;
    void *handler_ctx;

    void *backend_handle;
    void *handler_handle;
};

struct fsck_info {
    struct bookmarkfs_conf_opt *backend_opts;
    struct bookmarkfs_conf_opt *handler_opts;

    char const *backend_name;
    char const *handler_name;
    char       *path;
    char const *rl_app_name;

    struct {
        unsigned no_fsck       : 1;
        unsigned no_landlock   : 1;
        unsigned no_sandbox    : 1;
        unsigned print_help    : 1;
        unsigned print_version : 1;
        unsigned interactive   : 1;
        unsigned readonly      : 1;
        unsigned recursive     : 1;
        unsigned type          : BOOKMARKFS_BOOKMARK_TYPE_BITS;
    } flags;
};

// Forward declaration start
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
static int  init_readline (struct fsck_info const *);
#endif

static void destroy_ctx   (struct fsck_ctx const *);
static int  do_fsck       (struct fsck_ctx const *);
static int  enter_sandbox (struct fsck_ctx const *, struct fsck_info const *);
static int  init_all      (struct fsck_ctx *, int, char *[]);
static int  init_backend  (struct fsck_ctx *, struct fsck_info const *);
static int  init_handler  (struct fsck_ctx *, struct fsck_info const *);
static int  parse_opts    (struct fsck_info *, int, char *[]);
// Forward declaration end

extern struct bookmarkfs_fsck_handler const fsck_handler_simple;

#ifdef BOOKMARKFS_INTERACTIVE_FSCK

static int
init_readline (
    struct fsck_info const *info
) {
    if (!info->flags.interactive) {
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        log_puts("standard input and/or output is not a terminal");
        return -1;
    }

    rl_readline_name = info->rl_app_name;
    rl_inhibit_completion = 1;

    rl_initialize();
    using_history();
    return 0;
}

#endif  /* defined(BOOKMARKFS_INTERACTIVE_FSCK) */

static void
destroy_ctx (
    struct fsck_ctx const *ctx
) {
    if (ctx->ops != NULL) {
        ctx->ops->destroy(ctx->ops_ctx);
    }
    if (ctx->handler != NULL) {
        ctx->handler->destroy(ctx->handler_ctx);
    }
    bookmarkfs_unload(ctx->backend_handle);
    bookmarkfs_unload(ctx->handler_handle);
}

static int
do_fsck (
    struct fsck_ctx const *ctx
) {
    if (ctx->ops == NULL) {
        return 0;
    }

    while (1) {
        union bookmarkfs_fsck_handler_data data;
        int result = ctx->ops->next(ctx->ops_ctx, &data.entry);
        if (result < 0) {
            return -1;
        }
        if (result == BOOKMARKFS_FSCK_RESULT_END) {
            return 0;
        }

      run_handler:
        result = ctx->handler->run(ctx->handler_ctx, result, &data);
        if (result < 0) {
            return -1;
        }
        if (result == BOOKMARKFS_FSCK_NEXT) {
            continue;
        }
        if (result == BOOKMARKFS_FSCK_STOP) {
            return 0;
        }
        if (result == BOOKMARKFS_FSCK_APPLY) {
            result = ctx->ops->apply(ctx->ops_ctx, &data.entry);
            if (result < 0) {
                return -1;
            }
            if (result == BOOKMARKFS_FSCK_RESULT_END) {
                result = -1;
            }
            goto run_handler;
        }
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
        if (result == BOOKMARKFS_FSCK_USER_INPUT) {
            putchar('\n');
            data.str = readline(data.str);
            if (data.str == NULL) {
                return 0;
            }
            if (rl_end > 0) {
                add_history(data.str);
            }
            result = -1;
            goto run_handler;
        }
#endif
        result = ctx->ops->control(ctx->ops_ctx, result);
        if (result < 0) {
            return -1;
        }
        result = -1;
        goto run_handler;
    }
}

static int
enter_sandbox (
    struct fsck_ctx  const *ctx,
    struct fsck_info const *info
) {
    if (info->flags.no_sandbox) {
        return 0;
    }
    if (0 != ctx->ops->sandbox(ctx->ops_ctx)) {
        return -1;
    }
    debug_puts("sandbox entered");
    return 0;
}

static int
init_all (
    struct fsck_ctx *ctx,
    int              argc,
    char            *argv[]
) {
    struct fsck_info info = {
        .rl_app_name = "fsck.bookmarkfs",
        .flags = {
            .readonly = 1,
            .type     = BOOKMARKFS_BOOKMARK_TYPE_BOOKMARK,
        },
    };

    int status = -1;
    if (0 != parse_opts(&info, argc, argv)) {
        goto end;
    }
    if (0 != init_handler(ctx, &info)) {
        if (info.flags.no_fsck) {
            status = 0;
        }
        goto end;
    }
    if (0 != init_backend(ctx, &info)) {
        if (info.flags.no_fsck) {
            status = 0;
        }
        goto end;
    }
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
    if (0 != init_readline(&info)) {
        goto end;
    }
#endif
    if (0 != enter_sandbox(ctx, &info)) {
        goto end;
    }
    status = 0;

  end:
    bookmarkfs_opts_free(info.backend_opts);
    bookmarkfs_opts_free(info.handler_opts);
    return status;
}

static int
init_backend (
    struct fsck_ctx        *ctx,
    struct fsck_info const *info
) {
    struct bookmarkfs_backend const *backend = NULL;
    struct bookmarkfs_fsck_ops const *ops = &fsck_online_ops;
    if (info->backend_name != NULL) {
        backend = bookmarkfs_load_backend(info->backend_name,
                &ctx->backend_handle);
        if (backend == NULL || ctx->backend_handle == NULL) {
            return -1;
        }
        ops = &fsck_offline_ops;
    }

    uint32_t backend_flags = BOOKMARKFS_FRONTEND_FSCK;
    if (info->flags.no_fsck) {
        if (info->flags.print_help) {
            backend_flags |= BOOKMARKFS_BACKEND_INFO_HELP;
        } else {
            backend_flags |= BOOKMARKFS_BACKEND_INFO_VERSION;
        }
        ops->info(backend, backend_flags);
        return -1;
    }

    if (backend != NULL && backend->backend_init != NULL) {
        backend_flags |= BOOKMARKFS_BACKEND_LIB_READY;
        if (0 != backend->backend_init(backend_flags)) {
            return -1;
        }
    }

    uint32_t flags = info->flags.type << BOOKMARKFS_BOOKMARK_TYPE_SHIFT;
    if (info->flags.no_sandbox) {
        flags |= BOOKMARKFS_BACKEND_NO_SANDBOX;
    }
    if (info->flags.no_landlock) {
        flags |= BOOKMARKFS_BACKEND_NO_LANDLOCK;
    }
    if (info->flags.readonly) {
        flags |= BOOKMARKFS_BACKEND_READONLY;
    }
    if (info->flags.recursive) {
        flags |= BOOKMARKFS_FSCK_OP_RECURSIVE;
    }
    if (0 != ops->create(backend, info->path, info->backend_opts, flags,
                &ctx->ops_ctx)
    ) {
        return -1;
    }
    ctx->ops = ops;
    return 0;
}

static int
init_handler (
    struct fsck_ctx        *ctx,
    struct fsck_info const *info
) {
    struct bookmarkfs_fsck_handler const *handler = &fsck_handler_simple;
    if (info->handler_name != NULL) {
        handler = bookmarkfs_load_fsck_handler(info->handler_name,
                &ctx->handler_handle);
        if (handler == NULL || ctx->handler_handle == NULL) {
            return -1;
        }

    }
    if (info->flags.no_fsck) {
        if (info->handler_name == NULL) {
            return 0;
        }
        uint32_t flags = BOOKMARKFS_FSCK_HANDLER_INFO_HELP;
        if (info->flags.print_version) {
            flags = BOOKMARKFS_FSCK_HANDLER_INFO_VERSION;
        }
        handler->info(flags);
        return -1;
    }

    if (0 != bookmarkfs_lib_init()) {
        return -1;
    }
    uint32_t flags = 0;
    if (info->flags.interactive) {
        flags |= BOOKMARKFS_FSCK_HANDLER_INTERACTIVE;
    }
    if (info->flags.readonly) {
        flags |= BOOKMARKFS_FSCK_HANDLER_READONLY;
    }
    if (0 != handler->create(info->handler_opts, flags, &ctx->handler_ctx)) {
        return -1;
    }
    ctx->handler = handler;
    return 0;
}

static int
parse_opts (
    struct fsck_info *info,
    int               argc,
    char             *argv[]
) {
    enum {
        BOOKMARKFS_OPT_BACKEND,
        BOOKMARKFS_OPT_HANDLER,
        BOOKMARKFS_OPT_NO_LANDLOCK,
        BOOKMARKFS_OPT_NO_SANDBOX,
        BOOKMARKFS_OPT_REPAIR,
        BOOKMARKFS_OPT_RL_APP,
        BOOKMARKFS_OPT_TYPE,

        BOOKMARKFS_OPT_END_,
    };

#define BOOKMARKFS_OPT(name, token)  [BOOKMARKFS_OPT_##name] = (token)
    char const *const opts[] = {
        BOOKMARKFS_OPT(BACKEND,     "backend"),
        BOOKMARKFS_OPT(HANDLER,     "handler"),
        BOOKMARKFS_OPT(NO_LANDLOCK, "no_landlock"),
        BOOKMARKFS_OPT(NO_SANDBOX,  "no_sandbox"),
        BOOKMARKFS_OPT(REPAIR,      "repair"),
        BOOKMARKFS_OPT(RL_APP,      "rl_app"),
        BOOKMARKFS_OPT(TYPE,        "type"),

        BOOKMARKFS_OPT(END_, NULL),
    };

    getopt_foreach(argc, argv, ":o:iRhV") {
      case 'o':
        SUBOPT_START(opts)
        SUBOPT_OPT(BOOKMARKFS_OPT_BACKEND) SUBOPT_HAS_VAL {
            char const *name = SUBOPT_VAL;
            if (name[0] == '\0') {
                name = NULL;
            }
            info->backend_name = name;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_HANDLER) SUBOPT_HAS_VAL {
            char const *name = SUBOPT_VAL;
            if (name[0] == '\0') {
                name = NULL;
            }
            info->handler_name = name;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_NO_LANDLOCK) SUBOPT_NO_VAL {
            info->flags.no_landlock = 1;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_NO_SANDBOX) SUBOPT_NO_VAL {
            info->flags.no_sandbox = 1;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_RL_APP) {
            info->rl_app_name = SUBOPT_VAL;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_REPAIR) SUBOPT_NO_VAL {
            info->flags.readonly = 0;
        }
        SUBOPT_OPT(BOOKMARKFS_OPT_TYPE) SUBOPT_HAS_VAL {
            if (0 == strcmp("tag", SUBOPT_VAL)) {
                info->flags.type = BOOKMARKFS_BOOKMARK_TYPE_TAG;
            } else if (0 == strcmp("keyword", SUBOPT_VAL)) {
                info->flags.type = BOOKMARKFS_BOOKMARK_TYPE_KEYWORD;
            } else if (0 == strcmp("bookmark", SUBOPT_VAL)) {
                info->flags.type = BOOKMARKFS_BOOKMARK_TYPE_BOOKMARK;
            } else {
                return SUBOPT_ERR_BAD_KEY();
            }
        }
        SUBOPT_OPT_FALLBACK() {
            char *opt = SUBOPT_STR;
            switch (*(opt++)) {
              case '@':
                bookmarkfs_opts_add(&info->backend_opts, opt);
                break;

              case '%':
                bookmarkfs_opts_add(&info->handler_opts, opt);
                break;

              default:
                return SUBOPT_ERR_BAD_KEY();
            }
        }
        SUBOPT_END

      case 'i':
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
        info->flags.interactive = 1;
        break;
#else
        log_puts("bad option '-i': "
                "interactive fsck is not enabled on this build");
        return -1;
#endif

      case 'R':
        info->flags.recursive = 1;
        break;

      case 'h':
        info->flags.print_help = 1;
        info->flags.no_fsck    = 1;
        return 0;

      case 'V':
        info->flags.print_version = 1;
        info->flags.no_fsck       = 1;
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

    argc -= optind;
    if (argc != 1) {
        if (argc < 1) {
            log_puts("pathname must be specified");
        } else {
            log_puts("too many arguments");
        }
        return -1;
    }

    argv += optind;
    info->path = argv[0];
    return 0;
}

int
main (
    int   argc,
    char *argv[]
) {
    int status = EXIT_FAILURE;

    struct fsck_ctx ctx = { 0 };
    if (0 != init_all(&ctx, argc, argv)) {
        goto end;
    }
    if (0 != do_fsck(&ctx)) {
        goto end;
    }
    status = EXIT_SUCCESS;

  end:
    destroy_ctx(&ctx);
    return status;
}
