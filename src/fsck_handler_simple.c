/**
 * bookmarkfs/src/fsck_handler_simple.c
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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "backend_util.h"
#include "fsck_handler.h"
#include "fsck_util.h"
#include "ioctl.h"
#include "xstd.h"

#define FSCK_HANDLER_EXPECT_INPUT  ( 1u << 24 )
#define FSCK_HANDLER_HAS_ENTRY     ( 1u << 25 )
#define FSCK_HANDLER_INHIBIT_NEXT  ( 1u << 26 )

struct handler_ctx {
    uint64_t parent_id;
    uint32_t counter;
    uint32_t flags;

    struct parsed_opts {
        char const *prompt;
        int         translit;
    } opts;

    struct bookmarkfs_fsck_data data_buf;
};

// Forward declaration start
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
static int  expect_input (struct handler_ctx *, char **);
static int  expect_next  (struct handler_ctx *, int, char **);
static int  handle_input (struct handler_ctx *,
                          union bookmarkfs_fsck_handler_data *);
static void print_usage  (void);
#endif  /* defined(BOOKMARKFS_INTERACTIVE_FSCK) */

static void fix_name_dup (struct handler_ctx *, struct bookmarkfs_fsck_data *);
static void fix_entry    (struct handler_ctx *, enum bookmarkfs_fsck_result,
                          struct bookmarkfs_fsck_data *);
static int  handle_entry (struct handler_ctx *, enum bookmarkfs_fsck_result,
                          union bookmarkfs_fsck_handler_data *);
static int  parse_opts   (struct bookmarkfs_conf_opt const *,
                          struct parsed_opts *);
static void print_entry  (struct bookmarkfs_fsck_data const *);
// Forward declaration end

#ifdef BOOKMARKFS_INTERACTIVE_FSCK

static int
expect_input (
    struct handler_ctx  *ctx,
    char               **data_ptr
) {
    *data_ptr = (char *)(ctx->opts.prompt);
    ctx->flags |= FSCK_HANDLER_EXPECT_INPUT;
    return BOOKMARKFS_FSCK_USER_INPUT;
}

static int
expect_next (
    struct handler_ctx  *ctx,
    int                  control,
    char               **data_ptr
) {
    switch ((*data_ptr)[1]) {
      case '-':
        ctx->flags |= FSCK_HANDLER_INHIBIT_NEXT;
        break;

      case '\0':
        break;

      default:
        print_usage();
        return expect_input(ctx, data_ptr);
    }
    ctx->flags &= ~(FSCK_HANDLER_HAS_ENTRY | FSCK_HANDLER_EXPECT_INPUT);
    return control;
}

static int
handle_input (
    struct handler_ctx                 *ctx,
    union bookmarkfs_fsck_handler_data *data
) {
    char *input = data->str;
    debug_assert(input != NULL);

    char *next = strtok(input, " \t");
    int control;
    switch (input[0]) {
      case 'p':
        if (!(ctx->flags & FSCK_HANDLER_HAS_ENTRY)) {
          no_entry:
            puts("no entry");
        } else {
            print_entry(&ctx->data_buf);
        }
        control = expect_input(ctx, &data->str);
        break;

      case 'e':
        if (next == NULL) {
            goto bad_cmd;
        }
        strncpy(ctx->data_buf.name, next, sizeof(ctx->data_buf.name));
        // fallthrough
      case 'a':
        if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
            log_puts("cannot apply, fsck is running in readonly mode");
            control = expect_input(ctx, &data->str);
            break;
        }
        if (!(ctx->flags & FSCK_HANDLER_HAS_ENTRY)) {
            goto no_entry;
        }
        data->entry.data = ctx->data_buf;
        control = expect_next(ctx, BOOKMARKFS_FSCK_APPLY, &data->str);
        break;

      case 'c':
        control = expect_next(ctx, BOOKMARKFS_FSCK_NEXT, &data->str);
        break;

      case 's':
        control = expect_next(ctx, BOOKMARKFS_FSCK_SKIP, &data->str);
        break;

      case 'S':
        control = expect_next(ctx, BOOKMARKFS_FSCK_SKIP_CHILDREN, &data->str);
        break;

      case 'r':
        control = expect_next(ctx, BOOKMARKFS_FSCK_REWIND, &data->str);
        break;

      case 'R':
        control = expect_next(ctx, BOOKMARKFS_FSCK_RESET, &data->str);
        break;

      case 'w':
        control = expect_next(ctx, BOOKMARKFS_FSCK_SAVE, &data->str);
        break;

      case 'q':
        control = BOOKMARKFS_FSCK_STOP;
        break;

      default:
      bad_cmd:
        print_usage();
        // fallthrough
      case '\0':
        control = expect_input(ctx, &data->str);
        break;
    }

    free(input);
    return control;
}

static void
print_usage (void)
{
    puts("Commands:\n"
            "  p                  Print current entry\n"
            "  a[-]               Apply proposed fix\n"
            "  e[-] <new_name>    Edit proposed fix and then apply\n"
            "  c                  Continue to next entry\n"
            "  s[-]               Skip current directory\n"
            "  S[-]               Skip current directory and its children\n"
            "  r[-]               Rewind current directory\n"
            "  R[-]               Rewind all\n"
            "  w[-]               Save applied changes\n"
            "  q                  Quit\n"
            "\n"
            "The '-' suffix inhibits the default behavior of continuing to\n"
            "the next entry after the command completes successfully.\n"
            "\n"
            "See the user manual for more information.");
}

#endif  /* defined(BOOKMARKFS_INTERACTIVE_FSCK) */

