/**
* bookmarkfs/src/frontend_util.c
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

#include "frontend_util.h"

#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>

#include "xstd.h"

// Forward declaration start
static void const * bookmarkfs_load (char const *, char const *, void **);
// Forward declaration end

static void const *
bookmarkfs_load (
    char const  *name,
    char const  *prefix,
    void       **handle_ptr
) {
#define MODULE_LIB_NAME(prefix, name)  \
    "$ORIGIN/../lib/bookmarkfs/" prefix "_" name ".so"
#define MODULE_SYM_NAME(prefix, name)  "bookmarkfs_" prefix "_" name
    char *lib_name, *sym_name;

    char const *sep = strchr(name, ':');
    if (sep == NULL) {
        xasprintf(&lib_name, MODULE_LIB_NAME("%s", "%s"), prefix, name);
        xasprintf(&sym_name, MODULE_SYM_NAME("%s", "%s"), prefix, name);
    } else {
        xasprintf(&lib_name, "%.*s", (int)(sep - name), name);
        xasprintf(&sym_name, "%s", sep + 1);
    }

    void const *impl   = NULL;
    void       *handle = dlopen(lib_name, RTLD_NOW);
    if (handle == NULL) {
        log_printf("dlopen(): %s", dlerror());
        goto end;
    }
    *handle_ptr = handle;

    impl = dlsym(handle, sym_name);
    if (impl == NULL) {
        log_printf("dlsym(): %s", dlerror());
        goto end;
    }

  end:
    free(lib_name);
    free(sym_name);
    return impl;
}

struct bookmarkfs_backend const *
bookmarkfs_load_backend (
    char const  *name,
    void       **handle_ptr
) {
    return bookmarkfs_load(name, "backend", handle_ptr);
}

struct bookmarkfs_fsck_handler const *
bookmarkfs_load_fsck_handler (
    char const  *name,
    void       **handle_ptr
) {
    return bookmarkfs_load(name, "fsck_handler", handle_ptr);
}

void
bookmarkfs_opts_add (
    struct bookmarkfs_conf_opt **opts_ptr,
    char                        *opt_str
) {
    struct bookmarkfs_conf_opt *opt = xmalloc(sizeof(*opt));
    *opt = (struct bookmarkfs_conf_opt) {
        .key  = opt_str,
        .next = *opts_ptr,
    };
    *opts_ptr = opt;

    char *sep = strchr(opt_str, '=');
    if (sep != NULL) {
        *sep = '\0';
        opt->val = sep + 1;
    }
}

void
bookmarkfs_opts_free (
    struct bookmarkfs_conf_opt *opts
) {
    for (struct bookmarkfs_conf_opt *next; opts != NULL; opts = next) {
        next = opts->next;
        free(opts);
    }
}

void
bookmarkfs_unload (
    void *handle
) {
    if (handle == NULL) {
        return;
    }
    if (0 != dlclose(handle)) {
        log_printf("dlclose(): %s", dlerror());
    }
}

int
print_subopt_err_ (
    int         err_type,
    char const *optstr
) {
    switch (err_type) {
      case SUBOPT_ERR_BAD_KEY_:
        log_printf("bad option '-o %s'", optstr);
        break;

      case SUBOPT_ERR_NO_VAL_:
        log_printf("no value given for option '-o %s'", optstr);
        break;

      case SUBOPT_ERR_HAS_VAL_:
        log_printf("unexpected value for option '-o %s'", optstr);
        break;

      case SUBOPT_ERR_BAD_VAL_:
        log_printf("invalid value for option '-o %s'", optstr);
        break;
    }

    return -1;
}
