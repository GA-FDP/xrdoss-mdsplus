#!/usr/bin/env python3
"""A stock MDSplus.Connection reaching real data through the Pelican director.

This is the whole claim in one file: an unmodified client, a federation URL, and
tree data coming back. Everything else in tests/fed/ exercises the GET path;
this is the tunnel's.

    FED_TARGET=fdp://director:8444/mdsip python fed_client_check.py
"""

import os
import sys

import numpy as np
from MDSplus import Connection

TARGET = os.environ["FED_TARGET"]
TREE = os.environ.get("RELAY_TREE", "efit01")
SHOT = int(os.environ.get("RELAY_SHOT", "190000"))

print("  target: %s" % TARGET)

try:
    c = Connection(TARGET)
    c.openTree(TREE, SHOT)
except Exception as exc:
    print("  *** FAILED to connect/open: %s: %s" % (type(exc).__name__, exc))
    sys.exit(1)

failures = []
for expr in (r"\ipmhd", r"\q95", "10+32"):
    try:
        v = np.asarray(c.get(expr))
        ok = v.size > 0
        print("  %-14s %s  shape=%s" % (expr, "OK " if ok else "EMPTY", v.shape))
        if not ok:
            failures.append(expr)
    except Exception as exc:
        print("  %-14s FAILED %s: %s" % (expr, type(exc).__name__, str(exc)[:60]))
        failures.append(expr)

# The batch path, which is what the origin plugin uses and what a real caller
# reaches for when fetching more than one signal.
try:
    gm = c.getMany()
    gm.append("ip", r"\ipmhd")
    n = len(np.asarray(gm.execute()["ip"]["value"]))
    print("  %-14s OK   %d samples" % ("getMany", n))
except Exception as exc:
    print("  %-14s FAILED %s" % ("getMany", type(exc).__name__))
    failures.append("getMany")

# A 4.3 MB answer, to confirm the redirect path carries a large body rather
# than only the small ones.
try:
    v = np.asarray(c.get(r"\psirz"))
    print("  %-14s OK   shape=%s" % (r"\psirz", v.shape))
except Exception as exc:
    print("  %-14s FAILED %s" % (r"\psirz", type(exc).__name__))
    failures.append("psirz")

print()
if failures:
    print("  => FEDERATION CLIENT PATH FAILED: %s" % ", ".join(failures))
    sys.exit(1)
print("  => A STOCK MDSplus.Connection WORKS THROUGH THE DIRECTOR")
