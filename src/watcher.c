/**
 * bookmarkfs/src/watcher.c
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "watcher.h"

#include <errno.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(BOOKMARKFS_NATIVE_WATCHER)
#elif defined(__linux__)
#  include <sys/fanotify.h>
#  define WATCHER_IMPL_FANOTIFY
#elif defined(__FreeBSD__)
#  include <sys/event.h>
#  define WATCHER_IMPL_KQUEUE
#endif

#include "sandbox.h"
#include "xstd.h"

#define WATCHER_DEAD  ( 1u << 2 )
#define WATCHER_IDLE  ( 1u << 3 )

// This value is chosen according to how frequent Chromium saves its bookmarks
// (`bookmarks::BookmarkStorage::kSaveDelay`).
//
// See Chromium source code: /components/bookmarks/browser/bookmark_storage.h
#define WATCHER_FALLBACK_POLL_INTERVAL  2500

struct watcher {
    pthread_mutex_t mutex;
    uint32_t        flags;

    char const *name;
    int         dirfd;
#if defined(WATCHER_IMPL_FANOTIFY)
    int         fanfd;
#elif defined(WATCHER_IMPL_KQUEUE)
    int         kqfd;
    int         wfd;
#endif

    pthread_cond_t cond;
    struct stat    old_stat;
    int            pipefd[2];
    pthread_t      worker;
};

// Forward declaration start
static int    impl_init   (struct watcher *);
static void   impl_free   (struct watcher *);
static int    impl_rearm  (struct watcher *);
static int    impl_watch  (struct watcher *);
static void * worker_loop (void *);
// Forward declaration end

static int
impl_init (
    struct watcher *w
) {
    if (w->flags & WATCHER_FALLBACK) {
        return 0;
    }

#if defined(WATCHER_IMPL_FANOTIFY)
    int fanfd = fanotify_init(FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_FID, 0);
    if (fanfd < 0) {
        log_printf("fanotify_init(): %s", xstrerror(errno));
        return -1;
    }
    w->fanfd = fanfd;

#elif defined(WATCHER_IMPL_KQUEUE)
    int kqfd = kqueuex(KQUEUE_CLOEXEC);
    if (kqfd < 0) {
        log_printf("kqueuex(): %s", xstrerror(errno));
        return -1;
    }
    struct kevent ev = {
        .ident  = w->pipefd[0],
        .filter = EVFILT_READ,
        .flags  = EV_ADD,
    };
    if (0 != kevent(kqfd, &ev, 1, NULL, 0, NULL)) {
        close(kqfd);
        log_printf("kevent(): %s", xstrerror(errno));
        return -1;
    }
    w->kqfd = kqfd;
    w->wfd  = -1;

#endif
    return 0;
}

static void
impl_free (
    struct watcher *w
) {
    if (w->flags & WATCHER_FALLBACK) {
        return;
    }

#if defined(WATCHER_IMPL_FANOTIFY)
    close(w->fanfd);

#elif defined(WATCHER_IMPL_KQUEUE)
    close(w->kqfd);
    if (w->wfd >= 0) {
        close(w->wfd);
    }

#endif
}

static int
impl_rearm (
    struct watcher *w
) {
    if (w->flags & WATCHER_FALLBACK) {
        goto fallback;
    }

#if defined(WATCHER_IMPL_FANOTIFY)
    uint32_t mask = FAN_DELETE_SELF | FAN_MOVE_SELF | FAN_MODIFY;
    if (0 != fanotify_mark(w->fanfd, FAN_MARK_ADD, mask, w->dirfd, w->name)) {
        log_printf("fanotify_mark(): %s", xstrerror(errno));
        return -1;
    }

#elif defined(WATCHER_IMPL_KQUEUE)
    int wfd = openat(w->dirfd, w->name,
            O_RDONLY | O_CLOEXEC | O_PATH | O_RESOLVE_BENEATH);
    if (wfd < 0) {
        log_printf("openat(): %s", xstrerror(errno));
        return -1;
    }
    struct kevent ev = {
        .ident  = wfd,
        .filter = EVFILT_VNODE,
        .flags  = EV_ADD | EV_ONESHOT,
        .fflags = NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME | NOTE_WRITE,
    };
    if (0 != kevent(w->kqfd, &ev, 1, NULL, 0, NULL)) {
        close(wfd);
        log_printf("kevent(): %s", xstrerror(errno));
        return -1;
    }
    w->wfd = wfd;

#endif
    return 0;

  fallback:
    if (0 != fstatat(w->dirfd, w->name, &w->old_stat, 0)) {
        log_printf("fstatat(): %s", xstrerror(errno));
        return -1;
    }
    return 0;
}

static int
impl_watch (
    struct watcher *w
) {
    if (w->flags & WATCHER_FALLBACK) {
        goto fallback;
    }
    int nevs;

#if defined(WATCHER_IMPL_FANOTIFY)
    struct pollfd pfds[] = {
        { .fd = w->fanfd,     .events = POLLIN },
        { .fd = w->pipefd[0], .events = POLLIN },
    };
    nevs = poll(pfds, 2, -1);
    if (unlikely(nevs < 0)) {
        log_printf("poll(): %s", xstrerror(errno));
        return -1;
    }
    if (pfds[1].revents & POLLHUP) {
        // watcher_destroy() was called
        return -1;
    }

    char tmp_buf[4096];
    ssize_t len = read(w->fanfd, &tmp_buf, sizeof(tmp_buf));
    if (unlikely(len < 0)) {
        log_printf("read(): %s", xstrerror(errno));
        return -1;
    }

    unsigned flags = FAN_MARK_REMOVE;
    uint32_t mask  = FAN_DELETE_SELF | FAN_MOVE_SELF | FAN_MODIFY;
    if (0 != fanotify_mark(w->fanfd, flags, mask, w->dirfd, w->name)) {
        // If watched file is deleted, the mark is automatically removed.
        if (errno != ENOENT) {
            log_printf("fanotify_mark(): %s", xstrerror(errno));
            return -1;
        }
    }

    // Drain fanotify event queue.  There may exist more than one event.
    do {
        len = read(w->fanfd, &tmp_buf, sizeof(tmp_buf));
    } while (len > 0);

#elif defined(WATCHER_IMPL_KQUEUE)
    struct kevent ev;
    nevs = kevent(w->kqfd, NULL, 0, &ev, 1, NULL);
    if (unlikely(nevs < 0)) {
        log_printf("kevent(): %s", xstrerror(errno));
        return -1;
    }
    if (unlikely(ev.flags & EV_ERROR)) {
        log_printf("kevent(): %s", xstrerror(ev.data));
        return -1;
    }
    if ((int)ev.ident == w->pipefd[0]) {
        debug_assert(ev.flags & EV_EOF);
        return -1;
    }
    close(w->wfd);
    w->wfd = -1;

#endif
    goto end;

  fallback:
    for (struct stat *old_stat = &w->old_stat; ; ) {
        struct pollfd pfd = {
            .fd     = w->pipefd[0],
            .events = POLLIN,
        };
        nevs = poll(&pfd, 1, WATCHER_FALLBACK_POLL_INTERVAL);
        if (nevs != 0) {
            if (unlikely(nevs < 0)) {
                log_printf("poll(): %s", xstrerror(errno));
            } else {
                debug_assert(pfd.revents & POLLHUP);
            }
            return -1;
        }

        struct stat new_stat;
        if (0 != fstatat(w->dirfd, w->name, &new_stat, 0)) {
            debug_printf("fstatat(): %s", xstrerror(errno));
            break;
        }
        if (new_stat.st_ino != old_stat->st_ino
                || new_stat.st_mtim.tv_sec  != old_stat->st_mtim.tv_sec
                || new_stat.st_mtim.tv_nsec != old_stat->st_mtim.tv_nsec
        ) {
            break;
        }
    }

  end:
    debug_printf("file %s changed", w->name);
    return 0;
}

static void *
worker_loop (
    void *user_data
) {
    struct watcher *w = user_data;

    pthread_mutex_lock(&w->mutex);

#ifdef BOOKMARKFS_SANDBOX
    uint32_t sandbox_flags = w->flags >> WATCHER_SANDBOX_FLAGS_OFFSET;
    if (!(sandbox_flags & SANDBOX_NOOP)) {
        sandbox_flags |= SANDBOX_READONLY;
        if (unlikely(0 != sandbox_enter(w->dirfd, sandbox_flags))) {
            goto end;
        }
        debug_puts("worker thread enters sandbox");
    }
#endif  /* defined(BOOKMARKFS_SANDBOX) */

    if (0 != impl_rearm(w)) {
        goto end;
    }
    debug_puts("worker ready");
    while (0 == impl_watch(w)) {
        w->flags |= WATCHER_IDLE;
        pthread_cond_wait(&w->cond, &w->mutex);
    }

  end:
    w->flags |= (WATCHER_DEAD | WATCHER_IDLE);
    pthread_mutex_unlock(&w->mutex);
    debug_puts("worker stops");
    return NULL;
}

