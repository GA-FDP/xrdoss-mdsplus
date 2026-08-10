#!/usr/bin/env python3
"""Compare a stock MDSplus.Connection through the relay against one straight to
mdsip.

Equality with the direct connection is the whole claim: the relay must not
transform anything, so anything short of byte-for-byte agreement on the decoded
values is a failure. Nothing here is relay-aware -- it is ordinary client code.
"""

import os
import sys

import numpy as np
from MDSplus import Connection

SHIM = int(os.environ["RELAY_SHIM_PORT"])
MDSIP = int(os.environ["RELAY_MDSIP_PORT"])
SHOT = int(os.environ.get("RELAY_SHOT", "190000"))
TREE = os.environ.get("RELAY_TREE", "efit01")

failures = []


def check(name, got, want):
    ok = np.array_equal(np.asarray(got), np.asarray(want))
    print("  %-28s %s" % (name, "MATCH" if ok else "DIFFERS"))
    if not ok:
        failures.append(name)


direct = Connection("127.0.0.1:%d" % MDSIP)
relayed = Connection("127.0.0.1:%d" % SHIM)

for conn in (direct, relayed):
    conn.openTree(TREE, SHOT)

# get() is the call that matters: it dominates real code (55 sites locally
# against 0 for getMany) and was the expensive case in the translating design.
for expr in (r"\ipmhd", r"\q95", r"dim_of(\ipmhd)", "10+32"):
    check(expr, relayed.get(expr), direct.get(expr))

# A large result, to exercise the multi-read paths on both sides.
psirz_direct = direct.get(r"\psirz")
psirz_relay = relayed.get(r"\psirz")
check(r"\psirz %s" % (np.asarray(psirz_direct).shape,), psirz_relay, psirz_direct)

# getMany: several expressions, one round trip.
def many(conn):
    gm = conn.getMany()
    gm.append("ip", r"\ipmhd")
    gm.append("q", r"\q95")
    return gm.execute()

md, mr = many(direct), many(relayed)
check("getMany ip", np.asarray(mr["ip"]["value"]), np.asarray(md["ip"]["value"]))
check("getMany q", np.asarray(mr["q"]["value"]), np.asarray(md["q"]["value"]))

# An error must arrive as an error, not as plausible data.
try:
    relayed.get("this_is_not_tdi(")
    print("  %-28s NO ERROR RAISED" % "bad expression")
    failures.append("bad expression")
except Exception as exc:
    print("  %-28s raised %s" % ("bad expression", type(exc).__name__))

# The session must survive a re-open, which is the stateful behaviour that makes
# per-session connections mandatory in the first place.
relayed.openTree(TREE, SHOT)
check("after re-open", relayed.get(r"\ipmhd"), direct.get(r"\ipmhd"))

if failures:
    print("FAILURES: %s" % ", ".join(failures))
    sys.exit(1)
print("  all relayed results match the direct connection")
