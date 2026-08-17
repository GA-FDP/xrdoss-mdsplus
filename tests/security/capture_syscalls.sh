#!/usr/bin/env bash
# Record which syscalls the sandbox actually makes, under a realistic workload.
#
# An allowlist seccomp profile is only as good as the workload used to derive
# it: a syscall that appears solely on an error path, or only when a tree is
# large enough to mmap differently, will be missing and the service will fail in
# production rather than here. So this drives the same things the integration
# tests do -- connect, openTree, get, getMany, a 4 MB result, errors, several
# concurrent connections, disconnects -- plus PTData, and not just a smoke test.
#
#   pixi run bash tests/security/capture_syscalls.sh [outfile]
#
# The output is a sorted syscall list. Regenerating the profile from it is a
# deliberate, reviewed step (deploy/mdsip-seccomp.json), not automatic: the list
# is evidence, not the policy.
#
# WHAT THIS TRACES, AND WHY IT CHANGED
# ------------------------------------
# It traces the shipped IMAGE running its real ENTRYPOINT, not a bare `mdsip -m`
# on the host. Three things make that difference matter, and all three are
# invisible to a host capture:
#
#   * the entrypoint is socat, which forks AND execs a fresh mdsip per
#     connection -- a different process-creation path from `mdsip -m`, which
#     forks only
#   * `cd /` and the Pelican-path chain mean ptdata opens files through a
#     symlink chain by relative path
#   * ptdata is in the image now. Its file I/O is not in any capture taken
#     before it was added, and the profile in deploy/ predates it.
#
# strace goes into a THROWAWAY derived image rather than the shipped one. The
# sandbox's threat model assumes the client already has code execution inside
# it, so shipping a tracer would hand that client a debugger. The derived image
# also carries the same libptd3d built without fdpio, so the capture cannot pick
# up XRootD or libcurl syscalls that the real deployment can never make.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-/tmp/fdp-syscalls.txt}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fdp-syscap.XXXXXX")"
IMAGE="${MDSIP_IMAGE:-localhost/fdp-mdsip:latest}"
CAPTURE_IMAGE=fdp-mdsip-capture
NAME="fdp-mdsip-capture-$$"
TREES="${FDP_TREES:-/tmp/fdp-trees}"

# A free port, chosen at run time. A hard-coded one silently invalidates the
# whole capture: a leftover server keeps the port, the traced process dies on
# EADDRINUSE, and the port-wait loop connects to the OLD server quite happily --
# so the workload succeeds while strace records nothing but a failed startup.
# That happened.
PORT="${CAPTURE_PORT:-$(python -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')}"

command -v podman >/dev/null || { echo "SKIP: podman not available"; exit 0; }
[ -d "$TREES" ] || { echo "SKIP: no trees at $TREES"; exit 0; }

# podman derives paths from /run/user/$UID, which does not survive the login
# session that created it. Assign unconditionally: the variable is usually still
# SET to a directory that no longer exists, and `:-` would preserve it.
if [ ! -d "/run/user/$(id -u)" ]; then
    export XDG_RUNTIME_DIR="/tmp/xdg-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR/libpod/tmp" && chmod 700 "$XDG_RUNTIME_DIR"
fi

