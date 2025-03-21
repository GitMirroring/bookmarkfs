dnl
dnl  Copyright (C) 2024  CismonX <admin@cismon.net>
dnl
dnl  Copying and distribution of this file, with or without modification,
dnl  are permitted in any medium without royalty,
dnl  provided the copyright notice and this notice are preserved.
dnl  This file is offered as-is, without any warranty.
dnl

AC_PREREQ([2.70])
AC_INIT([bookmarkfs], [0.1.0], [bug-bookmarkfs@nongnu.org])
AC_CONFIG_SRCDIR([bookmarkfs.pc.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_TESTDIR([tests])
AM_INIT_AUTOMAKE([1.14 foreign no-installinfo no-installman])
AM_EXTRA_RECURSIVE_TARGETS([install-man])
AM_MISSING_PROG([AUTOM4TE], [autom4te])
PKG_PROG_PKG_CONFIG([0.27])
LT_PREREQ([2.4.3])
LT_INIT([disable-static dlopen])

# -- Checks for operating system --

AC_CANONICAL_HOST
AS_CASE(["${host_os}"], [linux*], [
    AS_VAR_SET([host_os_is_linux], [yes])
], [freebsd*], [
    AS_VAR_SET([host_os_is_freebsd], [yes])
], [
    AC_MSG_ERROR(m4_normalize([Unsupported platform "${host_os}".]))
])

# -- Checks for programs --

AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# -- Checks for features --

EX_FEAT([bookmarkfs-util], [no], [the BookmarkFS utility library])

EX_FEAT([bookmarkctl], [no], [the bookmarkctl program])

EX_FEAT([bookmarkfs-fsck], [no], [the fsck.bookmarkfs program], [
    AS_VAR_SET([enable_bookmarkfs_util], [yes])
])

EX_FEAT([bookmarkfs-mkfs], [no], [the mkfs.bookmarkfs program])

EX_FEAT([bookmarkfs-mount], [no], [the mount.bookmarkfs program], [
    AS_VAR_SET([enable_bookmarkfs_util], [yes])
])

EX_FEAT([sandbox], [yes], [sandboxing], [
    AC_DEFINE([BOOKMARKFS_SANDBOX], [1],
            [Define to 1 if sandboxing is enabled.])
])

AS_VAR_IF([host_os_is_linux], [yes], [
    EX_FEAT([sandbox-landlock], [yes], [Landlock features for sandboxing], [
        AC_DEFINE([BOOKMARKFS_SANDBOX_LANDLOCK], [1],
                [Define to 1 if Landlock is enabled for sandboxing.])
    ])
])

EX_FEAT([xxhash-inline], [no], [using xxhash as a header-only library], [
    AC_DEFINE([BOOKMARKFS_XXHASH_INLINE], [1],
            [Define to 1 if using xxhash as a header-only library.])
])

EX_FEAT([bookmarkfs-debug], [no], [debugging features for BookmarkFS], [
    AC_DEFINE([BOOKMARKFS_DEBUG], [1],
            [Define to 1 if BookmarkFS debugging is enabled.])
])

EX_FEAT([backend-firefox], [no], [Firefox backend], [
    AS_VAR_SET([enable_bookmarkfs_util], [yes])
])

EX_FEAT([backend-firefox-write], [yes],
        [write support for the Firefox backend],
[
    AC_DEFINE([BOOKMARKFS_BACKEND_FIREFOX_WRITE], [1],
            [Define to 1 if the Firefox backend supports writing.])
])

EX_FEAT([backend-chromium], [no], [Chromium backend], [
    AS_VAR_SET([enable_bookmarkfs_util], [yes])
])

EX_FEAT([backend-chromium-write], [yes],
        [write support for the Chromium backend],
[
    AC_DEFINE([BOOKMARKFS_BACKEND_CHROMIUM_WRITE], [1],
            [Define to 1 if the Chromium backend supports writing.])
])

EX_FEAT([native-watcher], [yes], [platform-specific file watcher], [
    AC_DEFINE([BOOKMARKFS_NATIVE_WATCHER], [1],
            [Define to 1 if platform-specific file watcher is enabled.])
])

EX_FEAT([interactive-fsck], [yes],
        [interactive features for fsck.bookmarkfs],
[
    AS_VAR_IF([enable_bookmarkfs_fsck], [no], [
        AS_VAR_SET([enable_interactive_fsck], [no])
    ], [
        AC_DEFINE([BOOKMARKFS_INTERACTIVE_FSCK], [1],
                [Define to 1 if interactive fsck features are enabled.])
    ])
])

EX_FEAT([fsck-handler-tcl], [no], [Tcl-based fsck handler])

# -- Checks for libraries --

AX_PTHREAD

EX_DEP([bookmarkfs], [>= 0.1], [BookmarkFS utility library], [
    AS_VAR_SET([enable_bookmarkfs_util], [no])
])

EX_DEP([fuse3], [>= 3.5], [fuse3 library], , [bookmarkfs-mount])

AS_VAR_IF([host_os_is_linux], [yes], [
    EX_DEP([libseccomp], [>= 2.5], [libseccomp library], , [sandbox])
])

EX_DEP([sqlite3], [>= 3.35], [SQLite3 library], , [backend-firefox])

EX_DEP([jansson], [>= 2.14], [Jansson library], , [backend-chromium])

EX_DEP([nettle], [>= 3.4], [Nettle library], ,
        [backend-firefox-write], [backend-chromium-write])

EX_DEP([readline], [>= 6.0], [GNU Readline library], , [interactive-fsck])

EX_DEP([tcl], [>= 8.6], [Tcl library], , [fsck-handler-tcl])

EX_DEP([liburiparser], [>= 0.9], [uriparser library], ,
        [backend-firefox-write])

EX_DEP([libxxhash], [>= 0.8], [xxHash library], [
    AS_VAR_IF([enable_xxhash_inline], [yes], [
        AS_VAR_SET([LIBXXHASH_LIBS])
    ])
], [bookmarkfs-util])

EX_AMCOND([bookmarkfs-util], [bookmarkctl], [bookmarkfs-fsck],
        [bookmarkfs-mkfs], [bookmarkfs-mount], [sandbox], [backend-firefox],
        [backend-firefox-write], [backend-chromium], [backend-chromium-write],
        [interactive-fsck], [fsck-handler-tcl])
EX_FEAT_EXPORT([bookmarkfs-debug], [sandbox-landlock], [native-watcher])

# -- Checks for compiler builtins and attributes --

AX_GCC_BUILTIN([__builtin_ctz])
AX_GCC_BUILTIN([__builtin_ctzl])
AX_GCC_BUILTIN([__builtin_expect])
AX_GCC_BUILTIN([__builtin_unreachable])
AX_GCC_FUNC_ATTRIBUTE([cold])
AX_GCC_FUNC_ATTRIBUTE([destructor])
AX_GCC_FUNC_ATTRIBUTE([malloc])
AX_GCC_FUNC_ATTRIBUTE([noreturn])
AX_GCC_FUNC_ATTRIBUTE([pure])
AX_GCC_FUNC_ATTRIBUTE([returns_nonnull])
AX_GCC_FUNC_ATTRIBUTE([visibility])
AX_GCC_VAR_ATTRIBUTE([unused])

AM_CONDITIONAL([HIDE_MODULE_SYMBOLS],
        [test x${ax_cv_have_func_attribute_visibility} != xno])

# -- Checks for macros and typedefs --

AC_MSG_CHECKING([for __FILE_NAME__])
AC_COMPILE_IFELSE([
    AC_LANG_SOURCE([[char const *name = __FILE_NAME__;]])
], [
    AC_MSG_RESULT([yes])
], [
    AC_MSG_RESULT([no])
    AS_IF([test . -ef "${srcdir}"], , [
        AS_VAR_SET([no_file_name], [yes])
    ])
])
AM_CONDITIONAL([NO_FILE_NAME], [test x${no_file_name} = xyes])

AC_SYS_LARGEFILE
AS_VAR_IF([ac_have_largefile], [no], [
    AC_MSG_ERROR(m4_normalize([
        64-bit off_t (required by libfuse) is unsupported on this platform.
    ]))
])

m4_version_prereq([2.72], [
    AC_SYS_YEAR2038
    AS_VAR_IF([ac_have_year2038], [no], [
        AC_MSG_ERROR(m4_normalize([
            64-bit time_t is unsupported on this platform.
        ]))
    ])
], [
    dnl  fallback to compile-time check...
    AX_COMPILE_CHECK_SIZEOF([time_t])
])

AX_COMPILE_CHECK_SIZEOF([uintptr_t], [#include <stdint.h>])

# -- Output --

PKG_INSTALLDIR()
AC_DEFINE([BUILDING_BOOKMARKFS], [1], [Define to 1 if building BookmarkFS.])
AC_CONFIG_FILES([
    Makefile
    doc/Makefile
    src/Makefile
    tests/Makefile tests/atlocal
    bookmarkfs.pc
])
AC_OUTPUT
