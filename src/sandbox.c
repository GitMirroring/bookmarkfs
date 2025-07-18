/**
 * bookmarkfs/src/sandbox.c
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

#include "sandbox.h"

#include <errno.h>

#include <fcntl.h>
#include <unistd.h>

#if !defined(ENABLE_SANDBOX)
#elif defined(__linux__)
#  ifdef ENABLE_SANDBOX_LANDLOCK
#    include <linux/landlock.h>
#    include <sys/syscall.h>
#  endif
#  include <sys/mman.h>
#  include <sys/prctl.h>
#  include <seccomp.h>
#  define SANDBOX_IMPL_SECCOMP_LANDLOCK
#elif defined(__FreeBSD__)
#  include <sys/capsicum.h>
#  include <sys/procctl.h>
#  define SANDBOX_IMPL_CAPSICUM
#else
#  error "sandbox not implemented on this platform"
#endif

#include "macros.h"
#include "xstd.h"

#ifdef SANDBOX_IMPL_SECCOMP_LANDLOCK

#define ARG_CMP_(...)  (struct scmp_arg_cmp []) { __VA_ARGS__ }
#define SCMP_RULE(sys, prio, ...)       \
    {                                   \
        .syscall_num = SCMP_SYS(sys),   \
        .priority    = prio,            \
        .argc = sizeof(ARG_CMP_(__VA_ARGS__)) / sizeof(struct scmp_arg_cmp),  \
        .argv = ARG_CMP_(__VA_ARGS__),  \
    }
#define SCMP_RULE_NOARG(sys, prio)     \
    {                                  \
        .syscall_num = SCMP_SYS(sys),  \
        .priority    = prio,           \
    }
#define SCMP_RULES(rules)  rules, sizeof(rules) / sizeof(struct scmp_rule_def)

/**
 * Whether the argument is a valid fd (non-negative 32-bit integer).
 * Intended to filter out *at() calls with `AT_FDCWD`.
 *
 * A saner alternative is just `SCMP_Ax_32(SCMP_CMP_NE, AT_FDCWD)`,
 * however, we have to workaround a glibc problem introduced in commit
 * 89b53077d2a5, where syscall arguments passed to `SYSCALL_CANCEL()`
 * are always explicitly casted to `long`, sign-extending the value
 * (instead of zero-extending, as we expected).
 *
 * The only affected syscall here is openat().
 */
#define SCMP_ARG_FD(n)  SCMP_A##n##_32(SCMP_CMP_MASKED_EQ, (1u << 31), 0)

struct scmp_rule_def {
    int                        syscall_num;
    int                        argc;
    struct scmp_arg_cmp const *argv;
    uint8_t                    priority;
};

// Forward declaration start
static int add_scmp_rules (scmp_filter_ctx, struct scmp_rule_def const *,
                           size_t);

#ifdef ENABLE_SANDBOX_LANDLOCK
static int landlock_create_ruleset (struct landlock_ruleset_attr const *,
                                    size_t, uint32_t);
static int landlock_add_rule       (int, enum landlock_rule_type,
                                    void const *, uint32_t);
static int landlock_restrict_self  (int, uint32_t);
#endif  /* defined(ENABLE_SANDBOX_LANDLOCK) */
// Forward declaration end

static int
add_scmp_rules (
    scmp_filter_ctx             sfctx,
    struct scmp_rule_def const *rules,
    size_t                      rules_cnt
) {
    for (size_t idx = 0; idx < rules_cnt; ++idx) {
        struct scmp_rule_def const *rule = rules + idx;

        int status = seccomp_rule_add_array(sfctx, SCMP_ACT_ALLOW,
                rule->syscall_num, rule->argc, rule->argv);
        if (status < 0) {
            log_printf("seccomp_rule_add(): %s", xstrerror(-status));
            return status;
        }
        status = seccomp_syscall_priority(sfctx, rule->syscall_num,
                rule->priority);
        if (status < 0) {
            log_printf("seccomp_syscall_priority(): %s", xstrerror(-status));
            return status;
        }
    }
    return 0;
}

#ifdef ENABLE_SANDBOX_LANDLOCK