cleanup() {
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# A PTData fixture, so the capture covers ptdata's file I/O. Without it the
# resulting profile is exactly the one we already have: correct for trees and
# untested for the reason this sandbox now exists.
python "$ROOT/tests/make_ptdata_fixture.py" "$WORK/sysd3" >/dev/null

echo "building the capture image (shipped image + strace)..."
podman build -t "$CAPTURE_IMAGE" -f - "$ROOT" >/dev/null <<EOF
FROM $IMAGE
USER root
RUN dnf -y install strace && dnf clean all && rm -rf /var/cache/dnf
USER mdsip
EOF

# -f to follow forks: socat execs a fresh mdsip per connection, so essentially
# every interesting syscall is in a descendant rather than in the parent.
podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" -p "127.0.0.1:$PORT:8000" \
    -v "$TREES:/trees:ro" \
    -v "$WORK/sysd3:/fdp-archives/archives/ptdata:ro" \
    -e efit01_path=/trees -e default_tree_path=/trees \
    -e "SYS_D3=pelican://osg-htc.org:443/fdp-d3d/archives/ptdata" \
    -e SYS_D3_DELIM=';' -e PTDATA_VAX_FLOATS=0 \
    `# The real deployment's filesystem flags, because they change what the` \
    `# server can do and therefore which syscalls it makes.` \
    --read-only --tmpfs /tmp:rw,nosuid,nodev,size=256m \
    `# The trace goes to the container's own tmpfs, not a bind mount: the` \
    `# server runs as uid 5000, which maps to a subuid that cannot write to a` \
    `# host directory owned by the invoking user. It is copied out below.` \
    --entrypoint '["/usr/bin/strace","-f","-qq","-o","/tmp/strace.txt","/usr/local/bin/fdp-mdsip-server"]' \
    "$CAPTURE_IMAGE" >/dev/null

for _ in $(seq 1 150); do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.1
done

# Belt and braces: prove we are talking to the process being traced.
if ! (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
  echo "FAIL: nothing is listening on $PORT; the capture would be of a failed"
  echo "      startup while the workload talked to something else."
  podman logs "$NAME" 2>&1 | tail -20
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

def exercise_ptdata(c):
    # The whole reason this capture was retaken. Each of these reaches
    # libptd3d, which opens shotfiles through the Pelican-path symlink chain
    # by RELATIVE path -- a different open() pattern from the tree reads above.
    np.asarray(c.get('PTDATA2("IP", 99001, 0)'))
    np.asarray(c.get('PTDATA2("BT", 99001, 0)'))
    np.asarray(c.get('PTNPTS("IP", 99001)'))       # site shim -> our PTHEAD2
    np.asarray(c.get('PTHEAD_RFIX("IP", 99001)'))  # the header path
    c.get('PTHEAD2_ASCII("IP", 99001)')
    np.asarray(c.get('PTDATA2("NOPE", 99001, 0)'))  # absent: the error path
    try:
        c.get('PTDATA2("IP", 99001, 3)')            # unsupported ical: raises
    except Exception:
        pass

# Several sequential connections, then several at once: connection setup and
# teardown are their own syscall paths, and socat execs a fresh mdsip for each.
for _ in range(3):
    c = Connection("127.0.0.1:%s" % port); exercise(c); exercise_ptdata(c); del c

conns = [Connection("127.0.0.1:%s" % port) for _ in range(4)]
for c in conns:
    exercise(c); exercise_ptdata(c)
del conns
print("  workload done")
PY

sleep 2
# Copy the trace out BEFORE stopping: it lives on the container's tmpfs, which
# does not survive the container.
podman cp "$NAME:/tmp/strace.txt" "$WORK/strace.txt" 2>/dev/null || {
    echo "FAIL: could not copy the trace out of the container"
    podman logs "$NAME" 2>&1 | tail -10
    exit 1
}
podman stop -t 5 "$NAME" >/dev/null 2>&1 || true

# Raw trace rather than `-c`: the summary is only written when strace exits
# cleanly, and tracing a forking daemon it frequently is not -- an empty file
# would silently become an empty allowlist. Raw output is flushed as it goes.
# Lines look like: "12345 openat(AT_FDCWD, ...) = 3"
[ -s "$WORK/strace.txt" ] || { echo "FAIL: no trace was written"; exit 1; }
sed -nE 's/^[0-9]+[[:space:]]+([a-z_0-9]+)\(.*/\1/p' "$WORK/strace.txt" \
  | sort -u > "$OUT"

# A capture without these did not observe a serving process. execve is here
# because socat EXECS a fresh mdsip per connection -- its absence means the
# trace caught the listener but never a served connection, which is most of
# the syscalls we care about. pread64 is here because it is how the tree and
# shotfile reads actually happen: without it the trace saw connections but no
# data.
for required in clone listen execve openat pread64; do
  grep -qx "$required" "$OUT" || {
    echo "FAIL: '$required' missing -- this capture did not trace a serving"
    echo "      sandbox. Refusing to hand over a list that would become a"
    echo "      broken allowlist."
    exit 1
  }
done

# accept OR accept4: socat uses accept(2), where the older `mdsip -m` entrypoint
# used accept4(2). Requiring accept4 by name failed a perfectly good capture of
# the current architecture -- the guard was written for the previous one.
grep -qxE "accept|accept4" "$OUT" || {
  echo "FAIL: neither accept nor accept4 present -- nothing ever accepted a"
  echo "      connection, so this trace is not of a serving sandbox."
  exit 1
}

# Keep the raw trace beside the list. The list is what feeds the profile, but
# every question about WHY a syscall is in it is answered only by the trace.
cp "$WORK/strace.txt" "${OUT%.txt}-strace.txt" 2>/dev/null || true

echo
echo "captured $(wc -l < "$OUT") distinct syscalls -> $OUT"

# The point of retaking this: what does the shipped profile not cover?
PROFILE="$ROOT/deploy/mdsip-seccomp.json"
if [ -f "$PROFILE" ]; then
  python - "$OUT" "$PROFILE" <<'PY'
import json, sys
captured = set(open(sys.argv[1]).read().split())
prof = json.load(open(sys.argv[2]))
allowed = {n for s in prof.get("syscalls", []) for n in s.get("names", [])}
missing = sorted(captured - allowed)
unused = sorted(allowed - captured)
print()
print("NOT in deploy/mdsip-seccomp.json (%d):" % len(missing))
print("  " + (", ".join(missing) if missing else "(none -- the profile covers this workload)"))
print()
print("allowed but unobserved (%d) -- not necessarily removable; an error path"
      % len(unused))
print("  may need them, so treat this as a review list, not a delete list.")
PY
fi

column -c 100 "$OUT" 2>/dev/null || cat "$OUT"
