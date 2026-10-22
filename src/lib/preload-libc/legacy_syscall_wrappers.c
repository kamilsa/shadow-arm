/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

/// This file contains wrappers for libc functions whose traditional syscalls
/// don't exist on some architectures, such as aarch64, which only provide the
/// newer variants (for example `unlinkat` instead of `unlink`). The wrappers in
/// syscall_wrappers.c and libcall_wrappers.c are only compiled when the
/// traditional syscall exists, so without these wrappers the libc functions
/// would make their syscalls natively, which would then need to be trapped by
/// the much slower seccomp method.
///
/// Each wrapper is only compiled when the traditional syscall doesn't exist,
/// and makes the same syscall(s) as glibc's implementation of the function.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

// Converts the times for utimes() and futimesat() to the times for utimensat().
static inline const struct timespec* _timevals_to_timespecs(const struct timeval* tv,
                                                            struct timespec* ts) {
    if (tv == NULL) {
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        ts[i].tv_sec = tv[i].tv_sec;
        ts[i].tv_nsec = tv[i].tv_usec * 1000;
    }
    return ts;
}

// man 2 access
#if !defined(SYS_access) && defined(SYS_faccessat)
int access(const char* pathname, int mode) {
    return syscall(SYS_faccessat, AT_FDCWD, pathname, mode);
}
#endif

// man 2 chown
#if !defined(SYS_chown) && defined(SYS_fchownat)
int chown(const char* pathname, uid_t owner, gid_t group) {
    return syscall(SYS_fchownat, AT_FDCWD, pathname, owner, group, 0);
}
#endif

// man 2 lchown
#if !defined(SYS_lchown) && defined(SYS_fchownat)
int lchown(const char* pathname, uid_t owner, gid_t group) {
    return syscall(SYS_fchownat, AT_FDCWD, pathname, owner, group, AT_SYMLINK_NOFOLLOW);
}
#endif

// man 2 creat
#if !defined(SYS_creat) && defined(SYS_openat)
int creat(const char* pathname, mode_t mode) {
    return syscall(SYS_openat, AT_FDCWD, pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int creat64(const char* pathname, mode_t mode) { return creat(pathname, mode); }
#endif

// man 2 open
#if !defined(SYS_open) && defined(SYS_openat)
// `open` itself is implemented in libc_impls.c.
int open64(const char* pathname, int flags, ...) {
    va_list(args);
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);

    return open(pathname, flags, mode);
}
#endif

// man 2 dup2
#if !defined(SYS_dup2) && defined(SYS_dup3)
int dup2(int oldfd, int newfd) {
    // dup3 fails if the descriptors are equal, but dup2 returns newfd if oldfd is valid
    if (oldfd == newfd) {
        return syscall(SYS_fcntl, oldfd, F_GETFL) < 0 ? -1 : newfd;
    }
    return syscall(SYS_dup3, oldfd, newfd, 0);
}
#endif

// man 2 epoll_create
#if !defined(SYS_epoll_create) && defined(SYS_epoll_create1)
int epoll_create(int size) {
    // the size is ignored, but must be positive
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_epoll_create1, 0);
}
#endif

// man 2 epoll_wait
#if !defined(SYS_epoll_wait) && defined(SYS_epoll_pwait)
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    // the last argument is the size of the kernel's sigset_t, which is smaller than libc's
    return syscall(SYS_epoll_pwait, epfd, events, maxevents, timeout, NULL, _NSIG / 8);
}
#endif

// man 2 eventfd
#if !defined(SYS_eventfd) && defined(SYS_eventfd2)
int eventfd(unsigned int initval, int flags) { return syscall(SYS_eventfd2, initval, flags); }
#endif

// man 2 getpgrp
#if !defined(SYS_getpgrp) && defined(SYS_getpgid)
pid_t getpgrp(void) { return syscall(SYS_getpgid, 0); }
#endif

// man 2 inotify_init
#if !defined(SYS_inotify_init) && defined(SYS_inotify_init1)
int inotify_init(void) { return syscall(SYS_inotify_init1, 0); }
#endif