struct watcher *
watcher_create (
    int         dirfd,
    char const *name,
    uint32_t    flags
) {
    struct watcher *w = xmalloc(sizeof(*w));
    *w = (struct watcher) {
        .dirfd     = dirfd,
        .name      = name,
        .pipefd[1] = -1,
        .flags     = flags,
    };
    if (flags & WATCHER_NOOP) {
        return w;
    }

    // The write end of the pipe must have the close-on-exec flag enabled,
    // since the calling process may fork-exec, so that poll()-ing the read end
    // may not POLLHUP when the write end closes.
    if (unlikely(0 != xpipe2(w->pipefd, O_CLOEXEC))) {
        goto free_watcher;
    }
    if (unlikely(0 != impl_init(w))) {
        goto close_pipes;
    }
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);

    int status = pthread_create(&w->worker, NULL, worker_loop, w);
    if (unlikely(status != 0)) {
        log_printf("pthread_create(): %s", xstrerror(-status));
        goto destroy_impl;
    }

    return w;

  destroy_impl:
    impl_free(w);
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);

  close_pipes:
    close(w->pipefd[0]);
    close(w->pipefd[1]);

  free_watcher:
    free(w);
    return NULL;
}

void
watcher_destroy (
    struct watcher *w
) {
    if (w == NULL) {
        return;
    }
    if (w->pipefd[1] < 0) {
        // WATCHER_NOOP
        goto end;
    }
    close(w->pipefd[1]);

    pthread_mutex_lock(&w->mutex);
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->worker, NULL);

    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->mutex);
    impl_free(w);
    close(w->pipefd[0]);

  end:
    free(w);
}

int
watcher_poll (
    struct watcher *w
) {
    int status = WATCHER_POLL_NOCHANGE;
    if (w->pipefd[1] < 0) {
        // WATCHER_NOOP
        return status;
    }
    if (0 != pthread_mutex_trylock(&w->mutex)) {
        return status;
    }
    if (unlikely(!(w->flags & WATCHER_IDLE))) {
        // The worker is just being initialized, or yet to return from
        // pthread_cond_wait() after a previous pthread_cond_signal().
        debug_puts("worker is drowsy...");
        goto end;
    }

    status = WATCHER_POLL_ERR;
    if (unlikely(w->flags & WATCHER_DEAD)) {
        log_puts("worker is dead");
        goto end;
    }
    if (0 != impl_rearm(w)) {
        goto end;
    }

    status = WATCHER_POLL_CHANGED;
    w->flags &= ~WATCHER_IDLE;
    pthread_cond_signal(&w->cond);

  end:
    pthread_mutex_unlock(&w->mutex);
    return status;
}
