#!/usr/bin/env python3
"""Assert that the sandbox actually contains a client who has code execution.

`probe_capability.py` measures what a client can do. This asserts what it must
*not* be able to do, and fails loudly when a control is missing. The difference
matters: a sandbox nobody attacks is indistinguishable from no sandbox at all.

Every check runs through ordinary TDI -- the same channel a real client has --
so this tests the deployed configuration rather than a model of it.

    python verify_sandbox.py <host:port>
"""

import os
import sys

from MDSplus import Connection

TARGET = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8000"
conn = Connection(TARGET)

# Set when the operator has deliberately accepted a host that cannot enforce
# resource limits (cgroups v1 + rootless podman silently ignores them). It
# downgrades those checks to WAIVED so the rest of the suite stays a usable
# gate -- but they are still reported, because a waiver that disappears from
# the output is a waiver nobody remembers making.
WAIVE_LIMITS = bool(os.environ.get("MDSIP_ALLOW_NO_LIMITS"))

failures = []
waived = []
checks = 0


def shell(cmd):
    """Exit status of a shell command run inside the server.

    Commands must not contain double quotes: they are interpolated into a TDI
    string literal, whose escaping rules are not worth relying on here. Use
    single quotes, which pass through untouched.
    """
    assert '"' not in cmd, "use single quotes in probe commands: %r" % cmd
    return int(conn.get('spawn("%s")' % cmd))


def expect(name, ok, detail, why, waivable=False):
    global checks
    checks += 1
    if not ok and waivable and WAIVE_LIMITS:
        print("  %-34s WAIVED" % name)
        waived.append((name, detail, why))
        return
    print("  %-34s %s" % (name, "PASS" if ok else "*** FAIL ***"))
    if not ok:
        failures.append((name, detail, why))


def expect_shell_fails(name, cmd, why):
    """The command must NOT succeed inside the sandbox."""
    try:
        status = shell(cmd)
    except Exception:
        expect(name, True, "", why)          # TDI refused outright: also fine
        return
    expect(name, status != 0, "`%s` succeeded (status 0)" % cmd, why)


def expect_shell_works(name, cmd, why, waivable=False):
    """A control that must succeed -- or a service check that must not break."""
    try:
        status = shell(cmd)
    except Exception as exc:
        expect(name, False, "%s: %s" % (type(exc).__name__, exc), why, waivable)
        return
    expect(name, status == 0, "`%s` failed (status %d)" % (cmd, status), why,
           waivable)


print("Verifying the sandbox at %s" % TARGET)
print()
print("First, confirm the client really does have code execution --")
print("otherwise the rest of this proves nothing.")

# If this ever starts failing, do not celebrate: it means the probe broke, not
# that MDSplus became safe. The controls below are the actual defence.
try:
    uid = int(conn.get("MdsShr->getuid()"))
    print("  code execution confirmed             uid=%d" % uid)
except Exception as exc:
    print("  *** could not confirm code execution: %s ***" % exc)
    print("  The checks below may be passing for the wrong reason.")
    uid = -1

print()
print("Containment:")

# --- identity -----------------------------------------------------------
expect("not running as root", uid != 0,
       "mdsip is running as uid 0",
       "code execution as root inside the namespace is a much shorter path out")

# In rootless podman, container uid 0 maps to the *invoking user*. Run the
# sandbox as the same account that runs the Pelican origin and becoming
# container-root is becoming the owner of issuer.jwk -- one mapping away,
# blocked only by cap-drop/no-new-privileges holding. Run it as a dedicated
# service account and that path does not exist to be attacked.
#
# The server's own uid is already distinct (container 5000 maps high into the
# subuid range); this checks the escalation target, which is the one that
# matters.
ORIGIN_UID = os.environ.get("MDSIP_ORIGIN_UID", "")
if ORIGIN_UID:
    expect_shell_fails(
        "container root is not the origin uid",
        "grep -qE '^[[:space:]]*0[[:space:]]+%s[[:space:]]' /proc/self/uid_map"
        % ORIGIN_UID,
        "container-root maps to uid %s, which owns the origin's issuer keys; "
        "run the sandbox as a dedicated service account" % ORIGIN_UID)
else:
    print("  %-34s SKIP (set MDSIP_ORIGIN_UID)" % "container root vs origin uid")
    print("    unchecked: whether container-root maps to the origin's user")

# --- the crown jewels ---------------------------------------------------
# A stolen issuer key mints arbitrary federation tokens. Nothing else in this
# file matters as much as these two.
expect_shell_fails("no origin config visible", "test -e /etc/pelican",
                   "the origin's issuer keys must not share a filesystem with mdsip")
