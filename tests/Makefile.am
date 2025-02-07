#
# Copyright (C) 2024  CismonX <admin@cismon.net>
#
# Copying and distribution of this file, with or without modification, are
# permitted in any medium without royalty, provided the copyright notice and
# this notice are preserved.  This file is offered as-is, without any warranty.
#

EXTRA_DIST = package.m4 testsuite.at $(TESTSUITE) $(TESTSUITE_AT_)

TESTSUITE_AT_ = lib_hash.at lib_prng.at

# Helper programs for testing

check_PROGRAMS = check-bookmarkfs-util

check_bookmarkfs_util_CPPFLAGS = -I$(top_srcdir)/src
check_bookmarkfs_util_LDADD    = $(top_builddir)/src/libbookmarkfs_util.la
check_bookmarkfs_util_SOURCES  = check_lib.c

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

$(TESTSUITE): $(srcdir)/testsuite.at $(srcdir)/package.m4 $(TESTSUITE_AT_)
	$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	mv $@.tmp $@

check-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' $(TESTSUITEFLAGS)

installcheck-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' $(TESTSUITEFLAGS) AUTOTEST_PATH='$(bindir)'

clean-local:
	test ! -f '$(TESTSUITE)' || $(SHELL) '$(TESTSUITE)' --clean