static int
landlock_create_ruleset (
    struct landlock_ruleset_attr const *attr,
    size_t                              attr_size,
    uint32_t                            flags
) {
    return syscall(SYS_landlock_create_ruleset, attr, attr_size, flags);
}

static int
landlock_add_rule (
    int                      ruleset_fd,
    enum landlock_rule_type  rule_type,
    void const              *rule_attr,
    uint32_t                 flags
) {
    return syscall(SYS_landlock_add_rule, ruleset_fd, rule_type, rule_attr,
            flags);
}

static int
landlock_restrict_self (
    int      ruleset_fd,
    uint32_t flags
) {
    return syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}

#endif  /* defined(ENABLE_SANDBOX_LANDLOCK) */

int
sandbox_enter (
    int      dirfd,
    uint32_t flags
) {
    if (flags & SANDBOX_NOOP) {
        return 0;
    }

    if (unlikely(0 != prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))) {
        log_printf("prctl(): %s", xstrerror(errno));
        return -1;
    }

    scmp_filter_ctx sfctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    xassert(sfctx != NULL);

    int status = seccomp_attr_set(sfctx, SCMP_FLTATR_CTL_NNP, 0);
    if (unlikely(status < 0)) {
        log_printf("seccomp_attr_set(): %s", xstrerror(-status));
        goto free_sfctx;
    }
    status = -1;

    pid_t pid = getpid();
    struct scmp_rule_def const rules_common[] = {
        // exit
        SCMP_RULE_NOARG(exit,       0),
        SCMP_RULE_NOARG(exit_group, 0),

        // signals
        SCMP_RULE_NOARG(sigaction,       10),
#ifdef ENABLE_BOOKMARKFS_DEBUG
        // Make ASAN happy...
        SCMP_RULE_NOARG(sigaltstack,     0),
#endif
        SCMP_RULE_NOARG(sigprocmask,     10),
        SCMP_RULE_NOARG(sigreturn,       10),
        SCMP_RULE_NOARG(restart_syscall, 10),
        SCMP_RULE_NOARG(rt_sigaction,    10),
        SCMP_RULE_NOARG(rt_sigprocmask,  10),
        SCMP_RULE_NOARG(rt_sigreturn,    10),
        SCMP_RULE(kill,   0, SCMP_A0_32(SCMP_CMP_EQ, pid)),
        SCMP_RULE(tgkill, 0, SCMP_A0_32(SCMP_CMP_EQ, pid)),

        // read/write
        SCMP_RULE_NOARG(lseek,    190),
        SCMP_RULE_NOARG(pread64,  190),
        SCMP_RULE_NOARG(pwrite64, 190),
        SCMP_RULE_NOARG(read,     200),
        SCMP_RULE_NOARG(write,    200),
        SCMP_RULE_NOARG(writev,   200),

        // other file operations
        SCMP_RULE_NOARG(close,      20),
        SCMP_RULE_NOARG(fallocate,  30),
        SCMP_RULE_NOARG(fcntl,      100),
        SCMP_RULE_NOARG(fdatasync,  30),
        SCMP_RULE_NOARG(flock,      20),
        SCMP_RULE_NOARG(fstat,      100),
        SCMP_RULE_NOARG(fstat64,    100),
        SCMP_RULE_NOARG(fsync,      30),
        SCMP_RULE_NOARG(ftruncate,  30),
        SCMP_RULE_NOARG(getdents64, 100),
        SCMP_RULE_NOARG(ioctl,      30),

        // memory management
        SCMP_RULE_NOARG(brk,     20),
        SCMP_RULE_NOARG(madvise, 30),
        SCMP_RULE(mmap,     30, SCMP_A2_32(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0)),
        SCMP_RULE(mmap2,    30, SCMP_A2_32(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0)),
        SCMP_RULE(mprotect, 30, SCMP_A2_32(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0)),
        SCMP_RULE_NOARG(mremap, 30),
        SCMP_RULE_NOARG(msync,  30),
        SCMP_RULE_NOARG(munmap, 30),

        // current process info
        SCMP_RULE_NOARG(geteuid,   10),
        SCMP_RULE_NOARG(geteuid32, 10),
        SCMP_RULE_NOARG(getegid,   10),
        SCMP_RULE_NOARG(getegid32, 10),
        SCMP_RULE_NOARG(getpid,    10),
        SCMP_RULE_NOARG(gettid,    10),

        // system info
        SCMP_RULE_NOARG(clock_gettime,   50),
        SCMP_RULE_NOARG(clock_gettime64, 50),

        // other utils
        SCMP_RULE_NOARG(futex,           40),
        SCMP_RULE_NOARG(getrandom,       40),
        SCMP_RULE_NOARG(poll,            40),
        SCMP_RULE_NOARG(pselect6,        40),
        SCMP_RULE_NOARG(pselect6_time64, 40),
    };
    if (unlikely(0 != add_scmp_rules(sfctx, SCMP_RULES(rules_common)))) {
        goto free_sfctx;
    }

    if (dirfd < 0) {
        goto apply_seccomp;
    }
    // libseccomp can optimize out `SCMP_CMP_MASKED_EQ(0, 0)`.
    int oflags_mask = 0;
    int oflags_exp  = 0;
    if ((flags & SANDBOX_NO_LANDLOCK) && (flags & SANDBOX_READONLY)) {
        oflags_mask = O_ACCMODE | O_CREAT;
        oflags_exp  = O_RDONLY;
    }
    struct scmp_rule_def const rules_dir[] = {
        SCMP_RULE(fanotify_mark, 40,  SCMP_ARG_FD(3)),
        SCMP_RULE(fstatat64,     100, SCMP_ARG_FD(0)),
        SCMP_RULE(newfstatat,    100, SCMP_ARG_FD(0)),
        SCMP_RULE(openat, 20, SCMP_ARG_FD(0),
                              SCMP_A2_32(SCMP_CMP_MASKED_EQ, oflags_mask,
                                                             oflags_exp)),
    };
    if (unlikely(0 != add_scmp_rules(sfctx, SCMP_RULES(rules_dir)))) {
        goto free_sfctx;
    }

    if (flags & SANDBOX_READONLY) {
        goto do_landlock;
    }
    struct scmp_rule_def const rules_dir_extra[] = {
        SCMP_RULE(renameat, 20, SCMP_ARG_FD(0),
                                SCMP_ARG_FD(2)),
        SCMP_RULE(unlinkat, 20, SCMP_ARG_FD(0)),
    };
    if (unlikely(0 != add_scmp_rules(sfctx, SCMP_RULES(rules_dir_extra)))) {
        goto free_sfctx;
    }

  do_landlock:
    if (flags & SANDBOX_NO_LANDLOCK) {
        goto apply_seccomp;
    }
    status = -1;