static void
fix_name_dup (
    struct handler_ctx          *ctx,
    struct bookmarkfs_fsck_data *data
) {
    size_t name_len = strnlen(data->name, sizeof(data->name));
    for (int nbytes; ; name_len = sizeof(data->name) - nbytes - 1) {
        nbytes = snprintf(data->name + name_len, sizeof(data->name) - name_len,
                "_%" PRIu32, ctx->counter);
        xassert(nbytes > 0);
        if (name_len + nbytes < sizeof(data->name)) {
            ++ctx->counter;
            break;
        }
    }
}

static void
fix_entry (
    struct handler_ctx          *ctx,
    enum bookmarkfs_fsck_result  why,
    struct bookmarkfs_fsck_data *data
) {
    switch (why) {
      case BOOKMARKFS_FSCK_RESULT_NAME_BADCHAR:
        if (unlikely(data->extra >= sizeof(data->name))) {
            data->extra = sizeof(data->name) - 1;
        }
        data->name[data->extra] = ctx->opts.translit;
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE:
        fix_name_dup(ctx, data);
        break;

      case BOOKMARKFS_FSCK_RESULT_NAME_BADLEN:
        if (data->extra > 0) {
            data->name[sizeof(data->name) - 1] = '\0';
            break;
        }
        // fallthrough
      case BOOKMARKFS_FSCK_RESULT_NAME_DOTDOT:
      case BOOKMARKFS_FSCK_RESULT_NAME_INVALID:
        sprintf(data->name, "fsck-%" PRIu64, data->id);
        break;

      default:
        unreachable();
    }
}

static int
handle_entry (
    struct handler_ctx                 *ctx,
    enum bookmarkfs_fsck_result         why,
    union bookmarkfs_fsck_handler_data *data
) {
    struct bookmarkfs_fsck_handler_entry *entry = &data->entry;
    if (entry->parent_id != ctx->parent_id) {
        ctx->parent_id = entry->parent_id;
        ctx->counter = 0;
    }

    struct bookmarkfs_fsck_data *entry_data = &entry->data;
    if (0 != explain_fsck_result(why, entry_data)) {
        return -1;
    }

    int control = BOOKMARKFS_FSCK_NEXT;
    if (!(ctx->flags & BOOKMARKFS_BACKEND_READONLY)) {
        if (why != BOOKMARKFS_FSCK_RESULT_END) {
            fix_entry(ctx, why, entry_data);
            control = BOOKMARKFS_FSCK_APPLY;
        }
    }
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
    if (ctx->flags & BOOKMARKFS_FSCK_HANDLER_INTERACTIVE) {
        ctx->data_buf = *entry_data;
        control = expect_input(ctx, &data->str);
    }
    ctx->flags |= FSCK_HANDLER_HAS_ENTRY;
#endif
    return control;
}

static int
parse_opts (
    struct bookmarkfs_conf_opt const *opts,
    struct parsed_opts               *out
) {
    char const *prompt   = "% ";
    int         translit = '_';

    BACKEND_OPT_START(opts)
    BACKEND_OPT_KEY("prompt") {
        BACKEND_OPT_VAL_START
        prompt = BACKEND_OPT_VAL_STR;
    }
    BACKEND_OPT_KEY("translit") {
        BACKEND_OPT_VAL_START
        char const *str = BACKEND_OPT_VAL_STR;
        if (str[0] != '\0' && str[0] != '/' && str[1] == '\0') {
            translit = str[0];
        }
        BACKEND_OPT_VAL_END
    }
    BACKEND_OPT_END

    out->prompt   = prompt;
    out->translit = translit;
    return 0;
}

static void
print_entry (
    struct bookmarkfs_fsck_data const *data
) {
    char name_buf[sizeof(data->name)];
    escape_control_chars(name_buf, sizeof(name_buf), data->name, '?');

    printf("id:   %" PRIu64 "\nname: %s\n", data->id, name_buf);
}

static int
fsck_handler_create (
    struct bookmarkfs_conf_opt const  *opts,
    uint32_t                           flags,
    void                             **handler_ctx_ptr
) {
    struct parsed_opts parsed_opts = { 0 };
    if (0 != parse_opts(opts, &parsed_opts)) {
        return -1;
    }

    struct handler_ctx *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct handler_ctx) {
        .parent_id = UINT64_MAX,
        .opts      = parsed_opts,
        .flags     = flags,
    };
    *handler_ctx_ptr = ctx;
    return 0;
}

static void
fsck_handler_destroy (
    void *handler_ctx
) {
    struct handler_ctx *ctx = handler_ctx;

    free(ctx);
}

static int
fsck_handler_run (
    void                               *handler_ctx,
    int                                 why,
    union bookmarkfs_fsck_handler_data *data
) {
    struct handler_ctx *ctx = handler_ctx;

    if (why > 0) {
        return handle_entry(ctx, why, data);
    }
#ifdef BOOKMARKFS_INTERACTIVE_FSCK
    if (ctx->flags & FSCK_HANDLER_EXPECT_INPUT) {
        return handle_input(ctx, data);
    }
    if (ctx->flags & FSCK_HANDLER_INHIBIT_NEXT) {
        ctx->flags &= ~FSCK_HANDLER_INHIBIT_NEXT;
        return expect_input(ctx, &data->str);
    }
#endif
    return BOOKMARKFS_FSCK_NEXT;
}

struct bookmarkfs_fsck_handler const fsck_handler_simple = {
    .create  = fsck_handler_create,
    .destroy = fsck_handler_destroy,
    .run     = fsck_handler_run,
};
