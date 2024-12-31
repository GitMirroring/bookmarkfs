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

/**
 * A watcher_poll() call returning WATCHER_POLL_ERR
 * most likely means that the file being watched has gone.
 *
 * When the file is back, calling watcher_poll() again should
 * return WATCHER_POLL_CHANGED, and the watcher should continue to work.
 */
#define WATCHER_POLL_ERR       -1
#define WATCHER_POLL_NOCHANGE   0
#define WATCHER_POLL_CHANGED    1

/**
 * Always use fstat() to detect file change.
 */
#define WATCHER_FALLBACK  ( 1u << 0 )
/**
 * Nothing is performed on the filesystem,
 * and watcher_poll() always returns WATCHER_POLL_NOCHANGE.
 */
#define WATCHER_NOOP      ( 1u << 1 )

/**
 * Bit to shift when applying sandbox flags to the watcher.
 */
#define WATCHER_SANDBOX_FLAGS_OFFSET  16

struct watcher;

/**
 * Creates a watcher that watches for changes of a single file.
 *
 * The `flags` argument is a bit combination of watcher flags
 * and sandbox flags.  The latter is used for
 * initializing the sandbox for the internal worker thread.
 */
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

/**
 * Check whether the file associated with the watcher
 * has changed since the last watcher_poll() call
 * (or, if not yet called, since watcher initialization).
 *
 * Returns one of WATCHER_POLL_*.
 */
int
watcher_poll (
    struct watcher *w
);


#endif  /* !defined(BOOKMARKFS_WATCHER_H_) */
