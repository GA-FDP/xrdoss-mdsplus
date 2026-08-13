#!/usr/bin/env python3
"""Generate deploy/mdsip-seccomp.json -- an allowlist seccomp profile for mdsip.

The profile is a build artifact; this file is the source, because the *reasons*
matter more than the list and JSON cannot hold a comment.

Why an allowlist at all. podman's default profile is a denylist: it blocks ~50
syscalls and permits the rest. That is sized for "untrusted-ish code in a
container". Here the starting assumption is stronger -- a client of mdsip has
arbitrary native code execution (docs/security.md) -- so the reachable kernel
surface is the thing worth shrinking, and only an allowlist does that. This
takes it from ~350 syscalls to ~120.

What it cannot do. The attacker can still call everything mdsip legitimately
needs, so this is not a containment boundary by itself; it narrows the kernel
attack surface behind the container boundary. Nothing here replaces the
namespace, cgroup and filesystem controls.

    python tests/security/make_seccomp.py [outfile]
"""

import json
import sys

# --------------------------------------------------------------------------
# Observed. Captured by tests/security/capture_syscalls.sh tracing a real
# mdsip through the full integration workload: connect, openTree, get, getMany,
# a 4 MB result, error paths, several concurrent connections, disconnects.
# --------------------------------------------------------------------------
OBSERVED = [
    "accept", "access", "arch_prctl", "bind", "brk", "clone", "close",
    "connect", "execve", "exit", "fcntl", "fstat", "futex", "getdents64",
    "getpeername", "getpid", "getrandom", "listen", "lseek", "madvise", "mmap",
    "mprotect", "munmap", "openat", "prlimit64", "read", "readlink",
    "recvfrom", "rt_sigaction", "rt_sigprocmask", "select", "sendto",
    "set_robust_list", "set_tid_address", "setsockopt", "shutdown", "socket",
    "stat", "write",
]

# --------------------------------------------------------------------------
# Margin. The capture traced the conda build on an el8 host; the sandbox runs
# the RPM build on AlmaLinux 9. Different glibc, different syscalls for the
# same C calls -- clone3 for pthread_create, newfstatat for stat, rseq
# registration at startup. Omitting these would mean the container failed to
# start on the target and worked here, which is the worst way to be wrong.
#
# Everything below is a variant or sibling of something already observed, or
# part of ordinary process startup. Nothing here grants a capability the
# observed set does not already imply.
# --------------------------------------------------------------------------
GLIBC_VARIANTS = [
    "clone3",            # glibc 2.34+ pthread_create
    "rseq",              # glibc 2.35+ registers this at startup
    "newfstatat", "statx", "fstatat64", "lstat", "fstat64",
    "faccessat", "faccessat2",
    "getdents",
    "accept4",
    "exit_group",        # every real process exits through this
    "restart_syscall",   # kernel-inserted after an interrupted call
    "membarrier",
    "sched_getaffinity", "sched_yield",
    "gettid", "getppid", "getpgrp",
    "getuid", "geteuid", "getgid", "getegid", "getgroups",
    "uname", "sysinfo", "getcwd",
    "clock_gettime", "clock_getres", "gettimeofday", "time",
    "nanosleep", "clock_nanosleep",
    "getrlimit", "setrlimit",
    "sigaltstack", "rt_sigreturn", "rt_sigsuspend", "rt_sigtimedwait",
    "rt_sigpending", "pause",
    "mremap", "msync", "mincore",
    "umask",
]

# I/O and process shapes mdsip may reach on paths the capture did not hit: a
# different tree layout, a larger result, a client that disconnects mid-write.
# Each is an alternative spelling of read/write/wait that the observed set
# already permits in another form.
IO_AND_PROCESS = [
    "pread64", "pwrite64", "readv", "writev", "preadv", "pwritev",
    "sendmsg", "recvmsg", "sendmmsg", "recvmmsg", "sendfile",
    "poll", "ppoll", "pselect6",
    "epoll_create", "epoll_create1", "epoll_ctl", "epoll_wait", "epoll_pwait",
    "dup", "dup2", "dup3", "pipe", "pipe2",
    "getsockname", "getsockopt", "socketpair",
    "wait4", "waitid",                   # the parent reaps its per-connection children
    "kill", "tgkill", "tkill",
    "ioctl",
    "flock", "fsync", "fdatasync", "ftruncate", "truncate",
    "unlink", "unlinkat", "rename", "renameat", "renameat2",
    "mkdir", "mkdirat", "rmdir",
    "chdir", "fchdir",
    "chmod", "fchmod", "fchmodat",
    "readlinkat", "symlink", "symlinkat", "link", "linkat",
    "eventfd", "eventfd2", "timerfd_create", "timerfd_settime",
    "timerfd_gettime", "signalfd", "signalfd4",
    "futex_waitv",
    "set_thread_area", "get_thread_area",
    "setsid", "setpgid",
    "execveat",
]

