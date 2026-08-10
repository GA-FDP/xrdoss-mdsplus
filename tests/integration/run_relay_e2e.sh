#!/usr/bin/env bash
# End-to-end for the relay: an unmodified MDSplus.Connection reaching a real
# mdsip server through XRootD's HTTP port.
#
#   MDSplus.Connection -> mdsip_http_client.py -> XRootD/XrdHttpMdsip -> mdsip
#
# The point is that only the middle two hops are ours, and the client is stock.
# Whatever comes back has to match the same call made straight to mdsip.
#
# Run from the repo root:  pixi run bash tests/integration/run_relay_e2e.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK=${RELAY_WORK:-/tmp/fdp-relay-e2e}
PORT=${RELAY_PORT:-10950}
HTTP_PORT=${RELAY_HTTP_PORT:-10951}
MDSIP_PORT=${RELAY_MDSIP_PORT:-8110}
SHIM_PORT=${RELAY_SHIM_PORT:-10952}
TREES=${FDP_TREES:-/tmp/fdp-trees}

RELAY="$ROOT/build/libXrdHttpMdsip.so"        # unsuffixed on purpose
RELAY_FILE="$ROOT/build/libXrdHttpMdsip-5.so" # what actually exists
CFG="$WORK/xrootd.cfg"

[ -f "$RELAY_FILE" ] || { echo "FAIL: build first (pixi run build)"; exit 1; }
[ -d "$TREES" ] || { echo "SKIP: no trees at $TREES"; exit 0; }

rm -rf "$WORK"
mkdir -p "$WORK/admin" "$WORK/run" "$WORK/export/plain"

sed -e "s#@@RELAY@@#$RELAY#" -e "s#@@MDSIP@@#$MDSIP_PORT#" \
    -e "s#@@HTTPPORT@@#$HTTP_PORT#" -e "s#@@PORT@@#$PORT#" -e "s#@@WORK@@#$WORK#" \
    "$ROOT/tests/integration/xrootd-relay.cfg" > "$CFG"

MDSIP_PID=""; XRD_PID=""; SHIM_PID=""
cleanup() {
  [ -n "$SHIM_PID"  ] && kill "$SHIM_PID"  2>/dev/null || true
  [ -n "$XRD_PID"   ] && kill "$XRD_PID"   2>/dev/null || true
  [ -n "$MDSIP_PID" ] && kill "$MDSIP_PID" 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "FAIL: $1"; echo "--- xrootd log ---"; tail -40 "$WORK/xrootd.log" 2>/dev/null; exit 1; }

# shellcheck source=../mdsip_helper.sh
. "$ROOT/tests/mdsip_helper.sh"
start_mdsip "$MDSIP_PORT" "$TREES" "$WORK"

xrootd -c "$CFG" -l "$WORK/xrootd.log" >/dev/null 2>&1 & XRD_PID=$!
for _ in $(seq 1 150); do
  kill -0 "$XRD_PID" 2>/dev/null || fail "xrootd exited during startup"
  grep -q "initialization failed" "$WORK/xrootd.log" 2>/dev/null \
    && fail "xrootd reported an initialization failure"
  (exec 3<>/dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.1
done
(exec 3<>/dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null || fail "http port never opened"
exec 3<&- 3>&- 2>/dev/null || true

grep -q "XrdHttpMdsip relay" "$WORK/xrootd.log" || fail "the relay handler was never loaded"
echo "--- 0. handler loaded ---"
grep "XrdHttpMdsip relay" "$WORK/xrootd.log" | tail -1

python "$ROOT/tests/integration/mdsip_http_client.py" \
       "$SHIM_PORT" "127.0.0.1:$HTTP_PORT" /mdsip > "$WORK/shim.log" 2>&1 & SHIM_PID=$!
for _ in $(seq 1 100); do
  (exec 3<>/dev/tcp/127.0.0.1/"$SHIM_PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.1
done

echo "--- 1. a stock Connection works through the relay ---"
RELAY_SHIM_PORT="$SHIM_PORT" RELAY_MDSIP_PORT="$MDSIP_PORT" \
  python "$ROOT/tests/integration/relay_client_check.py" \
  || { echo "--- shim log ---"; cat "$WORK/shim.log"; fail "client check failed"; }

echo "--- 2. an unknown session is refused ---"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
       -H 'X-Fdp-Session: deadbeef' --data-binary 'x' \
       "http://127.0.0.1:$HTTP_PORT/mdsip/msg")
[ "$CODE" = "502" ] || fail "expected 502 for an unknown session, got $CODE"
echo "OK ($CODE)"

echo "--- 3. a call with no session header is refused ---"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary 'x' \
       "http://127.0.0.1:$HTTP_PORT/mdsip/msg")
[ "$CODE" = "400" ] || fail "expected 400 without a session header, got $CODE"
echo "OK ($CODE)"

echo "--- 4. an unknown action under the prefix is refused ---"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary '' \
       "http://127.0.0.1:$HTTP_PORT/mdsip/nonsense")
[ "$CODE" = "404" ] || fail "expected 404 for an unknown action, got $CODE"
echo "OK ($CODE)"

echo "--- 5. GET under the prefix is NOT claimed by the relay ---"
# The handler must not shadow the object namespace; a GET has to fall through
# to normal XRootD handling (which 404s here, but as a file, not as an action).
CODE=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/mdsip/connect")
[ "$CODE" != "200" ] || fail "the relay answered a GET it should have ignored"
echo "OK ($CODE)"

echo "--- ALL PASS ---"
