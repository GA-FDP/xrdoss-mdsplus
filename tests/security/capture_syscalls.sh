#!/usr/bin/env bash
# Record which syscalls mdsip actually makes, under a realistic workload.
#
# An allowlist seccomp profile is only as good as the workload used to derive
# it: a syscall that appears solely on an error path, or only when a tree is
# large enough to mmap differently, will be missing and the service will fail in
# production rather than here. So this drives the same things the integration
# tests do -- connect, openTree, get, getMany, a 4 MB result, errors, several
# concurrent connections, disconnects -- and not just a smoke test.
#
#   pixi run bash tests/security/capture_syscalls.sh [outfile]
#
# The output is a sorted syscall list. Regenerating the profile from it is a
# deliberate, reviewed step (deploy/mdsip-seccomp.json), not automatic: the list
# is evidence, not the policy.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-/tmp/fdp-syscalls.txt}"
WORK=/tmp/fdp-syscap
# A free port, chosen at run time. A hard-coded one silently invalidates the
# whole capture: a leftover mdsip from an earlier run keeps the port, the traced
# process dies on EADDRINUSE, and the port-wait loop connects to the OLD server
# quite happily -- so the workload succeeds while strace records nothing but a
# failed startup. That happened.
PORT="${CAPTURE_PORT:-$(python -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')}"
TREES="${FDP_TREES:-/tmp/fdp-trees}"

command -v strace >/dev/null || { echo "SKIP: strace not available"; exit 0; }
[ -d "$TREES" ] || { echo "SKIP: no trees at $TREES"; exit 0; }

rm -rf "$WORK"; mkdir -p "$WORK"
printf '* | SELF\n' > "$WORK/mdsip.hosts"

# -f to follow forks: mdsip -m serves each connection in a child, so the
# interesting syscalls are mostly NOT in the parent.
MDS_PATH="$CONDA_PREFIX/tdi;$CONDA_PREFIX/tdi/remote" efit01_path="$TREES" \
  strace -f -qq -o "$WORK/strace.txt" \
  mdsip -m -p "$PORT" -h "$WORK/mdsip.hosts" > "$WORK/mdsip.log" 2>&1 &
STRACE_PID=$!
trap 'kill -INT "$STRACE_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 150); do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.1
done

# Belt and braces: prove we are talking to the process being traced.
if grep -qi "error binding" "$WORK/mdsip.log" 2>/dev/null; then
  echo "FAIL: the traced mdsip could not bind port $PORT; the capture would be"
  echo "      of a failed startup while the workload talked to something else."
  cat "$WORK/mdsip.log"
  exit 1
fi

echo "driving a realistic workload on port $PORT..."
CAPTURE_PORT="$PORT" python - <<'PY'
import os
import numpy as np
from MDSplus import Connection

port = os.environ["CAPTURE_PORT"]
tree, shot = os.environ.get("RELAY_TREE", "efit01"), int(os.environ.get("RELAY_SHOT", "190000"))

def exercise(c):
    c.openTree(tree, shot)
    np.asarray(c.get(r"\ipmhd"))
    np.asarray(c.get(r"\q95"))
    np.asarray(c.get(r"dim_of(\ipmhd)"))
    np.asarray(c.get("10+32"))
    np.asarray(c.get(r"\psirz"))          # ~4 MB: exercises the large-write path
    gm = c.getMany(); gm.append("ip", r"\ipmhd"); gm.execute()
    try:
        c.get("this_is_not_tdi(")          # the error path
    except Exception:
        pass
    try:
        c.openTree("no_such_tree", 1)      # a failing open
    except Exception:
        pass
    c.openTree(tree, shot)                 # still usable afterwards

# Several sequential connections, then several at once: connection setup and
# teardown are their own syscall paths, and mdsip forks for each.
for _ in range(3):
    c = Connection("127.0.0.1:%s" % port); exercise(c); del c

conns = [Connection("127.0.0.1:%s" % port) for _ in range(4)]
for c in conns:
    exercise(c)
del conns
print("  workload done")
PY

sleep 1
kill -INT "$STRACE_PID" 2>/dev/null || true
sleep 2

# Raw trace rather than `-c`: the summary is only written when strace exits
# cleanly, and tracing a forking daemon it frequently is not -- an empty file
# would silently become an empty allowlist. Raw output is flushed as it goes.
# Lines look like: "12345 openat(AT_FDCWD, ...) = 3"
sed -nE 's/^[0-9]+[[:space:]]+([a-z_0-9]+)\(.*/\1/p' "$WORK/strace.txt" \
  | sort -u > "$OUT"

# A capture without these did not observe a serving process: mdsip -m forks per
# connection and must accept on a listening socket, so their absence means the
# trace is of something else entirely.
for required in clone accept4 listen; do
  grep -qx "$required" "$OUT" || {
    echo "FAIL: '$required' missing -- this capture did not trace a serving mdsip."
    echo "      Refusing to hand over a list that would become a broken allowlist."
    exit 1
  }
done

echo
echo "captured $(wc -l < "$OUT") distinct syscalls -> $OUT"
column -c 100 "$OUT" 2>/dev/null || cat "$OUT"