// man 2 link
#if !defined(SYS_link) && defined(SYS_linkat)
int link(const char* oldpath, const char* newpath) {
    return syscall(SYS_linkat, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}
#endif

// man 2 stat
#if !defined(SYS_stat) && defined(SYS_newfstatat)
int stat(const char* pathname, struct stat* statbuf) {
    return syscall(SYS_newfstatat, AT_FDCWD, pathname, statbuf, 0);
}
#endif

// man 2 lstat
#if !defined(SYS_lstat) && defined(SYS_newfstatat)
int lstat(const char* pathname, struct stat* statbuf) {
    return syscall(SYS_newfstatat, AT_FDCWD, pathname, statbuf, AT_SYMLINK_NOFOLLOW);
}
#endif

// man 2 mkdir
#if !defined(SYS_mkdir) && defined(SYS_mkdirat)
int mkdir(const char* pathname, mode_t mode) {
    return syscall(SYS_mkdirat, AT_FDCWD, pathname, mode);
}
#endif

// man 2 mknod
#if !defined(SYS_mknod) && defined(SYS_mknodat)
int mknod(const char* pathname, mode_t mode, dev_t dev) {
    // the kernel's dev_t is only 32 bits
    unsigned int kernel_dev = dev;
    if (kernel_dev != dev) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_mknodat, AT_FDCWD, pathname, mode, kernel_dev);
}
#endif

// man 2 pipe
#if !defined(SYS_pipe) && defined(SYS_pipe2)
int pipe(int pipefd[2]) { return syscall(SYS_pipe2, pipefd, 0); }
#endif

// man 2 readlink
#if !defined(SYS_readlink) && defined(SYS_readlinkat)
ssize_t readlink(const char* pathname, char* buf, size_t bufsiz) {
    return syscall(SYS_readlinkat, AT_FDCWD, pathname, buf, bufsiz);
}
#endif

// man 2 rename
#if !defined(SYS_rename) && (defined(SYS_renameat) || defined(SYS_renameat2))
int rename(const char* oldpath, const char* newpath) {
#ifdef SYS_renameat
    return syscall(SYS_renameat, AT_FDCWD, oldpath, AT_FDCWD, newpath);
#else
    return syscall(SYS_renameat2, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
#endif
}
#endif

// man 2 rmdir
#if !defined(SYS_rmdir) && defined(SYS_unlinkat)
int rmdir(const char* pathname) { return syscall(SYS_unlinkat, AT_FDCWD, pathname, AT_REMOVEDIR); }
#endif

// man 2 symlink
#if !defined(SYS_symlink) && defined(SYS_symlinkat)
int symlink(const char* target, const char* linkpath) {
    return syscall(SYS_symlinkat, target, AT_FDCWD, linkpath);
}
#endif

// man 2 unlink
#if !defined(SYS_unlink) && defined(SYS_unlinkat)
int unlink(const char* pathname) { return syscall(SYS_unlinkat, AT_FDCWD, pathname, 0); }
#endif

// man 2 utime
#if !defined(SYS_utime) && defined(SYS_utimensat)
int utime(const char* filename, const struct utimbuf* times) {
    struct timespec ts[2];
    if (times != NULL) {
        ts[0] = (struct timespec){.tv_sec = times->actime};
        ts[1] = (struct timespec){.tv_sec = times->modtime};
    }
    return syscall(SYS_utimensat, AT_FDCWD, filename, times != NULL ? ts : NULL, 0);
}
#endif

// man 2 utimes
#if !defined(SYS_utimes) && defined(SYS_utimensat)
int utimes(const char* filename, const struct timeval times[2]) {
    struct timespec ts[2];
    return syscall(SYS_utimensat, AT_FDCWD, filename, _timevals_to_timespecs(times, ts), 0);
}
#endif

// man 2 futimesat
#if !defined(SYS_futimesat) && defined(SYS_utimensat)
int futimesat(int dirfd, const char* pathname, const struct timeval times[2]) {
    struct timespec ts[2];
    return syscall(SYS_utimensat, dirfd, pathname, _timevals_to_timespecs(times, ts), 0);
}
#endif
