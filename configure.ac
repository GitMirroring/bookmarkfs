dnl
dnl  Copyright (C) 2024  CismonX <admin@cismon.net>
dnl
dnl  Copying and distribution of this file, with or without modification,
dnl  are permitted in any medium without royalty,
dnl  provided the copyright notice and this notice are preserved.
dnl  This file is offered as-is, without any warranty.
dnl

AC_PREREQ([2.70])
AC_INIT([bookmarkfs], [0.1.4], [bug-bookmarkfs@nongnu.org])
AC_CONFIG_SRCDIR([src/bookmarkfs_util.pc.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_TESTDIR([tests])
AM_INIT_AUTOMAKE([1.14 foreign no-installinfo])
AM_MISSING_PROG([AUTOM4TE], [autom4te])
PKG_PROG_PKG_CONFIG([0.27])
LT_PREREQ([2.4.3])
LT_INIT([disable-static dlopen])

# -- Checks for operating system --

AC_CANONICAL_HOST
AS_CASE(["${host_os}"], [linux*], [
    AS_VAR_SET([host_os], [linux])
], [freebsd*], [
    AS_VAR_SET([host_os], [freebsd])
], [
    AC_MSG_ERROR([Unsupported platform "${host_os}".])
])

# -- Checks for programs --

AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# -- Checks for features --

EX_FEAT([bookmarkctl], [E], [the bookmarkctl program])

EX_FEAT([bookmarkfs-fsck], [E], [the fsck.bookmarkfs program], [
    AS_VAR_SET([enable_bookmarkfs_util],  [yes])
    AS_VAR_SET([enable_interactive_fsck], [yes])
])

EX_FEAT([bookmarkfs-mkfs], [E], [the mkfs.bookmarkfs program])

EX_FEAT([bookmarkfs-mount], [E], [the mount.bookmarkfs program], [
    AS_VAR_SET([enable_bookmarkfs_util], [yes])
])

EX_FEAT([sandbox], [YDE], [sandboxing])

AS_VAR_IF([host_os], [linux], [
    EX_FEAT([sandbox-landlock], [YD], [using Landlock for sandboxing])
])

EX_FEAT([xxhash-inline], [YD], [using xxhash as a header-only library])

EX_FEAT([bookmarkfs-debug], [D], [debugging features for BookmarkFS])

EX_FEAT([backend-firefox], [E], [Firefox backend], [
    AS_VAR_SET([enable_bookmarkfs_util],       [yes])
    AS_VAR_SET([enable_backend_firefox_write], [yes])
])

EX_FEAT([backend-firefox-write], [DE], [write support for the Firefox backend])

EX_FEAT([backend-chromium], [E], [Chromium backend], [
    AS_VAR_SET([enable_bookmarkfs_util],        [yes])
    AS_VAR_SET([enable_backend_chromium_write], [yes])
])

EX_FEAT([backend-chromium-write], [DE],
        [write support for the Chromium backend])

EX_FEAT([native-watcher], [YD], [platform-specific file watcher])

EX_FEAT([interactive-fsck], [DE], [interactive features for fsck.bookmarkfs])

EX_FEAT([fsck-handler-tcl], [E], [Tcl-based fsck handler])

EX_FEAT([bookmarkfs-util], [E], [the BookmarkFS utility library], [
    EX_DEP([bookmarkfs_util], [>= 0.1], [BookmarkFS utility library], [
        AS_VAR_SET([enable_bookmarkfs_util], [no])
    ])
])

# -- Checks for libraries --

AX_PTHREAD

EX_DEP([fuse3], [>= 3.5], [fuse3 library], , [bookmarkfs-mount])

AS_VAR_IF([host_os], [linux], [
    EX_DEP([libseccomp], [>= 2.5], [libseccomp library], , [sandbox])
])

EX_DEP([sqlite3], [>= 3.35], [SQLite3 library], , [backend-firefox])

EX_DEP([jansson], [>= 2.14], [Jansson library], , [backend-chromium])

EX_DEP([nettle], [>= 3.4], [Nettle library], , [backend-chromium-write])

EX_DEP([readline], [>= 6.0], [GNU Readline library], , [interactive-fsck])

EX_DEP([tcl], [>= 8.6], [Tcl library], , [fsck-handler-tcl])

EX_DEP([libxxhash], [>= 0.8], [xxHash library], [
    AS_VAR_IF([enable_xxhash_inline], [yes], [
        AS_VAR_SET([LIBXXHASH_LIBS])
    ])
], [bookmarkfs-util])

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

AX_COMPILE_CHECK_SIZEOF([time_t])
AX_COMPILE_CHECK_SIZEOF([uintptr_t], [#include <stdint.h>])

# -- Output --

PKG_INSTALLDIR()
AC_DEFINE([BUILDING_BOOKMARKFS], [1], [Define to 1 if building BookmarkFS.])
AC_CONFIG_FILES([
    Makefile
    doc/Makefile
    src/Makefile src/bookmarkfs_util.pc
    tests/Makefile tests/atlocal
])
AC_OUTPUT
