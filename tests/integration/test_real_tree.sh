#!/usr/bin/env bash
# Verify real MDSplus nodes fetched through the plugin equal a direct read.
#
# Everything up to here used synthetic expressions like [1.0,2.0,3.0]. This is
# the first test that opens a real tree, resolves real nodes, and compares
# against MDSplus itself -- the only way to catch a wrong-data bug as opposed
# to a plumbing bug.
#
# Stage a tree first (about 9.5 MB):
#
#   mkdir -p /tmp/fdp-trees
#   cd ../toksearch_d3d
#   for ext in tree characteristics datafile; do
#     pixi run fdp run xrdcp -f -s \
#       "root://fdp-d3d-origin.nationalresearchplatform.org:8443/\
# /fdp-d3d/archives/mdsplus/codes/efit01/00/00/19/00/efit01_190000.$ext" \
#       "/tmp/fdp-trees/efit01_190000.$ext"
#   done
#
# Run from the repo root:  pixi run bash tests/integration/test_real_tree.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TREES=${FDP_TREES:-/tmp/fdp-trees}
TREE=efit01
SHOT=190000
WORK=${REAL_WORK:-/tmp/fdp-real}
PORT=${REAL_PORT:-10941}
PLUGIN="$ROOT/build/libXrdOssMdsplus.so"          # unsuffixed on purpose
PLUGIN_FILE="$ROOT/build/libXrdOssMdsplus-5.so"
MDSIP_PORT=${REAL_MDSIP_PORT:-8101}
EXPORT_DIR="$WORK/export"
CFG="$WORK/xrootd.cfg"

[ -f "$PLUGIN_FILE" ] || { echo "FAIL: build the plugin first (pixi run build)"; exit 1; }
[ -f "$TREES/${TREE}_${SHOT}.datafile" ] || {
  echo "FAIL: stage the tree first -- see the header of this script"; exit 1; }

# MDSplus locates a tree via <treename>_path; without it, TreeFOPENR.
export efit01_path="$TREES"

rm -rf "$WORK"
mkdir -p "$WORK/admin" "$WORK/run" "$EXPORT_DIR/tdi" "$EXPORT_DIR/tdi-version" "$EXPORT_DIR/plain"
echo hello > "$EXPORT_DIR/plain/hello.txt"

sed -e "s#@@PLUGIN@@#$PLUGIN#" -e "s#@@MDSIP@@#localhost:$MDSIP_PORT#" \
    -e "s#@@TREES@@#${FDP_TREES:-/tmp/fdp-trees}#" -e "s#@@EXPORT@@#$EXPORT_DIR#" -e "s#@@PORT@@#$PORT#" -e "s#@@WORK@@#$WORK#" \
    "$ROOT/tests/integration/xrootd-test.cfg" > "$CFG"

MDSIP_PID=""; XRD_PID=""
cleanup() {
  [ -n "$XRD_PID"   ] && kill "$XRD_PID"   2>/dev/null || true
  [ -n "$MDSIP_PID" ] && kill "$MDSIP_PID" 2>/dev/null || true
}
trap cleanup EXIT

# shellcheck source=../mdsip_helper.sh
. "$ROOT/tests/mdsip_helper.sh"
fail() { echo "FAIL: $1"; tail -20 "$WORK/xrootd.log" 2>/dev/null; exit 1; }

start_mdsip "$MDSIP_PORT" "$TREES" "$WORK"

xrootd -c "$CFG" -l "$WORK/xrootd.log" >/dev/null 2>&1 & XRD_PID=$!
disown "$XRD_PID" 2>/dev/null || true
for _ in $(seq 1 150); do
  kill -0 "$XRD_PID" 2>/dev/null || fail "xrootd exited during startup"
  grep -q "initialization failed" "$WORK/xrootd.log" 2>/dev/null && fail "xrootd init failed"
  XRD_STREAM_TIMEOUT=3 xrdfs "localhost:$PORT" stat /plain/hello.txt >/dev/null 2>&1 && break
  sleep 0.1
done

# Build a path naming a real tree and shot.
export FDP_TREES="${FDP_TREES:-/tmp/fdp-trees}"
mkpath() { python "$ROOT/tests/mkpath.py" "$TREE" "$SHOT" "$@"; }

echo "=== comparing against a direct MDSplus read ==="
for NODE in '\ipmhd' '\q95' '\psirz'; do
  printf '%-10s ' "$NODE"
  P=$(mkpath "r0=$NODE")
  xrdcp -f "root://localhost:$PORT/$P" "$WORK/got.bin" >/dev/null 2>&1 \
    || fail "fetch failed for $NODE"
  python - "$WORK/got.bin" "$TREE" "$SHOT" "$NODE" <<'PY' || fail "mismatch for $NODE"
import sys
import numpy as np
import MDSplus
from MDSplus import Data

payload, tree, shot, node = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]

