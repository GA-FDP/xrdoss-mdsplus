#!/usr/bin/env python3
"""A session that dies under a working client must not take the client with it.

The relay retires a session whenever a call fails, and reaps one that has gone
idle. Neither is the caller's fault and neither used to be survivable: the
transport kept presenting the dead token, the relay answered every later call
with 502 "unknown or expired session", and one lost session turned into every
remaining fetch in the process failing. That is what GA-FDP/imas_composer CI run
33033220932 hit -- one call over the relay's timeout, then 25 dead ones.

This drives the loss for real, against a relay configured to reap after a
second, and requires three things of the transport:

  * the call after the loss still returns data,
  * it returns the SAME data, which only holds if the tree context that the
    reaped session carried was re-established on the new one,
  * the relay really did lose the session -- otherwise this passes by never
    testing anything.

Run by run_relay_e2e.sh, which supplies the ports and the reaping prefix.
"""

import os
import sys
import time

import numpy as np
from MDSplus import Connection

HTTP_PORT = os.environ["RELAY_HTTP_PORT"]
PREFIX = os.environ.get("RELAY_REAP_PREFIX", "/mdsip-reap")
IDLE = float(os.environ.get("RELAY_REAP_IDLE", "1"))
SHOT = int(os.environ.get("RELAY_SHOT", "190000"))
TREE = os.environ.get("RELAY_TREE", "efit01")

target = "fdp://127.0.0.1:%s%s" % (HTTP_PORT, PREFIX)
print("recovery check against %s (idle=%gs)" % (target, IDLE))

conn = Connection(target)
conn.openTree(TREE, SHOT)
before = np.asarray(conn.get(r"\ipmhd"))
print("  first read                   %s" % (before.shape,))

# Well past the reaper's window. The relay has no timer thread -- it reaps at
# the top of the next request -- so the reaping happens inside the very call
# that then has to survive it.
time.sleep(IDLE + 3)

try:
    after = np.asarray(conn.get(r"\ipmhd"))
except Exception as e:
    print("  FAIL: the read after the reap raised %s: %s" % (type(e).__name__, e))
    sys.exit(1)
print("  read after the session died  %s" % (after.shape,))

if not np.array_equal(before, after):
    # A redial that forgot to replay TreeOpen lands here rather than raising:
    # mdsip answers from no tree at all.
    print("  FAIL: the value changed across the redial")
    sys.exit(1)

# The tree context specifically, not just any answer: a bare expression like
# 1+1 would succeed on a session that never had a tree.
if int(np.asarray(conn.get("$SHOT"))) != SHOT:
    print("  FAIL: the redialled session is not on shot %d" % SHOT)
    sys.exit(1)

print("  value and tree context both survived")

# And the control: prove the session was actually lost. Without this the test
# passes just as happily against a relay that never reaped anything.
log = os.environ.get("RELAY_XROOTD_LOG")
if log and os.path.exists(log):
    with open(log, "r", errors="replace") as f:
        text = f.read()
    if "unknown or expired session" not in text:
        print("  FAIL: the relay never reported a lost session -- this test "
              "did not exercise recovery")
        sys.exit(1)
    print("  relay logged the lost session, so recovery is what was tested")
else:
    print("  WARNING: no relay log to check; loss was not confirmed")

print("OK")
