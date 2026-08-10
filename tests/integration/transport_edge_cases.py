#!/usr/bin/env python3
"""Edge cases for the fdp:// transport that the happy-path comparison misses.

Connection lifecycle is where a byte-stream-to-request/response adapter tends to
go wrong: sessions leak, a second connection reuses the first one's state, or a
failure hangs instead of raising. None of that shows up in a single get().

    RELAY_HTTP_PORT=... RELAY_MDSIP_PORT=... python transport_edge_cases.py
"""

import os
import sys

import numpy as np
from MDSplus import Connection

PORT = os.environ["RELAY_HTTP_PORT"]
TREE = os.environ.get("RELAY_TREE", "efit01")
SHOT = int(os.environ.get("RELAY_SHOT", "190000"))
TARGET = "fdp://127.0.0.1:%s/mdsip" % PORT

failures = []


def check(name, ok, detail=""):
    print("  %-40s %s" % (name, "PASS" if ok else "*** FAIL ***"))
    if not ok:
        failures.append("%s %s" % (name, detail))


# --- many calls on one session ------------------------------------------
# The workload this transport exists for: thousands of one-signal get() calls,
# each a separate HTTP round trip on a reused curl handle and mdsip session.
c = Connection(TARGET)
c.openTree(TREE, SHOT)
vals = [len(np.asarray(c.get(r"\ipmhd"))) for _ in range(25)]
check("25 sequential get() on one session", len(set(vals)) == 1 and vals[0] > 0,
      str(set(vals)))

# --- interleaved trees on one session -----------------------------------
# openTree sets state on the mdsip connection; if the transport ever lost
# session affinity, this is where it would show.
c.openTree(TREE, SHOT)
a = np.asarray(c.get(r"\ipmhd"))
c.openTree(TREE, SHOT)
b = np.asarray(c.get(r"\ipmhd"))
check("state survives repeated openTree", np.array_equal(a, b))

# --- many independent connections ---------------------------------------
# Each must get its own relay session; sharing would interleave two callers
# into one mdsip byte stream.
conns = []
for i in range(8):
    ci = Connection(TARGET)
    ci.openTree(TREE, SHOT)
    conns.append(ci)
got = [len(np.asarray(ci.get(r"\ipmhd"))) for ci in conns]
check("8 concurrent connections are independent",
      len(set(got)) == 1 and got[0] > 0, str(set(got)))
del conns

# --- open/close churn ----------------------------------------------------
# A leaked session would hold an mdsip process until the idle reaper; a leaked
# curl handle would hold a socket. 40 cycles is enough for either to show up as
# a failure rather than a slow drift.
for i in range(40):
    ci = Connection(TARGET)
    ci.openTree(TREE, SHOT)
    ci.get("1+1")
    del ci
check("40 connect/disconnect cycles", True)

# --- errors surface as errors -------------------------------------------
try:
    c.get("this_is_not_tdi(")
    check("a bad expression raises", False, "no exception")
except Exception as exc:
    check("a bad expression raises", True, type(exc).__name__)

# A tree that does not exist must fail, not return something plausible.
try:
    c.openTree("no_such_tree", 1)
    check("a missing tree raises", False, "no exception")
except Exception:
    check("a missing tree raises", True)

# The session must still work after those errors.
c.openTree(TREE, SHOT)
check("session usable after errors", len(np.asarray(c.get(r"\ipmhd"))) > 0)

# --- an unreachable origin fails instead of hanging ----------------------
# Port 1 refuses immediately; the point is that ConnectToMds returns a failure
# rather than blocking forever or crashing in the transport.
try:
    bad = Connection("fdp://127.0.0.1:1/mdsip")
    try:
        bad.get("1+1")
        check("an unreachable origin fails", False, "get() succeeded")
    except Exception:
        check("an unreachable origin fails", True)
except Exception:
    check("an unreachable origin fails", True)   # failed at connect: also fine

print()
if failures:
    print("FAILURES:")
    for f in failures:
        print("  - %s" % f)
    sys.exit(1)
print("all transport edge cases passed")
