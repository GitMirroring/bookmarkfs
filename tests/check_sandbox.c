/**
 * bookmarkfs/tests/check_sandbox.c
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <stdbool.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check_lib.h"
#include "frontend_util.h"
#include "sandbox.h"

// Forward declaration start
static int do_check_sandbox (int, uint32_t);
// Forward declaration end

static int
do_check_sandbox (
    int      dirfd,
    uint32_t flags
) {
#define FILE1_NAME  "false.sh"
#define FILE2_NAME  "foo.tmp"

#if defined(__linux__)
#  define ERR1  EACCES
#  define ERR2  EPERM
#elif defined(__FreeBSD__)
#  define ERR1  ENOTCAPABLE
#  define ERR2  ECAPMODE
#else
#  error "not implemented"
#endif

#define ASSERT_BAD_SYS(expr, cleanup_action)             \
    ASSERT_EXPR_INT(expr, r_, (err_ = errno, r_ < 0), {  \
        cleanup_action                                   \
        goto end;                                        \
    });                                                  \
    ASSERT_EXPR_INT(err_, r_, r_ == ERR1 || r_ == ERR2, goto end;)

#define ASSERT_BAD_FD(expr)   ASSERT_BAD_SYS(expr, close(r_);)
#define ASSERT_EQ(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) == r_, goto end;)
#define ASSERT_NE(val, expr)  ASSERT_EXPR_INT(expr, r_, (val) != r_, goto end;)

    int err_;
    int status = -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
#if defined(__linux__)
    ASSERT_EQ(-1, fd);
#elif defined(__FreeBSD__)
    // In capability mode, socket() is allowed,
    // but bind(), connect(), etc., are not.
    ASSERT_NE(-1, fd);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    ASSERT_BAD_SYS(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), );
    close(fd);
#else
#  error "not implemented"
#endif
    fd = -1;

    // Not allowed to perform filesystem lookups with AT_FDCWD.
    ASSERT_BAD_FD(openat(AT_FDCWD, ".", O_RDONLY));
    struct stat buf;
    ASSERT_BAD_SYS(fstatat(AT_FDCWD, ".", &buf, 0), );

    bool check_above        = true;
    bool check_lookup_above = true;
#ifdef __linux__
#  ifndef BOOKMARKFS_SANDBOX_LANDLOCK
    // If only we could filter renameat2() with seccomp...
    check_above = false;
#  endif
    // LANDLOCK_RULE_PATH_BENEATH does not prohibit lookup
    // above the given directory.
    // Unfiltered syscalls (e.g., fstatat()) are still able to
    // operate on the entire filesystem tree.
    // This is not desired, but generally harmless.
    check_lookup_above = false;
#endif
    if (check_above) {
        // Not allowed to operate above the directory.
        ASSERT_BAD_FD(openat(dirfd, "..", O_RDONLY));
        if (check_lookup_above) {
            ASSERT_BAD_SYS(fstatat(dirfd, "..", &buf, 0), );
        }
    }

    fd = openat(dirfd, FILE1_NAME, O_RDONLY);
    ASSERT_NE(-1, fd);

    // Not allowed to execute files.
    char *argv[] = { FILE2_NAME, NULL };
    char *envp[] = { NULL };
    ASSERT_BAD_SYS(fexecve(fd, argv, envp), );

    if (flags & SANDBOX_READONLY) {
        // Not allowed to create, modify or delete files in read-only mode.
        ASSERT_BAD_FD(openat(dirfd, FILE2_NAME, O_RDONLY | O_CREAT, 0600));
        ASSERT_BAD_SYS(renameat(dirfd, FILE1_NAME, dirfd, FILE2_NAME), );
        ASSERT_BAD_SYS(unlinkat(dirfd, FILE1_NAME, 0), );
    } else {
        close(fd);
        fd = openat(dirfd, FILE2_NAME, O_RDONLY | O_CREAT, 0600);
        ASSERT_NE(-1, fd);

        ASSERT_EQ(0, renameat(dirfd, FILE1_NAME, dirfd, FILE2_NAME));
        ASSERT_EQ(0, unlinkat(dirfd, FILE2_NAME, 0));
    }

    status = 0;

  end:
    if (fd >= 0) {
        close(fd);
    }
    return status;
}

int
check_sandbox (
    int   argc,
    char *argv[]
) {
    uint32_t flags = 0;
#ifndef BOOKMARKFS_SANDBOX_LANDLOCK
    flags |= SANDBOX_NO_LANDLOCK;
#endif
    char const *path = NULL;

    OPT_START(argc, argv, "d:r")
    OPT_OPT('d') {
        path = optarg;
        break;
    }
    OPT_OPT('r') {
        flags |= SANDBOX_READONLY;
        break;
    }
    OPT_END

    if (path == NULL) {
        log_puts("path not specified");
        return -1;
    }

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        log_printf("failed to open '%s'", path);
        return -1;
    }
    int status = sandbox_enter(dirfd, flags);
    if (status < 0) {
        goto end;
    }
    status = do_check_sandbox(dirfd, flags);

  end:
    close(dirfd);
    return status;
}
