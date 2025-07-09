<!--
  Copyright (C) 2024  CismonX <admin@cismon.net>

  Copying and distribution of this file, with or without modification, are
  permitted in any medium without royalty, provided the copyright notice and
  this notice are preserved.  This file is offered as-is, without any warranty.
-->


Requirements
------------

 ### Operating System

  Currently, BookmarkFS runs on the following operating systems:

  - GNU/Linux
  - FreeBSD (with caveats)

  See the user manual for comments if you wish to port BookmarkFS to other
  operating systems.

 ### Dependencies

  In addition to the OS kernel and a POSIX-compatible libc,
  BookmarkFS depends on a few third-party libraries:

  | Library Name | Minimal Version |
  | ------------ | --------------- |
  | libfuse      | 3.5             |
  | libseccomp   | 2.5             |
  | SQLite       | 3.35            |
  | [Jansson]    | 2.14            |
  | [Nettle]     | 3.4             |
  | GNU Readline | 6.0             |
  | [Tcl]        | 8.6             |
  | [uriparser]  | 0.9             |
  | xxHash       | 0.8             |

  Some of them can be optional, depending on which components of BookmarkFS
  are to be built, and which features are enabled.

 ### Build Tools

  - The GNU build system
    * Autoconf
    * Automake
    * Libtool
    * Autoconf Archive
  - `pkg-config`
  - POSIX-compatible `make`
    * One with `VPATH` support is recommended
  - C99-capable C compiler
    * GCC or Clang is recommended

  Optionally:

  - GNU Texinfo
    * For building the user manual


Installation
------------

 ### Configure Build

  Select a build directory and generate configuration scripts:

    $ mkdir build && cd build
    $ autoreconf -i ..

  To list all available configuration options, run:

    $ ../configure --help

  BookmarkFS has multiple components.  By default, none will be built.
  Except for the utility library, each component can be enabled independently:

  - The BookmarkFS utility library: `--enable-bookmarkfs-util`
    * Requires: libseccomp (Linux-only), xxHash
    * Automatically enabled if required by other components
  - The `mount.bookmarkfs` program: `--enable-bookmarkfs-mount`
    * Requires: libfuse, bookmarkfs-util
  - The `fsck.bookmarkfs` program: `--enable-bookmarkfs-fsck`
    * Requires: Readline, bookmarkfs-util
  - The `mkfs.bookmarkfs` program: `--enable-bookmarkfs-mkfs`
  - The `bookmarkctl` program: `--enable-bookmarkctl`
  - The Firefox backend: `--enable-backend-firefox`
    * Requires: SQLite, Nettle, uriparser, bookmarkfs-util
  - The Chromium backend: `--enable-backend-chromium`
    * Requires: Jansson, Nettle, bookmarkfs-util
  - The Tcl-based handler for `fsck.bookmarkfs`: `--enable-fsck-handler-tcl`
    * Requires: Tcl

  For each of the required third-party libraries, if installed in a
  custom location, it should be specified with `--with-<foo>=<pkgconfdir>`,
  where `<foo>` is the library name, and `<pkgconfdir>` is the directory
  holding its pkg-config file.

  Other options:

  - `--disable-sandbox`
    * Build the utility library without sandboxing features
    * No longer requires: libseccomp
  - `--disable-sandbox-landlock` (Linux-only)
    * Disable the [Landlock] feature in the sandbox implementation
  - `--disable-xxhash-inline`
    * Do not use xxHash as a header-only library
  - `--disable-backend-firefox-write`
    * Build the Firefox backend without write features
    * No longer requires: Nettle, uriparser
  - `--disable-backend-chromium-write`
    * Build the Chromium backend without write features
    * No longer requires: Nettle
  - `--enable-boookmarkfs-debug`
    * Add more run-time checks and logs for debugging
  - `--disable-native-watcher`
    * Build the file watcher without platform-specific API dependency
  - `--disable-interactive-fsck`
    * Disable interactive features for `fsck.bookmarkfs`
    * No longer requires: Readline

  An example configuration:

    $ ../configure --prefix=$HOME/.local  \
    >         --enable-bookmarkfs-mount --enable-backend-firefox  \
    >         --with-fuse3=$HOME/.local/lib/pkgconfig  \
    >         CFLAGS='-O2'

 ### Build and Install

  After configuration, build the binaries:

    $ make

  Run tests:

    $ make check

  Install the binaries:

    $ make install-exec

  Install the development headers and the pkg-config file:

    $ make install-data

  Install the user manual:

    $ make install-info

  Uninstall:

    $ make uninstall


Notes
-----

 ### Linking to an Existing BookmarkFS Utility Library

  If a BookmarkFS utility library is already installed, it can be
  specified with `--with-bookmarkfs-util[=<pkgconfdir>]` during configuration,
  in the same way as with other dependencies.

  In this case, the utility library will not be built from source,
  and other components will link to the specified library instead.

 ### FreeBSD and GNU libiconv

  NOTE: You may skip this section if _not_ building the Chromium backend.

  Before 10.0-RELEASE, the FreeBSD libc did not have a native iconv,
  and linked to GNU libiconv from the `converters/libiconv` port.
  Nowadays, that port is still used by many other ports, thus it is likely
  to be installed on your system, which could be problematic.

  The system `iconv.h` header is installed as `/usr/include/iconv.h`,
  and the libiconv one `/usr/local/include/iconv.h`.
  When we `#include <iconv.h>`, the latter may be chosen by the preprocessor
  instead of the former, where `iconv*()` function calls are redefined to
  `libiconv*()` ones, which results in "undefined symbols" error
  since we're not linking to libiconv.

  To workaround this issue, you may add preprocessor flags
  `-D_LIBICONV_H -include /usr/include/iconv.h` to choose the system iconv,
  or linker flags `-L/usr/local/lib -liconv` to choose GNU libiconv.


<!-- reflinks -->

[Jansson]:   https://github.com/akheron/jansson
[Nettle]:    https://www.lysator.liu.se/~nisse/nettle/
[Landlock]:  https://docs.kernel.org/userspace-api/landlock.html
[Tcl]:       https://www.tcl-lang.org/
[uriparser]: https://github.com/uriparser/uriparser