#ifdef ENABLE_SANDBOX_LANDLOCK
    int ruleset_version = landlock_create_ruleset(NULL, 0,
            LANDLOCK_CREATE_RULESET_VERSION);
    if (unlikely(ruleset_version < 0)) {
        log_printf("landlock_create_ruleset(): %s", xstrerror(errno));
        goto free_sfctx;
    }

#ifndef LANDLOCK_ACCESS_FS_REFER  // Available since Linux 5.19
#define LANDLOCK_ACCESS_FS_REFER  ( UINT64_C(1) << 13 )
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE  // Available since Linux 6.2
#define LANDLOCK_ACCESS_FS_TRUNCATE  ( UINT64_C(1) << 14 )
#endif
#define LANDLOCK_FS_RIGHT_NAME_(name)  LANDLOCK_ACCESS_FS_##name
#define LANDLOCK_FS_RIGHT(...)  \
    BITWISE_OR(LANDLOCK_FS_RIGHT_NAME_, __VA_ARGS__)

    uint64_t handled_access = LANDLOCK_FS_RIGHT(EXECUTE) |
        LANDLOCK_FS_RIGHT(WRITE_FILE,  READ_FILE, READ_DIR,   REMOVE_DIR) |
        LANDLOCK_FS_RIGHT(REMOVE_FILE, MAKE_CHAR, MAKE_DIR,   MAKE_REG)   |
        LANDLOCK_FS_RIGHT(MAKE_SOCK,   MAKE_FIFO, MAKE_BLOCK, MAKE_SYM);
    switch (ruleset_version) {
      default:
      case 3:
        handled_access |= LANDLOCK_FS_RIGHT(TRUNCATE);
        // fallthrough
      case 2:
        handled_access |= LANDLOCK_FS_RIGHT(REFER);
        // fallthrough
      case 1:
        break;
    }
    struct landlock_ruleset_attr const ruleset_attr = {
        .handled_access_fs = handled_access,
    };
    int lrfd = landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
    if (unlikely(lrfd < 0)) {
        log_printf("landlock_create_ruleset(): %s", xstrerror(errno));
        goto free_sfctx;
    }

    uint64_t allowed_access = LANDLOCK_FS_RIGHT(READ_FILE, READ_DIR);
    if (!(flags & SANDBOX_READONLY)) {
        allowed_access |= LANDLOCK_FS_RIGHT(WRITE_FILE, REMOVE_FILE, MAKE_REG);
        if (ruleset_version >= 3) {
            allowed_access |= LANDLOCK_FS_RIGHT(TRUNCATE);
        }
    }
    enum landlock_rule_type rule_type = LANDLOCK_RULE_PATH_BENEATH;
    struct landlock_path_beneath_attr const rule_attr = {
        .allowed_access = allowed_access,
        .parent_fd      = dirfd,
    };
    if (unlikely(0 != landlock_add_rule(lrfd, rule_type, &rule_attr, 0))) {
        log_printf("landlock_add_rule(): %s", xstrerror(errno));
        goto free_ruleset;
    }

    if (unlikely(0 != landlock_restrict_self(lrfd, 0))) {
        log_printf("landlock_restrict_self(): %s", xstrerror(errno));
        goto free_ruleset;
    }
    status = 0;

  free_ruleset:
    close(lrfd);

