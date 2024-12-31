#
# Copyright (C) 2024  CismonX <admin@cismon.net>
#
# Copying and distribution of this file, with or without modification, are
# permitted in any medium without royalty, provided the copyright notice and
# this notice are preserved.  This file is offered as-is, without any warranty.
#

info_TEXINFOS  = bookmarkfs.texi
dist_man1_MANS =

bookmarkfs_TEXINFOS = fdl.texi

if BOOKMARKCTL
  dist_man1_MANS += bookmarkctl.1
endif  # BOOKMARKCTL

if BOOKMARKFS_FSCK
  dist_man1_MANS += fsck.bookmarkfs.1
endif  # BOOKMARKFS_FSCK

if BOOKMARKFS_MKFS
  dist_man1_MANS += mkfs.bookmarkfs.1
endif  # BOOKMARKFS_MKFS

if BOOKMARKFS_MOUNT
  dist_man1_MANS += mount.bookmarkfs.1
endif  # BOOKMARKFS_MOUNT
