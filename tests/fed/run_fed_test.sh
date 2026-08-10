#!/usr/bin/env bash
# Fetch virtual files through a full local Pelican federation.
#
# This is the only layer that exercises the DIRECTOR -- its path normalisation
# is the assumption most likely to break the design silently, and root:// never
# touches it. tests/integration/run_e2e.sh is the fast inner loop; this is the
# one that proves the federation path.
#
# Run from the repo root:  pixi run bash tests/fed/run_fed_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEB=${FEDBOX_URL:-https://localhost:8444}
MDSIP_PORT=${FED_MDSIP_PORT:-8000}
WORK=${FED_WORK:-/tmp/fdp-fed-run}
OUT=/tmp/fdp-fed-out
PLUGIN_FILE="$ROOT/build/libXrdOssMdsplus-5.so"

[ -f "$PLUGIN_FILE" ] || { echo "FAIL: build the plugin first (pixi run build)"; exit 1; }

MDSIP_PID=""
cleanup() {
  [ -n "$MDSIP_PID" ] && kill "$MDSIP_PID" 2>/dev/null || true
  bash "$ROOT/tests/fed/fedbox.sh" stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

# shellcheck source=../mdsip_helper.sh
. "$ROOT/tests/mdsip_helper.sh"

fail() {
  echo "FAIL: $1"
  echo "--- plugin log ---"
  podman logs fdp-fedbox 2>&1 | grep "ossmdsplus_" | tail -10 || true
  exit 1
}

rm -rf "$WORK" "$OUT"; mkdir -p "$WORK" "$OUT"

# mdsip runs on the host; the container uses --network host, so the plugin
# reaches it at localhost:$MDSIP_PORT.
start_mdsip "$MDSIP_PORT" "${FDP_TREES:-/tmp/fdp-trees}" "$WORK"

# The plugin links libMdsIpShr, whose transport (libMdsIpTCP.so) ConnectToMds
# dlopens BY NAME at runtime -- so the origin image must carry the MDSplus
# runtime no matter how the plugin is linked. Containerfile.runtime builds it.
if ! podman image exists "${FEDBOX_IMAGE:-localhost/fdp-origin-mdsplus:latest}" 2>/dev/null; then
  echo "FAIL: build the origin image first:"
  echo "      podman build -f Containerfile.runtime -t fdp-origin-mdsplus ."
  exit 1
fi

FEDBOX_EXTRA_MOUNTS="-v ${FDP_TREES:-/tmp/fdp-trees}:/trees:ro,z" \
bash "$ROOT/tests/fed/fedbox.sh" start \
    "$ROOT/tests/fed/xrootd-tdi.cfg" "$PLUGIN_FILE" >/dev/null \
  || { echo "FAIL: federation did not start"; exit 1; }

# '-' is the reserved no-tree segment, so none of this needs staged MDSplus data.
# The payload is MDSplus's own serialised GetMany list (see tests/mkpath.py).
export FDP_TREES="${FDP_TREES:-/tmp/fdp-trees}"
mkpath() { python "$ROOT/tests/mkpath.py" - 0 "$@"; }

get() {  # get <object-path> <outfile> -> prints HTTP code
  curl -ksL -w '%{http_code}' -o "$2" "$WEB/api/v1.0/director/origin$1"
}

echo "--- 1. pass-through still works ---"
curl -ksL "$WEB/api/v1.0/director/origin/test/hello.txt" | grep -q hello-federation \
  || fail "pass-through broken"
echo "OK"

echo "--- 2. virtual file through the director ---"
P=$(mkpath 'r0=[1.0,2.0,3.0]')
CODE=$(get "$P" "$OUT/single.bin")
[ "$CODE" = "200" ] || fail "expected 200, got $CODE for $P"
python - "$OUT/single.bin" <<'PY' || fail "decode failed"
import sys, numpy as np
from MDSplus import Data
res = Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))
got = np.asarray(res['r0']['value'])
assert np.allclose(got, [1.0, 2.0, 3.0]), got
print("OK", got)
PY

echo "--- 3. the director preserves the encoded segment verbatim ---"
SEG=$(basename "$P")
LOC=$(curl -ks -D- -o /dev/null "$WEB/api/v1.0/director/origin$P" | tr -d '\r' | grep -i '^location:')
echo "$LOC" | grep -qF "$SEG" || fail "director mangled the encoded segment: $LOC"
echo "OK (${#SEG} chars preserved)"

echo "--- 4. batch with an in-band error ---"
P=$(mkpath 'plain=[1.0,2.0,3.0]' "quoted=concat('a,b', ' c')" 'bad=this_is_not_tdi(')
CODE=$(get "$P" "$OUT/batch.bin")
[ "$CODE" = "200" ] || fail "batch expected 200, got $CODE"
python - "$OUT/batch.bin" <<'PY' || fail "batch decode failed"
import sys, numpy as np
from MDSplus import Data
r = Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))
assert np.allclose(np.asarray(r['plain']['value']), [1.0, 2.0, 3.0])
assert str(r['quoted']['value']) == 'a,b c', str(r['quoted']['value'])
assert 'error' in r['bad'], "a broken expression should error in band"
print("OK", sorted(r.keys()))
PY

echo "--- 5. a long multi-chunk request survives the director ---"
LONG=$(python -c "print('lng=[' + ','.join(str(float(i)) for i in range(120)) + ']')")
P=$(mkpath "$LONG")
NSEG=$(python - "$P" <<'PY'
import sys
print(len(sys.argv[1].strip('/').split('/')) - 6)
PY
)
CODE=$(get "$P" "$OUT/long.bin")
[ "$CODE" = "200" ] || fail "long request expected 200, got $CODE"
[ "$NSEG" -ge 2 ] || fail "expected a multi-chunk path, got $NSEG chunk(s)"
python - "$OUT/long.bin" <<'PY' || fail "long decode failed"
import sys, numpy as np
from MDSplus import Data
r = Data.deserialize(np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int8))
got = np.asarray(r['lng']['value'])
assert len(got) == 120 and got[0] == 0.0 and got[-1] == 119.0, (len(got), got[:3])
print("OK 120 elements recovered")
PY
echo "    (request occupied $NSEG path chunks, reassembled correctly)"

echo "--- 6. a malformed request is refused, not served ---"
CODE=$(get "/tdi/-/00/00/00/00/0/QUJD" "$OUT/bad.bin")
[ "$CODE" = "200" ] && fail "a payload mdsip cannot parse should not have been served"
echo "OK (HTTP $CODE)"

echo "--- ALL PASS ---"
