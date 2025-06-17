dnl
dnl  Copyright (C) 2024  CismonX <admin@cismon.net>
dnl
dnl  Copying and distribution of this file, with or without modification,
dnl  are permitted in any medium without royalty,
dnl  provided the copyright notice and this notice are preserved.
dnl  This file is offered as-is, without any warranty.
dnl

dnl
dnl  EX_FEAT(feature, [default-value], description, [action-if-enabled],
dnl          [no-ac-define])
dnl
dnl  Provide an option to enable or disable a feature.
dnl
AC_DEFUN([EX_FEAT], [
    m4_pushdef([arg_action_], m4_if([$2], [no], [enable], [disable]))
    m4_pushdef([feat_name_], m4_translit([$1], [-], [_]))
    AC_MSG_CHECKING(m4_normalize([if $3 is enabled]))
    AC_ARG_ENABLE([$1], m4_normalize([
        AS_HELP_STRING([--]arg_action_[-$1], arg_action_ [$3])
    ]), , m4_ifnblank([$2], [
        AS_VAR_SET([enable_]feat_name_, [$2])
    ]))
    AS_VAR_IF([enable_]feat_name_, [yes], [
        AC_MSG_RESULT([yes])
        $4
        m4_ifblank([$5], [
            AC_DEFINE(m4_if(m4_substr(feat_name_, 0, 10), [bookmarkfs], ,
                            [BOOKMARKFS_])[]m4_toupper(feat_name_),
                    [1], [Define to 1 if $3 is enabled.])
        ])
    ], [
        AC_MSG_RESULT([no])
    ])
    AS_VAR_SET([desc_]feat_name_, ["$3"])
    m4_popdef([arg_action_])
    m4_popdef([feat_name_])
])

dnl
dnl  EX_DEP(pkg-name, version, pkg-desc, [action-if-found],
dnl         [required-by-features]...)
dnl
dnl  Check whether a package exists with `pkg-config`,
dnl  and provide an option to specify the package's custom install location.
dnl
AC_DEFUN([EX_DEP], [
    AC_ARG_WITH([$1], m4_normalize([
        AS_HELP_STRING([--with-m4_translit([$1], [_], [-])[[=PKGCONFIGDIR]]],
                [pkg-config search path for $3])
    ]), , [
        AS_VAR_SET([with_$1], [no])
        m4_foreach([feat_name_], [m4_shiftn(4, $@)], [
            AS_VAR_IF([enable_]m4_translit(feat_name_, [-], [_]), [yes], [
                AS_VAR_SET([with_$1], [yes])
            ])
        ])
    ])
    AS_VAR_IF([with_$1], [no], [
        m4_foreach([feat_name_], [m4_shiftn(4, $@)], [
            AS_VAR_IF([enable_]m4_translit(feat_name_, [-], [_]), [yes], [
                AC_MSG_ERROR(m4_normalize([
                    Bad option '[--without-]feat_name_'. The $3 is mandatory
                    for AS_VAR_GET([desc_]m4_translit(feat_name_, [-], [_])).
                ]))
            ])
        ])
    ], [
        AS_VAR_SET([old_pkg_config_path_], ["${PKG_CONFIG_PATH}"])
        AS_VAR_IF([with_$1], [yes], , [
            AS_VAR_SET([PKG_CONFIG_PATH], ["${with_$1}:${PKG_CONFIG_PATH}"])
            export PKG_CONFIG_PATH
        ])
        PKG_CHECK_MODULES(m4_toupper([$1]), [$1 $2], [$4])
        AS_VAR_SET([PKG_CONFIG_PATH], ["${old_pkg_config_path_}"])
    ])
])

dnl
dnl  EX_AMCOND([features]...)
dnl
dnl  Export feature flags to Makefile templates.
dnl
AC_DEFUN([EX_AMCOND], [
    m4_foreach([feat_name_], [$@], [
        AM_CONDITIONAL(m4_translit(feat_name_, [-a-z], [_A-Z]),
                [test x$enable_]m4_translit(feat_name_, [-], [_])[ != xno])
    ])
])

dnl
dnl  EX_FEAT_EXPORT([features]...)
dnl
dnl  Export feature flags to Autoconf output variables,
dnl  similar to the ones set by AM_CONTITIONAL() (`xxx_TRUE` only).
dnl
AC_DEFUN([EX_FEAT_EXPORT], [
    m4_foreach([feat_name_], [$@], [
        m4_pushdef([out_var_], m4_translit(feat_name_, [-a-z], [_A-Z])[_TRUE])
        AS_VAR_IF([enable_]m4_translit(feat_name_, [-], [_]), [yes], [
            AC_SUBST(out_var_, [''])
        ], [
            AC_SUBST(out_var_, ['#'])
        ])
        AM_SUBST_NOTMAKE(out_var_)
        m4_popdef([out_var_])
    ])
])