#else
    log_puts("landlock is not supported on this build");
#endif  /* defined(ENABLE_SANDBOX_LANDLOCK) */

    if (status < 0) {
        goto free_sfctx;
    }

  apply_seccomp:
    status = seccomp_load(sfctx);
    if (unlikely(status != 0)) {
        log_printf("seccomp_load(): %s", xstrerror(-status));
    }

  free_sfctx:
    seccomp_release(sfctx);

    return status;
}

#endif  /* defined(SANDBOX_IMPL_SECCOMP_LANDLOCK) */

#if defined(SANDBOX_IMPL_CAPSICUM)

int
sandbox_enter (
    int      dirfd,
    uint32_t flags
) {
    if (flags & SANDBOX_NOOP) {
        return 0;
    }

    int val = PROC_NO_NEW_PRIVS_ENABLE;
    if (unlikely(0 != procctl(P_PID, getpid(), PROC_NO_NEW_PRIVS_CTL, &val))) {
        log_printf("procctl(): %s", xstrerror(errno));
        return -1;
    }

    if (unlikely(0 != cap_enter())) {
        log_printf("cap_enter(): %s", xstrerror(errno));
        return -1;
    }

    if (dirfd >= 0) {
        cap_rights_t rights;
        cap_rights_init(&rights, CAP_LOOKUP, CAP_READ, CAP_FSTAT, CAP_FLOCK,
                CAP_EVENT, CAP_IOCTL, CAP_MMAP_R);
        if (!(flags & SANDBOX_READONLY)) {
            cap_rights_set(&rights, CAP_CREATE, CAP_WRITE, CAP_FSYNC,
                    CAP_FTRUNCATE, CAP_RENAMEAT_SOURCE, CAP_RENAMEAT_TARGET,
                    CAP_UNLINKAT);
        }

        if (unlikely(0 != cap_rights_limit(dirfd, &rights))) {
            log_printf("cap_rights_limit(): %s", xstrerror(errno));
            return -1;
        }
    }

    return 0;
}

#endif  /* defined(SANDBOX_IMPL_CAPSICUM) */

#ifndef ENABLE_SANDBOX

int
sandbox_enter (
    int      UNUSED_VAR(dirfd),
    uint32_t flags
) {
    if (flags & SANDBOX_NOOP) {
        return 0;
    }

    log_puts("sandbox not implemented");
    return -1;
}

#endif  /* !defined(ENABLE_SANDBOX) */
