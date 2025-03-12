/**
 * bookmarkfs/src/frontend_util.h
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

#ifndef BOOKMARKFS_FRONTEND_UTIL_H_
#define BOOKMARKFS_FRONTEND_UTIL_H_

#include <stdlib.h>

#include <unistd.h>

#include "backend.h"
#include "fsck_handler.h"
#include "xstd.h"

#define OPT_START(argc, argv, optstr)  \
    for (int opt_; -1 != (opt_ = getopt(argc, argv, ":" optstr)); ) {  \
        switch (opt_) {
#define OPT_OPT(opt)  case opt:
#define OPT_NOVAL       \
          case ':':     \
            log_printf("no value provided for option '-%c'", optopt);  \
            return -1;
#define OPT_END         \
          default:      \
            log_printf("invalid option '-%c'", optopt);  \
            return -1;  \
        }               \
    }                   \
    argc -= optind;     \
    argv += optind;

#define SUBOPT_START(opts)                                               \
    while (optarg[0] != '\0') {                                          \
        char *val_;                                                      \
        char *suboptstr_ = optarg;                                       \
        /* Safe to cast away constness - POSIX guarantees that
           getsubopt() does not attempt to modify the strings. */        \
        int subopt_ = getsubopt(&optarg, (char *const *)(opts), &val_);  \
        switch (subopt_) {
#define SUBOPT_OPT(opt)        break; case (opt):
#define SUBOPT_OPT_DEFAULT()   break; default:
#ifdef __FreeBSD__
#  define SUBOPT_OPT_FALLBACK_WORKAROUND_  if (val_ != NULL) val_[-1] = '=';
#else
#  define SUBOPT_OPT_FALLBACK_WORKAROUND_
#endif
// Upon getsubopt() returning -1, the value of `optionp`
// is left unspecified by POSIX.
//
// The BSDs store the original option string to `suboptarg`;
// glibc stores it to `optionp`; musl does neither.
//
// We should not depend on unspecified behavior.
#define SUBOPT_OPT_FALLBACK()  SUBOPT_OPT(-1) SUBOPT_OPT_FALLBACK_WORKAROUND_
#define SUBOPT_END  \
            break;  \
        }           \
    }               \
    break;
#define SUBOPT_STR  (suboptstr_)
#define SUBOPT_VAL  (val_)

#define SUBOPT_ERR_BAD_KEY_  0
#define SUBOPT_ERR_NO_VAL_   1
#define SUBOPT_ERR_HAS_VAL_  2
#define SUBOPT_ERR_BAD_VAL_  3
#define SUBOPT_ERR_(err_type)  \
    print_subopt_err_(SUBOPT_ERR_##err_type##_, suboptstr_)
#define SUBOPT_HAS_VAL  if (val_ == NULL) return SUBOPT_ERR_(NO_VAL);
#define SUBOPT_NO_VAL   if (val_ != NULL) return SUBOPT_ERR_(HAS_VAL);
#define SUBOPT_ERR_BAD_KEY()  SUBOPT_ERR_(BAD_KEY)
#define SUBOPT_ERR_BAD_VAL()  SUBOPT_ERR_(BAD_VAL)

struct bookmarkfs_backend const *
bookmarkfs_load_backend (
    char const  *name,
    void       **handle_ptr
);

struct bookmarkfs_fsck_handler const *
bookmarkfs_load_fsck_handler (
    char const  *name,
    void       **handle_ptr
);

void
bookmarkfs_opts_add (
    struct bookmarkfs_conf_opt **opts_ptr,
    char                        *opt_str
);

void
bookmarkfs_opts_free (
    struct bookmarkfs_conf_opt *opts
);

void
bookmarkfs_unload (
    void *handle
);

FUNCATTR_COLD
int
print_subopt_err_ (
    int         err_type,
    char const *optstr
);

#endif  /* !defined(BOOKMARKFS_FRONTEND_UITL_H_) */
