/**
 * bookmarkfs/src/watcher.h
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

#ifndef BOOKMARKFS_WATCHER_H_
#define BOOKMARKFS_WATCHER_H_

#include <stdint.h>

// If we want this on Linux, we have to whitelist a bunch of syscalls
// like clone(), fanotify_init(), etc., which are generally okay,
// but not used anywhere else in the BookmarkFS codebase.
//
// The list should be kept short, since anything added there may
// introduce extra syscall filtering overhead.
#if defined(__FreeBSD__)
#  define WATCHER_CAN_CREATE_IN_SANDBOX
#endif

#define WATCHER_FALLBACK  ( 1u << 0 )
#define WATCHER_NOOP      ( 1u << 1 )

#define WATCHER_SANDBOX_FLAGS_OFFSET  16

struct watcher;

struct watcher *
watcher_create (
    int         dirfd,
    char const *name,
    uint32_t    flags
);

void
watcher_destroy (
    struct watcher *w
);

int
watcher_poll (
    struct watcher *w
);

#endif  /* !defined(BOOKMARKFS_WATCHER_H_) */
