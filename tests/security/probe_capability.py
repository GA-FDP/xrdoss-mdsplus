#!/usr/bin/env python3
"""Measure what an mdsip client can actually do to the server.

The sandbox has to be sized to real capability, not to assumption. This probes a
running mdsip with the powers an ordinary client already has -- no exploit, no
memory corruption, just TDI -- and reports which succeed.

Every check here is something a token holder could do today. Run it against a
throwaway mdsip you started yourself:

    python probe_capability.py <host:port> [tree] [shot]

Exit status is 0 always: this reports, it does not judge. `verify_sandbox.sh`
is the script that turns these into pass/fail expectations.
"""

import os
import sys

from MDSplus import Connection

TARGET = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:8100"

conn = Connection(TARGET)

results = []


def probe(name, expr, note=""):
    """Evaluate `expr` and record whether it did what it says on the tin."""
    try:
        got = conn.get(expr)
        results.append((name, "WORKED", str(got)[:120].replace("\n", " "), note))
    except Exception as exc:
        results.append((name, "blocked", "%s: %s" % (type(exc).__name__,
                                                     str(exc)[:90]), note))


# --- code execution ------------------------------------------------------
# TDI's spawn() runs a shell command. This is not a bug being exploited; it is
# a documented TDI builtin, which is why "just don't expose mdsip" was never a
# sufficient answer.
probe("spawn(id)", 'spawn("id")', "shell command execution")
probe("spawn(hostname)", 'spawn("hostname")', "")
probe("spawn(env)", 'spawn("env | head -20")', "leaks the server's environment")

# --- reading the server's filesystem -------------------------------------
# Anything the mdsip user can read, a client can read.
probe("read /etc/passwd", 'spawn("cat /etc/passwd")', "arbitrary file read")
probe("read origin token", 'spawn("cat /etc/pelican/issuer.jwk 2>&1")',
      "the reason mdsip must not share a container with the origin")

# --- writing ------------------------------------------------------------
probe("write /tmp", 'spawn("touch /tmp/mdsip-probe-wrote-here && echo wrote")',
      "filesystem write")
probe("write a tree dir",
      r'spawn("touch $efit01_path/probe-wrote-here 2>&1 && echo wrote")',
      "tree integrity -- trees must be read-only even to the server")

# --- network egress ------------------------------------------------------
# A server that can reach the internet is a pivot and an exfiltration path.
probe("dns", 'spawn("getent hosts example.com 2>&1 | head -1")', "egress")
probe("outbound tcp",
      'spawn("timeout 3 bash -c \'</dev/tcp/1.1.1.1/53\' 2>&1 && echo open || echo refused")',
      "egress")

# --- resource exhaustion -------------------------------------------------
probe("fork capacity", 'spawn("ulimit -u")', "pids limit")
probe("memory limit", 'spawn("cat /sys/fs/cgroup/memory.max 2>/dev/null || echo none")', "")

# --- native code ---------------------------------------------------------
# image->routine() is a generic FFI: it dlopens a library and calls into it.
# Even with spawn() removed, this alone is arbitrary native code execution.
probe("ffi getpid", 'libc->getpid()', "generic FFI into any shared library")
probe("ffi system", 'libc->system("echo ffi-ran")', "FFI reaching system(3)")

# --- privilege -----------------------------------------------------------
probe("whoami", 'spawn("id -u")', "is it running as root?")
probe("capabilities", 'spawn("grep CapEff /proc/self/status")', "")

width = max(len(r[0]) for r in results)
print()
print("%-*s  %-8s  %s" % (width, "PROBE", "RESULT", "OUTPUT"))
print("-" * (width + 60))
for name, status, out, note in results:
    print("%-*s  %-8s  %s" % (width, name, status, out))
    if note:
        print("%-*s            (%s)" % (width, "", note))

worked = [r[0] for r in results if r[1] == "WORKED"]
print()
print("%d/%d probes succeeded" % (len(worked), len(results)))
