#
# Copyright (C) 2024  CismonX <admin@cismon.net>
#
# Copying and distribution of this file, with or without modification, are
# permitted in any medium without royalty, provided the copyright notice and
# this notice are preserved.  This file is offered as-is, without any warranty.
#

bin_PROGRAMS       =
pkginclude_HEADERS = backend.h common.h ioctl.h fsck_handler.h version.h
noinst_HEADERS     = base16.h base64.h backend_util.h db.h defs.h  \
                     frontend_util.h fs_ops.h fsck_ops.h fsck_util.h json.h  \
                     lib.h macros.h uuid.h xattr.h xstd.h
check_LTLIBRARIES  = libbookmarkfs_check.la
lib_LTLIBRARIES    =
pkglib_LTLIBRARIES =
pkgconfig_DATA     = bookmarkfs_util.pc

BASE_CPPFLAGS_ =
if NO_FILE_NAME
  BASE_CPPFLAGS_ += -DBOOKMARKFS_SRCDIR=\"$(srcdir)\"
endif  # NO_FILE_NAME

MODULE_CFLAGS_ =
if HIDE_MODULE_SYMBOLS
  MODULE_CFLAGS_ += -fvisibility=hidden
endif  # HIDE_MODULE_SYMBOLS

UTIL_LIBS_ =
if BOOKMARKFS_UTIL
  UTIL_LIBS_ += libbookmarkfs_util.la
else
  UTIL_LIBS_ += $(BOOKMARKFS_UTIL_LIBS)
endif  # BOOKMARKFS_UTIL

BACKEND_SOURCES_  = backend_util.c lib.c xstd.c
FRONTEND_SOURCES_ = frontend_util.c xstd.c
LT_VERSION_INFO_  = -version-info 0:0:0
MODULE_LDFLAGS_   = $(LT_VERSION_INFO_) -module

if BOOKMARKFS_UTIL
  lib_LTLIBRARIES    += libbookmarkfs_util.la
  pkginclude_HEADERS += hash.h hashmap.h prng.h sandbox.h watcher.h

  libbookmarkfs_util_la_CPPFLAGS = $(BASE_CPPFLAGS_) $(PTHREAD_CFLAGS)  \
                                   $(LIBXXHASH_CFLAGS)
  libbookmarkfs_util_la_LDFLAGS  = $(LT_VERSION_INFO_)
  libbookmarkfs_util_la_LIBADD   = $(PTHREAD_LIBS) $(LIBXXHASH_LIBS)
  libbookmarkfs_util_la_SOURCES  = hash.c hashmap.c prng.c sandbox.c  \
                                   version.c watcher.c xstd.c

if SANDBOX
  libbookmarkfs_util_la_CPPFLAGS += $(LIBSECCOMP_CFLAGS)
  libbookmarkfs_util_la_LIBADD   += $(LIBSECCOMP_LIBS)
endif  # SANDBOX
endif  # BOOKMARKFS_UTIL

if BOOKMARKCTL
  bin_PROGRAMS += bookmarkctl

  bookmarkctl_CPPFLAGS = $(BASE_CPPFLAGS_)
  bookmarkctl_SOURCES  = bookmarkctl.c fsck_util.c xattr.c xstd.c
endif  # BOOKMARKCTL

if BOOKMARKFS_FSCK
  bin_PROGRAMS += fsck.bookmarkfs

  fsck_bookmarkfs_CPPFLAGS = $(BASE_CPPFLAGS_)
  fsck_bookmarkfs_LDADD    = $(UTIL_LIBS_)
  fsck_bookmarkfs_SOURCES  = $(FRONTEND_SOURCES_) backend_util.c fsck.c  \
                             fsck_handler_simple.c fsck_offline.c  \
                             fsck_online.c fsck_util.c lib.c

if INTERACTIVE_FSCK
  fsck_bookmarkfs_CPPFLAGS += $(READLINE_CFLAGS)
  fsck_bookmarkfs_LDADD    += $(READLINE_LIBS)
endif  # INTERACTIVE_FSCK
endif  # BOOKMARKFS_FSCK

if BOOKMARKFS_MKFS
  bin_PROGRAMS += mkfs.bookmarkfs

  mkfs_bookmarkfs_CPPFLAGS = $(BASE_CPPFLAGS_)
  mkfs_bookmarkfs_SOURCES  = $(FRONTEND_SOURCES_) mkfs.c
endif  # BOOKMARKFS_MKFS

if BOOKMARKFS_MOUNT
  bin_PROGRAMS += mount.bookmarkfs

  mount_bookmarkfs_CPPFLAGS = $(BASE_CPPFLAGS_) $(FUSE3_CFLAGS)
  mount_bookmarkfs_LDADD    = $(UTIL_LIBS_) $(FUSE3_LIBS)
  mount_bookmarkfs_SOURCES  = $(FRONTEND_SOURCES_) fs_ops.c lib.c mount.c
endif  # BOOKMARKFS_MOUNT

if BACKEND_FIREFOX
  pkglib_LTLIBRARIES += backend_firefox.la

  backend_firefox_la_CFLAGS   = $(MODULE_CFLAGS_)
  backend_firefox_la_CPPFLAGS = $(BASE_CPPFLAGS_) $(SQLITE3_CFLAGS)
  backend_firefox_la_LDFLAGS  = $(MODULE_LDFLAGS_)
  backend_firefox_la_LIBADD   = $(UTIL_LIBS_) $(SQLITE3_LIBS)
  backend_firefox_la_SOURCES  = $(BACKEND_SOURCES_) backend_firefox.c db.c

if BACKEND_FIREFOX_WRITE
  backend_firefox_la_SOURCES += base64.c
endif  # BACKEND_FIREFOX_WRITE
endif  # BACKEND_FIREFOX

if BACKEND_CHROMIUM
  pkglib_LTLIBRARIES += backend_chromium.la

  backend_chromium_la_CFLAGS   = $(MODULE_CFLAGS_)
  backend_chromium_la_CPPFLAGS = $(BASE_CPPFLAGS_) $(JANSSON_CFLAGS)
  backend_chromium_la_LDFLAGS  = $(MODULE_LDFLAGS_)
  backend_chromium_la_LIBADD   = $(UTIL_LIBS_) $(JANSSON_LIBS)
  backend_chromium_la_SOURCES  = $(BACKEND_SOURCES_) backend_chromium.c  \
                                 base16.c json.c uuid.c

if BACKEND_CHROMIUM_WRITE
  backend_chromium_la_CPPFLAGS += $(NETTLE_CFLAGS)
  backend_chromium_la_LIBADD   += $(LIBICONV_LIBS) $(NETTLE_LIBS)
endif  # BACKEND_CHROMIUM_WRITE
endif  # BACKEND_CHROMIUM

if FSCK_HANDLER_TCL
  pkglib_LTLIBRARIES += fsck_handler_tcl.la

  fsck_handler_tcl_la_CFLAGS   = $(MODULE_CFLAGS_)
  fsck_handler_tcl_la_CPPFLAGS = $(BASE_CPPFLAGS_) $(TCL_CFLAGS)
  fsck_handler_tcl_la_LDFLAGS  = $(MODULE_LDFLAGS_)
  fsck_handler_tcl_la_LIBADD   = $(TCL_LIBS)
  fsck_handler_tcl_la_SOURCES  = backend_util.c fsck_handler_tcl.c xstd.c
endif  # FSCK_HANDLER_TCL

libbookmarkfs_check_la_CPPFLAGS = $(BASE_CPPFLAGS_)
libbookmarkfs_check_la_SOURCES  = base16.c base64.c xstd.c