# `find -type f`, not `ls`: podman mounts the host's subscription credentials at
# /run/secrets by default, and the sandbox masks that with an empty tmpfs -- so
# the directory still exists and only its emptiness matters. An `ls` of several
# paths prints a `/run/secrets:` header for the directory itself, which reads as
# content when it is not.
expect_shell_fails("no credential files reachable",
                   "find /run/secrets /etc/pelican /root -type f 2>/dev/null | grep -q .",
                   "any credential reachable here is a credential a client has")
expect_shell_fails("no xrootd config visible", "test -e /etc/xrootd",
                   "the origin's configuration names its keys and its peers")

# --- egress -------------------------------------------------------------
# Without this, code execution is a pivot into the internal network and an
# exfiltration channel out of it.
expect_shell_fails("no DNS resolution", "getent hosts example.com",
                   "name resolution is the first step of most exfiltration")
expect_shell_fails("no outbound TCP",
                   "timeout 3 bash -c '</dev/tcp/1.1.1.1/53'",
                   "an internal network with no route off the host")

# Backend-independent, and the reason the previous version of this check was
# worthless: it aimed at 10.0.2.2, the slirp4netns host address, which does not
# exist on a bridge network -- so it passed without testing anything. A default
# route is what actually makes egress possible, so assert its absence directly.
#
# grep -x on the printed field, not an awk numeric compare: awk converts the
# subnet route's destination "0A890000" to 0, which would false-match a
# `$2 == 00000000` test and report a default route that is not there.
expect_shell_fails("no default route",
                   "awk '{print $2}' /proc/net/route | grep -qx 00000000",
                   "an internal network must have no route off the host; if one "
                   "appears, the egress checks above are passing on luck")

# --- filesystem ---------------------------------------------------------
expect_shell_fails("root filesystem read-only", "touch /probe-root",
                   "a writable root means persistence across the next connection")
expect_shell_fails("trees read-only", "touch /trees/probe-tree",
                   "read-only is the stated data policy; it must be enforced, not assumed")
expect_shell_fails("cannot write MDSplus install", "touch /usr/local/mdsplus/probe",
                   "a writable install means replacing the server binary itself")
expect_shell_fails("/tmp is noexec", "cp /bin/true /tmp/x && /tmp/x",
                   "the one writable path must not be a place to stage a binary")

# --- privilege ----------------------------------------------------------
# Note the direction: an all-zero CapEff is the *desired* state, so this is a
# must-succeed check rather than a must-fail one.
expect_shell_works("no effective capabilities",
                   "grep CapEff /proc/self/status | grep -q 0000000000000000",
                   "CAP_* are the difference between contained and not")
expect_shell_fails("no setuid binaries to escalate with",
                   "find /usr /bin /sbin -perm -4000 -type f 2>/dev/null | grep -q .",
                   "no-new-privileges blocks the escalation; removing the targets is belt and braces")
# Tests the property, not the entrypoint's name. This used to assert
# `cat /proc/1/comm = mdsip`, which broke the moment the entrypoint became a
# per-connection spawner -- PID 1 is socat now -- while the isolation it was
# meant to check was never affected. The host has ~1400 processes; inside its
# own namespace the sandbox sees about 5, and every extra one is a live
# connection.
expect_shell_works("own pid namespace",
                   "test $(ls -d /proc/[0-9]* 2>/dev/null | wc -l) -lt 50",
                   "sharing the host pid namespace would expose every other process")

# --- resources ----------------------------------------------------------
expect_shell_works("pids are limited",
                   "test -r /sys/fs/cgroup/pids.max && "
                   "test $(cat /sys/fs/cgroup/pids.max) != max",
                   "unbounded forks are a trivial denial of service",
                   waivable=True)
expect_shell_works("memory is limited",
                   "test $(cat /sys/fs/cgroup/memory.max) != max",
                   "an unbounded allocation takes the host down, not just the container",
                   waivable=True)

# --- kernel attack surface ----------------------------------------------
# The seccomp allowlist. Most escape-relevant syscalls (mount, setns, chroot)
# are already refused by --cap-drop=ALL, so testing those would credit seccomp
# for work the capability drop does. What the allowlist uniquely removes is the
# long tail that needs no capability at all -- personality() and mlock() are
# reachable in a capability-dropped container and are not on the list.
print()
print("Kernel surface (seccomp):")

def ffi(expr):
    return int(conn.get(expr))

# Positive control FIRST. TDI's FFI silently returns -1 when it mis-calls a
# function -- it cannot call variadic ones such as syscall() or ptrace() -- and
# a -1 for that reason is indistinguishable from "seccomp blocked it". Without
# this control every check below would pass for the wrong reason.
probe_ok = False
try:
    probe_ok = ffi("MdsShr->getpid()") > 0
