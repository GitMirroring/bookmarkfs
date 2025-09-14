#
# Copyright (C) 2024  CismonX <admin@cismon.net>
#
# Copying and distribution of this file, with or without modification, are
# permitted in any medium without royalty, provided the copyright notice and
# this notice are preserved.  This file is offered as-is, without any warranty.
#

EXTRA_DIST = package.m4 testsuite.at $(TESTSUITE) $(TESTS_)

TESTS_ = lib_hash.at lib_prng.at lib_watcher.at lib_sandbox.at  \
         lib_hashmap.at fs_basic.at fs_regrw.at fs_dents.at fs_assoc.at  \
         fs_times.at fs_xattr.at misc_base16.at misc_base64.at  \
         misc_uuid.at

# Helper programs for testing

check_HEADERS  = check_util.h
check_PROGRAMS = check-misc

check_misc_CPPFLAGS = -I$(top_srcdir)/src
check_misc_LDADD    = $(top_builddir)/src/libbookmarkfs_check.la
check_misc_SOURCES  = check_misc.c

if BOOKMARKFS_UTIL
  check_PROGRAMS += check-util-lib

  check_util_lib_CPPFLAGS = -I$(top_srcdir)/src
  check_util_lib_LDADD    = $(top_builddir)/src/libbookmarkfs_util.la
  check_util_lib_SOURCES  = check_lib.c check_watcher.c  \
                            check_hashmap.c check_util.c

if SANDBOX
  check_util_lib_SOURCES += check_sandbox.c
endif  # SANDBOX
endif  # BOOKMARKFS_UTIL

if BOOKMARKFS_MOUNT
  check_PROGRAMS += check-fs

  check_fs_CPPFLAGS = -I$(top_srcdir)/src
  check_fs_LDADD    =
  check_fs_SOURCES  = check_fs.c check_fs_dents.c check_fs_regrw.c  \
                      check_fs_times.c check_util.c

if BOOKMARKFS_UTIL
  check_fs_LDADD += $(top_builddir)/src/libbookmarkfs_util.la
else
  check_fs_LDADD += $(BOOKMARKFS_UTIL_LIBS)
endif  # BOOKMARKFS_UTIL
endif  # BOOKMARKFS_MOUNT

# Autotest setup

AUTOTEST  = $(AUTOM4TE) --language=autotest
TESTSUITE = $(srcdir)/testsuite

$(srcdir)/package.m4: $(top_srcdir)/configure.ac
	{  \
	    echo '# Signature of the current package.';                       \
	    echo 'm4_define([AT_PACKAGE_NAME],      [@PACKAGE_NAME@])';       \
	    echo 'm4_define([AT_PACKAGE_TARNAME],   [@PACKAGE_TARNAME@])';    \
	    echo 'm4_define([AT_PACKAGE_VERSION],   [@PACKAGE_VERSION@])';    \
	    echo 'm4_define([AT_PACKAGE_STRING],    [@PACKAGE_STRING@])';     \
	    echo 'm4_define([AT_PACKAGE_BUGREPORT], [@PACKAGE_BUGREPORT@])';  \
	} >'$(srcdir)/package.m4'

$(TESTSUITE): $(srcdir)/testsuite.at $(srcdir)/package.m4 $(TESTS_)
	$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	mv $@.tmp $@

check-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' $(TESTSUITEFLAGS)

clean-local:
	test ! -f '$(TESTSUITE)' || $(SHELL) '$(TESTSUITE)' --clean
