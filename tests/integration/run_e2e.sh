#!/usr/bin/env bash
# End-to-end: evaluator + standalone XRootD + plugin, fetched over root://.
#
# This is the fast inner loop -- no container, no federation, no TLS. The
# federation-level equivalent is tests/fed/run_fed_test.sh.
#
# Run from the repo root:  pixi run bash tests/integration/run_e2e.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK=${E2E_WORK:-/tmp/fdp-e2e}
PORT=${E2E_PORT:-10940}
PLUGIN="$ROOT/build/libXrdOssMdsplus.so"        # unsuffixed on purpose
PLUGIN_FILE="$ROOT/build/libXrdOssMdsplus-5.so" # what actually exists
SOCKET="$WORK/evald.sock"
EXPORT_DIR="$WORK/export"
CFG="$WORK/xrootd.cfg"

[ -f "$PLUGIN_FILE" ] || { echo "FAIL: build the plugin first (pixi run build)"; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/admin" "$WORK/run" "$EXPORT_DIR/plain" "$EXPORT_DIR/tdi"
echo "hello-passthrough" > "$EXPORT_DIR/plain/hello.txt"

sed -e "s#@@PLUGIN@@#$PLUGIN#" -e "s#@@SOCKET@@#$SOCKET#" \
    -e "s#@@EXPORT@@#$EXPORT_DIR#" -e "s#@@PORT@@#$PORT#" -e "s#@@WORK@@#$WORK#" \
    "$ROOT/tests/integration/xrootd-test.cfg" > "$CFG"

EVAL_PID=""
XRD_PID=""
cleanup() {
  [ -n "$XRD_PID"  ] && kill "$XRD_PID"  2>/dev/null || true
  [ -n "$EVAL_PID" ] && kill "$EVAL_PID" 2>/dev/null || true
}
trap cleanup EXIT

python "$ROOT/evaluator/fdp_mdsplus_evald.py" --socket "$SOCKET" & EVAL_PID=$!
for _ in $(seq 1 100); do [ -S "$SOCKET" ] && break; sleep 0.1; done
[ -S "$SOCKET" ] || { echo "FAIL: evaluator socket never appeared"; exit 1; }

fail() { echo "FAIL: $1"; echo "--- xrootd log ---"; tail -30 "$WORK/xrootd.log" 2>/dev/null; exit 1; }

xrootd -c "$CFG" -l "$WORK/xrootd.log" >/dev/null 2>&1 & XRD_PID=$!
# Fail fast if xrootd dies during startup: an xrdfs against a dead server
# blocks for its full retry budget, which turns a config typo into a hang.
for _ in $(seq 1 150); do
  kill -0 "$XRD_PID" 2>/dev/null || fail "xrootd exited during startup"
  grep -q "initialization failed" "$WORK/xrootd.log" 2>/dev/null \
    && fail "xrootd reported an initialization failure"
  XRD_STREAM_TIMEOUT=3 xrdfs "localhost:$PORT" stat /plain/hello.txt >/dev/null 2>&1 && break
  sleep 0.1
done
XRD_STREAM_TIMEOUT=3 xrdfs "localhost:$PORT" stat /plain/hello.txt >/dev/null 2>&1 \
  || fail "xrootd never became ready on port $PORT"

echo "--- 1. pass-through must still work ---"
xrdcp -f "root://localhost:$PORT//plain/hello.txt" "$WORK/out.txt" >/dev/null 2>&1 \
  || fail "pass-through fetch failed"
grep -q hello-passthrough "$WORK/out.txt" || fail "pass-through content wrong"
echo "OK"

# Build a canonical request path. '-' is the reserved no-tree segment, so this
# needs no staged MDSplus data (see tests/fed/FINDINGS.md).
mkpath() {
  python - "$@" <<'PY'
import base64, struct, sys
items = [(a.split('=', 1)[0].encode(), a.split('=', 1)[1].encode()) for a in sys.argv[1:]]
c = struct.pack('>BH', 1, len(items))
for n, e in items:
    c += struct.pack('>H', len(n)) + n + struct.pack('>I', len(e)) + e + struct.pack('>B', 0)
print('/tdi/-/00/00/00/00/0/' + base64.urlsafe_b64encode(c).decode().rstrip('='))
PY
}

echo "--- 2. single expression through the plugin ---"
P=$(mkpath 'r0=[1.0,2.0,3.0]')
xrdcp -f "root://localhost:$PORT/$P" "$WORK/result.bin" >/dev/null 2>&1 \
  || fail "virtual-file fetch failed for $P"
python - "$WORK/result.bin" <<'PY' || fail "decode failed"
import sys, numpy as np
from MDSplus import Data
res = Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))
got = np.asarray(res['r0']['value'])
assert np.allclose(got, [1.0, 2.0, 3.0]), got
print("OK", got)
PY

echo "--- 3. batch with an in-band error ---"
P=$(mkpath 'plain=[1.0,2.0,3.0]' "quoted=concat('a,b', ' c')" 'bad=this_is_not_tdi(')
xrdcp -f "root://localhost:$PORT/$P" "$WORK/batch.bin" >/dev/null 2>&1 \
  || fail "batch fetch failed"
python - "$WORK/batch.bin" <<'PY' || fail "batch decode failed"
import sys, numpy as np
from MDSplus import Data
r = Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))
assert np.allclose(np.asarray(r['plain']['value']), [1.0, 2.0, 3.0])
assert str(r['quoted']['value']) == 'a,b c', str(r['quoted']['value'])
assert 'error' in r['bad'], "a broken expression should error in band"
print("OK", sorted(r.keys()))
PY

echo "--- 4. stat reports the real size ---"
P=$(mkpath 'r0=[1.0,2.0,3.0]')
SIZE=$(xrdfs "localhost:$PORT" stat "$P" 2>/dev/null | awk '/^Size:/{print $2}')
ACTUAL=$(stat -c %s "$WORK/result.bin")
[ "$SIZE" = "$ACTUAL" ] || fail "stat size $SIZE != fetched size $ACTUAL"
echo "OK ($SIZE bytes)"

echo "--- 5. a malformed request is refused, not served ---"
if xrdcp -f "root://localhost:$PORT//tdi/-/00/00/00/00/0/QUJD" "$WORK/bad.bin" >/dev/null 2>&1; then
  fail "a non-Request payload should not have been served"
fi
echo "OK"

echo "--- ALL PASS ---"