except Exception:
    pass
expect("the FFI probe itself works", probe_ok,
       "MdsShr->getpid() did not return a pid",
       "a broken probe makes every seccomp check below meaningless")

if probe_ok:
    for name, expr in (("personality()", "MdsShr->personality(0)"),
                       ("mlock()", "MdsShr->mlock(0, 0)")):
        try:
            rc = ffi(expr)
        except Exception as exc:
            rc = -1   # refused outright is also blocked
        expect("%s is not on the allowlist" % name, rc == -1,
               "%s returned %s -- it should be EPERM" % (name, rc),
               "measured reachable without seccomp, so this is what the "
               "allowlist is actually buying")

# --- the service still works -------------------------------------------
# A sandbox that breaks the server is not a sandbox, it is an outage.
print()
print("Service:")
try:
    val = conn.get("10+32")
    expect("TDI still evaluates", int(val) == 42, "got %r" % val,
           "containment must not cost correctness")
except Exception as exc:
    expect("TDI still evaluates", False, str(exc), "containment must not cost correctness")

# Arithmetic proves the server is alive; only a tree read proves the read-only
# mounts, the tree path and the TDI function library all actually line up.
TREE = os.environ.get("RELAY_TREE", "efit01")
SHOT = int(os.environ.get("RELAY_SHOT", "190000"))
try:
    conn.openTree(TREE, SHOT)
    n = len(conn.get(r"\ipmhd"))
    expect("reads trees through :ro mounts", n > 0, "got %d samples" % n,
           "read-only must still be readable")

    # GetManyExecute lives in the noarch mdsplus-kernel package, not in
    # kernel_bin. An image with only the latter starts, accepts connections,
    # and fails every batch request with %TDI-E-UNKNOWN_VAR.
    gm = conn.getMany()
    gm.append("ip", r"\ipmhd")
    got = len(gm.execute()["ip"]["value"])
    expect("getMany works", got > 0, "got %d samples" % got,
           "the batch path is what the origin plugin uses")
except Exception as exc:
    print("  %-34s SKIP (%s: %s)"
          % ("tree reads", type(exc).__name__, str(exc)[:60]))
    print("    no %s/%d staged; containment checks above are unaffected"
          % (TREE, SHOT))

print()
if waived:
    print("%d control(s) WAIVED -- absent, and knowingly accepted:" % len(waived))
    for name, detail, why in waived:
        print("  - %s" % name)
        if detail:
            print("      %s" % detail)
        print("      why it matters: %s" % why)
    print("  This host cannot enforce them (cgroups v1 + rootless podman).")
    print("  The sandbox is running WITHOUT them. See docs/security.md.")
    print()

# --- ptdata --------------------------------------------------------------
# The no-network argument for ptdata rests on the library being INCAPABLE of
# remote I/O, not merely on the container having no route. Only the first
# survives someone adding a network for an unrelated reason, so assert it
# where the sandbox is actually running rather than only at image build.
#
# NEEDED entries, not ldd output: ldd prints resolved paths, and a path can
# contain almost any substring -- an earlier check in this repo failed a
# perfectly clean binary because the build directory was named after the
# package.
expect_shell_fails(
    "libptd3d does not link remote I/O",
    "sh -c 'readelf -d /usr/local/ptdata/lib/libptd3d.so "
    "| grep NEEDED | grep -qE \'libfdpio|libXrd\''",
    "libptd3d built with PTDATA_WITH_FDPIO=ON can open a Pelican URL, which "
    "turns the sandbox's no-network property from a structural guarantee back "
    "into a policy one.")

expect_shell_works(
    "the ptdata C ABI is present",
    "sh -c 'nm -D --defined-only /usr/local/ptdata/lib/libptd3d.so "
    "| grep -q ptdata_capi_size'",
    "a libptd3d that loads but exports no C ABI fails exactly like a data "
    "problem: every PTDATA2 call returns nothing, with nothing pointing at "
    "the cause.",
    waivable=True)

expect_shell_works(
    "the Pelican-path chain resolves from /",
    "sh -c 'cd / && /usr/local/fdp/tests/check_pelican_path.sh >/dev/null'",
    "index entries are absolute Pelican URLs that reach open(2) verbatim and "
    "resolve relative to /; without the chain every ptdata lookup fails as "
    "'file not found' with nothing pointing at the cause.",
    waivable=True)


if failures:
    print("%d of %d checks FAILED:" % (len(failures), checks))
    for name, detail, why in failures:
        print("  - %s" % name)
        if detail:
            print("      %s" % detail)
        print("      why it matters: %s" % why)
    sys.exit(1)

print("%d checks passed%s"
      % (checks - len(waived), ", %d waived" % len(waived) if waived else ""))