want = np.asarray(MDSplus.Tree(tree, shot, 'READONLY').tdiExecute(node))
got = np.asarray(
    Data.deserialize(np.frombuffer(open(payload, 'rb').read(), dtype=np.int8))['r0']['value'])

assert got.shape == want.shape, "shape %s != %s" % (got.shape, want.shape)
assert got.dtype == want.dtype, "dtype %s != %s" % (got.dtype, want.dtype)
assert np.array_equal(got, want, equal_nan=True), "values differ"
print("OK  shape=%-14s dtype=%-8s bytes=%d" % (got.shape, got.dtype, got.nbytes))
PY
done

echo "=== the time base is fetchable as its own item ==="
P=$(mkpath 'r0=dim_of(\ipmhd)')
xrdcp -f "root://localhost:$PORT/$P" "$WORK/dim.bin" >/dev/null 2>&1 || fail "dim_of fetch failed"
python - "$WORK/dim.bin" <<'PY' || fail "dim_of mismatch"
import sys
import numpy as np
import MDSplus
from MDSplus import Data
want = np.asarray(MDSplus.Tree('efit01', 190000, 'READONLY').tdiExecute(r'dim_of(\ipmhd)'))
got = np.asarray(
    Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))['r0']['value'])
assert np.array_equal(got, want, equal_nan=True), "time base differs"
print("OK  %d time points, %.1f..%.1f ms (dims do NOT ride along; ask for them)"
      % (len(got), got[0], got[-1]))
PY

echo "=== one batch returns several real nodes ==="
P=$(mkpath 'ip=\ipmhd' 'q95=\q95' 'times=dim_of(\ipmhd)')
xrdcp -f "root://localhost:$PORT/$P" "$WORK/batch.bin" >/dev/null 2>&1 || fail "batch fetch failed"
python - "$WORK/batch.bin" <<'PY' || fail "batch mismatch"
import sys
import numpy as np
import MDSplus
from MDSplus import Data
t = MDSplus.Tree('efit01', 190000, 'READONLY')
r = Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))
for key, expr in (('ip', r'\ipmhd'), ('q95', r'\q95'), ('times', r'dim_of(\ipmhd)')):
    want = np.asarray(t.tdiExecute(expr))
    got = np.asarray(r[key]['value'])
    assert np.array_equal(got, want, equal_nan=True), "%s differs" % key
print("OK  all three match a direct read")
PY

echo "=== the version-lookup endpoint agrees with the path builder ==="
# A remote client cannot stat the tree, so it asks the origin. This endpoint is
# what the client transport will call at openTree() time.
VPATH="/tdi-version/$TREE/$(python -c "d='%08d'%($SHOT//100); print('/'.join(d[i:i+2] for i in range(0,8,2)))")/$SHOT"
xrdcp -f "root://localhost:$PORT/$VPATH" "$WORK/ver.txt" >/dev/null 2>&1 \
  || fail "version lookup failed for $VPATH"
SERVED=$(tr -d '\n' < "$WORK/ver.txt")
EXPECTED=$(mkpath 'r0=1' | sed -E 's#.*/(v[0-9a-f]{16})/.*#\1#')
[ "$SERVED" = "$EXPECTED" ] || fail "lookup said $SERVED, path builder said $EXPECTED"
echo "OK ($SERVED)"

echo "=== a stale version is refused, not served ==="
# The whole reason versions exist: XrdPfc never revalidates, so an object whose
# tree has been re-analysed must not keep being served under the old name.
GOOD=$(mkpath 'r0=\ipmhd')
STALE=$(echo "$GOOD" | sed -E 's#/v[0-9a-f]{16}/#/vdeadbeefdeadbeef/#')
[ "$STALE" != "$GOOD" ] || fail "could not construct a stale path"
if xrdcp -f "root://localhost:$PORT/$STALE" "$WORK/stale.bin" >/dev/null 2>&1; then
  fail "a stale version should not have been served"
fi
echo "OK (refused)"

echo "=== the current version still works after that ==="
xrdcp -f "root://localhost:$PORT/$GOOD" "$WORK/good.bin" >/dev/null 2>&1 \
  || fail "current version stopped working"
echo "OK"

echo "=== a missing shot fails cleanly ==="
P=$(python - <<'PY'
import base64, struct
n, e = b'r0', b'\\ipmhd'
c = struct.pack('>BH',1,1) + struct.pack('>H',len(n)) + n + struct.pack('>I',len(e)) + e + struct.pack('>B',0)
print('/tdi/efit01/00/00/99/99/999999/vdeadbeefdeadbeef/' + base64.urlsafe_b64encode(c).decode().rstrip('='))
PY
)
if xrdcp -f "root://localhost:$PORT/$P" "$WORK/missing.bin" >/dev/null 2>&1; then
  fail "a missing shot should not have been served"
fi
echo "OK"

echo "--- ALL PASS ---"