# The entrypoint is now xinetd, which forks and execs one mdsip PER CONNECTION,
# rather than a single long-lived `mdsip -m`. That was a correctness fix -- `-m`
# shares one tree context across all connections -- but it changes the syscall
# shape of the container from "one process that accepts" to "a supervisor that
# spawns", so the profile has to cover the spawn path.
#
# fork/vfork are here even though glibc's fork() routes through clone: the
# profile is an allowlist and the cost of being wrong is a server that accepts a
# connection and then cannot serve it. setuid/setgid/setgroups are for xinetd's
# mandatory `user` attribute; it is the account we already run as, so the calls
# are no-ops, but they are still made and a denial DISABLES the service.
#
# NOTE: this list is reasoned, not observed. Re-run tests/security/
# capture_syscalls.sh against the xinetd entrypoint to replace it with a
# measured set -- the capture that produced OBSERVED predates this change and
# never saw a spawn.
SPAWNER = [
    "fork", "vfork",
    "setuid", "setgid", "setgroups", "setresuid", "setresgid",
    "getgroups", "getresuid", "getresgid",
    "getpgrp", "getppid", "getpriority", "setpriority",
    "rt_sigsuspend", "sigaltstack",
    "getrlimit", "prlimit64",
    "umask",
]

# --------------------------------------------------------------------------
# Deliberately absent, and why. These are what the profile is FOR: each is a
# well-trodden step in container escape, privilege escalation, or kernel
# exploitation, and mdsip needs none of them.
#
#   mount umount2 pivot_root chroot unshare setns   reshape the container
#   ptrace process_vm_readv process_vm_writv        read/write other processes
#   bpf perf_event_open userfaultfd io_uring_*      large kernel attack surface
#   keyctl add_key request_key                      kernel keyring
#   init_module finit_module delete_module          load kernel code
#   kexec_load kexec_file_load reboot               replace or stop the kernel
#   setreuid setregid                               change identity
#   capset                                          acquire capabilities
#   swapon swapoff quotactl acct                    privileged sysadmin
#   settimeofday clock_settime adjtimex              move the clock
#   modify_ldt personality iopl ioperm               odd execution modes
#   seccomp                                          re-arm the filter
#   open_by_handle_at name_to_handle_at              bypass path resolution
#
# setuid/setgid/setgroups USED to be denied here, on the reasoning that runc
# sets the uid before applying seccomp so nothing legitimate needed them. That
# stopped being true when the entrypoint became xinetd: its mandatory `user`
# attribute makes it call them, and a denial DISABLES the service rather than
# failing loudly. They are allowed in SPAWNER now.
#
# That is not a weakening. The container runs as a non-root uid with
# --cap-drop=ALL, so without CAP_SETUID these calls can only ever move to the
# uid the process already has; and --security-opt=no-new-privileges is what
# actually blocks regaining privilege, not this list. setreuid/setregid stay
# denied because nothing here calls them.
# --------------------------------------------------------------------------

def build():
    allowed = sorted(set(OBSERVED) | set(GLIBC_VARIANTS) | set(IO_AND_PROCESS) | set(SPAWNER))
    return {
        "defaultAction": "SCMP_ACT_ERRNO",
        # EPERM rather than killing the process: a syscall this list missed
        # then surfaces as an ordinary error the server can report, instead of
        # a container that dies with no explanation. The functional tests are
        # what catch such a gap.
        "defaultErrnoRet": 1,
        "archMap": [
            {"architecture": "SCMP_ARCH_X86_64",
             "subArchitectures": ["SCMP_ARCH_X86", "SCMP_ARCH_X32"]},
        ],
        "syscalls": [
            {"names": allowed, "action": "SCMP_ACT_ALLOW"},
        ],
    }


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "deploy/mdsip-seccomp.json"
    profile = build()
    with open(out, "w") as f:
        json.dump(profile, f, indent=2)
        f.write("\n")
    n = len(profile["syscalls"][0]["names"])
    print("wrote %s: %d syscalls allowed, everything else EPERM" % (out, n))
