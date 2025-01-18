/**
 * bookmarkfs/src/fsck_handler_tcl.c
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
#include <stdlib.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <tcl.h>

#include "backend.h"
#include "backend_util.h"
#include "fsck_handler.h"
#include "macros.h"
#include "version.h"
#include "xstd.h"

#define FSCK_HANDLER_UNSAFE        ( 1u << 24 )
#define FSCK_HANDLER_EXPECT_INPUT  ( 1u << 25 )
#define FSCK_HANDLER_INITIALIZED   ( 1u << 26 )

struct handler_ctx {
    Tcl_Interp *interp;
    uint32_t    flags;

    union {
        Tcl_Obj *obj;
        int      fd;
    } script;
};

struct parsed_opts {
    char const *script;
    uint32_t    flags;
};

// Forward declaration start
static void         finalize_tcl  (void) FUNCATTR_DTOR;
static int          init_handler  (struct handler_ctx *);
static Tcl_Interp * init_interp   (uint32_t);
static int          parse_opts    (struct bookmarkfs_conf_opt const *,
                                   struct parsed_opts *);
static void         print_help    (void);
static void         print_version (void);
static int          set_tcl_var   (Tcl_Interp *, char const *, size_t, int);
// Forward declaration end

static void
finalize_tcl (void)
{
    Tcl_Finalize();
}

static int
init_handler (
    struct handler_ctx *ctx
) {
    if (ctx->flags & FSCK_HANDLER_INITIALIZED) {
        return 0;
    }

    struct stat stat_buf;
    if (unlikely(0 != fstat(ctx->script.fd, &stat_buf))) {
        log_printf("fstat(): %s", xstrerror(errno));
        return -1;
    }

    void *buf = mmap(NULL, stat_buf.st_size, PROT_READ | PROT_MAX(PROT_READ),
            MAP_PRIVATE, ctx->script.fd, 0);
    if (unlikely(buf == MAP_FAILED)) {
        log_printf("mmap(): %s", xstrerror(errno));
        return -1;
    }

    Tcl_Interp *interp = ctx->interp;
    int result = Tcl_EvalEx(interp, buf, stat_buf.st_size, TCL_EVAL_GLOBAL);
    munmap(buf, stat_buf.st_size);
    if (result != TCL_OK) {
        log_printf("%s", Tcl_GetStringResult(interp));
        return -1;
    }
    close(ctx->script.fd);
    ctx->script.obj = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(ctx->script.obj);

    ctx->flags |= FSCK_HANDLER_INITIALIZED;
    return 0;
}

static Tcl_Interp *
init_interp (
    uint32_t flags
) {
    Tcl_Interp *interp = Tcl_CreateInterp();
    if (flags & FSCK_HANDLER_UNSAFE) {
        if (TCL_OK != Tcl_Init(interp)) {
            log_printf("%s", Tcl_GetStringResult(interp));
            goto fail;
        }
        Tcl_InitMemory(interp);
    } else {
        xassert(TCL_OK == Tcl_MakeSafe(interp));
    }

#define WITH_NAMESPACE(name)  "bookmarkfs::fsck::" name
#define DO_CREATE_NAMESPACE(interp, name)                          \
    if (NULL == Tcl_CreateNamespace(interp, WITH_NAMESPACE(name),  \
                NULL, NULL)                                        \
    ) {                                                            \
        log_printf("%s", Tcl_GetStringResult(interp));             \
        goto fail;                                                 \
    }
    DO_CREATE_NAMESPACE(interp, "handler");
    DO_CREATE_NAMESPACE(interp, "result");

#define DO_SET_VAR(interp, name, val)                                        \
    if (0 != set_tcl_var(interp, STR_WITHLEN(WITH_NAMESPACE(name)), val)) {  \
        log_printf("%s", Tcl_GetStringResult(interp));                       \
        goto fail;                                                           \
    }
    DO_SET_VAR(interp, "isInteractive",
            !!(flags & BOOKMARKFS_FSCK_HANDLER_INTERACTIVE));
    DO_SET_VAR(interp, "isReadonly",
            !!(flags & BOOKMARKFS_BACKEND_READONLY));
    DO_SET_VAR(interp, "handler::next",         BOOKMARKFS_FSCK_NEXT);
    DO_SET_VAR(interp, "handler::apply",        BOOKMARKFS_FSCK_APPLY);
    DO_SET_VAR(interp, "handler::userInput",    BOOKMARKFS_FSCK_USER_INPUT);
    DO_SET_VAR(interp, "handler::save",         BOOKMARKFS_FSCK_SAVE);
    DO_SET_VAR(interp, "handler::stop",         BOOKMARKFS_FSCK_STOP);
    DO_SET_VAR(interp, "handler::rewind",       BOOKMARKFS_FSCK_REWIND);
    DO_SET_VAR(interp, "handler::skip",         BOOKMARKFS_FSCK_SKIP);
    DO_SET_VAR(interp, "handler::skipChildren", BOOKMARKFS_FSCK_SKIP_CHILDREN);
    DO_SET_VAR(interp, "handler::reset",        BOOKMARKFS_FSCK_RESET);
#define DO_SET_RESULT_VAR(interp, name, val)  \
    DO_SET_VAR(interp, "result::" name, BOOKMARKFS_FSCK_RESULT_##val)
    DO_SET_RESULT_VAR(interp, "nameDuplicate", NAME_DUPLICATE);
    DO_SET_RESULT_VAR(interp, "nameBadChar",   NAME_BADCHAR);
    DO_SET_RESULT_VAR(interp, "nameBadLen",    NAME_BADLEN);
    DO_SET_RESULT_VAR(interp, "nameDotDot",    NAME_DOTDOT);
    DO_SET_RESULT_VAR(interp, "nameInvalid",   NAME_INVALID);

    return interp;

  fail:
    Tcl_DeleteInterp(interp);
    return NULL;
}

static int
parse_opts (
    struct bookmarkfs_conf_opt const *opts,
    struct parsed_opts               *parsed_opts
) {
    char const *script = NULL;

    BACKEND_OPT_START(opts)
    BACKEND_OPT_KEY("script") {
        BACKEND_OPT_VAL_START
        script = BACKEND_OPT_VAL_STR;
    }
    BACKEND_OPT_KEY("unsafe") {
        BACKEND_OPT_NO_VAL
        parsed_opts->flags |= FSCK_HANDLER_UNSAFE;
    }
    BACKEND_OPT_END

    parsed_opts->script = script;
    return 0;
}

static void
print_help (void)
{
    printf("Tcl-based fsck handler for BookmarkFS\n"
            "\n"
            "Options:\n"
            "  script=<path>    Path to Tcl script file\n"
            "  unsafe           Enable unsafe Tcl interpreter features\n"
            "\n"
            "Run 'info bookmarkfs' for more information.\n\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.\n");
}

static void
print_version (void)
{
    printf("bookmarkfs-fsck-handler-tcl %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
    puts(BOOKMARKFS_FEATURE_STRING(DEBUG, "debug"));
}

static int
set_tcl_var (
    Tcl_Interp *interp,
    char const *name,
    size_t      name_len,
    int         val
) {
    Tcl_Obj *name_obj = Tcl_NewStringObj(name, name_len);
    Tcl_Obj *val_obj  = Tcl_NewIntObj(val);

    Tcl_IncrRefCount(name_obj);
    val_obj = Tcl_ObjSetVar2(interp, name_obj, NULL, val_obj,
            TCL_GLOBAL_ONLY | TCL_LEAVE_ERR_MSG);
    Tcl_DecrRefCount(name_obj);
    return val_obj == NULL ? -1 : 0;
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
    flags |= parsed_opts.flags;

    if (parsed_opts.script == NULL) {
        log_puts("script not specified");
        return -1;
    }
    int fd = open(parsed_opts.script, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log_printf("open(): %s", xstrerror(errno));
        return -1;
    }

    Tcl_Interp *interp = init_interp(flags);
    if (interp == NULL) {
        goto fail;
    }

    struct handler_ctx *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct handler_ctx) {
        .interp    = interp,
        .flags     = flags,
        .script.fd = fd,
    };
    *handler_ctx_ptr = ctx;
    return 0;

  fail:
    close(fd);
    return -1;
}

static void
fsck_handler_destroy (
    void *handler_ctx
) {
    struct handler_ctx *ctx = handler_ctx;

    if (ctx->flags & FSCK_HANDLER_INITIALIZED) {
        Tcl_DecrRefCount(ctx->script.obj);
    } else {
        close(ctx->script.fd);
    }
    Tcl_DeleteInterp(ctx->interp);
    free(ctx);
}

static void
fsck_handler_info (
    uint32_t flags
) {
    if (flags & BOOKMARKFS_FSCK_HANDLER_INFO_HELP) {
        print_help();
    } else if (flags & BOOKMARKFS_FSCK_HANDLER_INFO_VERSION) {
        print_version();
    }
}

static int
fsck_handler_run (
    void                               *handler_ctx,
    int                                 why,
    union bookmarkfs_fsck_handler_data *data
) {
    struct handler_ctx *ctx = handler_ctx;

    if (0 != init_handler(ctx)) {
        return -1;
    }
    Tcl_Interp *interp = ctx->interp;

    Tcl_Obj *data_obj = Tcl_NewObj();
    if (why < 0) {
        if (ctx->flags & FSCK_HANDLER_EXPECT_INPUT) {
            Tcl_SetStringObj(data_obj, data->str, -1);
        }
    } else {
        struct bookmarkfs_fsck_handler_entry *entry = &data->entry;
        Tcl_Obj *elems[] = {
            Tcl_NewWideIntObj(entry->data.id),
            Tcl_NewWideIntObj(entry->data.extra),
            Tcl_NewStringObj(entry->data.name,
                    strnlen(entry->data.name, sizeof(entry->data.name))),
            Tcl_NewWideIntObj(entry->parent_id),
        };
        Tcl_SetListObj(data_obj, 4, elems);
    }
    Tcl_Obj *elems[] = { Tcl_NewIntObj(why), data_obj };
    Tcl_Obj *args_obj = Tcl_NewListObj(2, elems);
    Tcl_IncrRefCount(args_obj);

    Tcl_Obj *args[] = { ctx->script.obj, args_obj };
    int result = Tcl_EvalObjv(interp, 2, args, TCL_EVAL_GLOBAL);
    Tcl_DecrRefCount(args_obj);
    if (result != TCL_OK) {
        log_printf("%s", Tcl_GetStringResult(interp));
        return -1;
    }

    Tcl_Obj *result_obj = Tcl_GetObjResult(interp);
    if (TCL_OK != Tcl_ListObjIndex(interp, result_obj, 0, &data_obj)) {
        log_puts("bad return value, cannot convert to list");
        return -1;
    }
    if (TCL_OK != Tcl_GetIntFromObj(interp, data_obj, &result)) {
        log_printf("bad result code '%s'", Tcl_GetString(data_obj));
        return -1;
    }
    switch (result) {
      case BOOKMARKFS_FSCK_USER_INPUT:
        if (!(ctx->flags & BOOKMARKFS_FSCK_HANDLER_INTERACTIVE)) {
            log_printf("bad result code %d, not in interactive mode", result);
            return -1;
        }
        if (TCL_OK != Tcl_ListObjIndex(interp, result_obj, 1, &data_obj)) {
            log_puts("bad return value, no prompt string given");
            return -1;
        }
        data->str = Tcl_GetString(data_obj);
        ctx->flags |= FSCK_HANDLER_EXPECT_INPUT;
        break;

      case BOOKMARKFS_FSCK_APPLY:
        if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
            log_puts("cannot apply, fsck is running in readonly mode");
            return -1;
        }
        if (TCL_OK != Tcl_ListObjIndex(interp, result_obj, 1, &data_obj)) {
            log_puts("bad return value, no new name given");
            return -1;
        }
        struct bookmarkfs_fsck_data *data_buf = &data->entry.data;
        char const *new_name = Tcl_GetString(data_obj);
        strncpy(data_buf->name, new_name, sizeof(data_buf->name));
        // fallthrough
      case BOOKMARKFS_FSCK_NEXT:
      case BOOKMARKFS_FSCK_SAVE:
      case BOOKMARKFS_FSCK_STOP:
      case BOOKMARKFS_FSCK_REWIND:
      case BOOKMARKFS_FSCK_SKIP:
      case BOOKMARKFS_FSCK_SKIP_CHILDREN:
      case BOOKMARKFS_FSCK_RESET:
        ctx->flags &= ~FSCK_HANDLER_EXPECT_INPUT;
        break;

      default:
        log_printf("bad result code %d", result);
        return -1;
    }
    return result;
}

BOOKMARKFS_API
struct bookmarkfs_fsck_handler const bookmarkfs_fsck_handler_tcl = {
    .info    = fsck_handler_info,
    .create  = fsck_handler_create,
    .destroy = fsck_handler_destroy,
    .run     = fsck_handler_run,
};
