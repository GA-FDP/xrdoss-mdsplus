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

# Several DISTINCT shots, because the tree-context check cannot work without
# them: if every connection opens the same shot, a server sharing one tree
# context returns exactly what a correct one returns. That is precisely how the
# old "8 concurrent connections are independent" check passed while `mdsip -m`
# was serving every client whichever shot was opened last.
#
# The fixture ships one shot, so clone it. The clones carry identical DATA --
# only the shot number differs -- which is all $SHOT needs to tell them apart.
BASE_SHOT="$(ls "$TREES"/efit01_*.tree 2>/dev/null | head -1 |
             sed 's/.*efit01_\([0-9]*\)\.tree/\1/')"
RELAY_SHOTS=""
if [ -n "$BASE_SHOT" ] && [ -w "$TREES" ]; then
  RELAY_SHOTS="$BASE_SHOT"
  for i in 1 2 3 4 5 6 7; do
    n=$((BASE_SHOT + i))
    for ext in tree datafile characteristics; do
      [ -f "$TREES/efit01_$n.$ext" ] || \
        cp "$TREES/efit01_$BASE_SHOT.$ext" "$TREES/efit01_$n.$ext" 2>/dev/null || true
    done
    [ -f "$TREES/efit01_$n.tree" ] && RELAY_SHOTS="$RELAY_SHOTS,$n"
  done
fi
export RELAY_SHOTS

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

# MDSIP_SANDBOX=1 runs the relay against the containerised mdsip instead of a
# bare one on the host -- the production topology, where a client's code
# execution lands inside the sandbox rather than on the origin. The relay is
# indifferent to which it is talking to, which is the point.
if [ -n "${MDSIP_SANDBOX:-}" ]; then
  export MDSIP_NAME="${MDSIP_NAME:-fdp-mdsip-relay}"
  export MDSIP_PUBLISH="127.0.0.1:$MDSIP_PORT"
  SANDBOXED=1
  cleanup_sandbox() { bash "$ROOT/scripts/mdsip-sandbox.sh" stop >/dev/null 2>&1 || true; }
  trap 'cleanup; cleanup_sandbox' EXIT
  bash "$ROOT/scripts/mdsip-sandbox.sh" start "$TREES"
  for _ in $(seq 1 150); do
    (exec 3<>/dev/tcp/127.0.0.1/"$MDSIP_PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
    sleep 0.1
  done
else
  # shellcheck source=../mdsip_helper.sh
  . "$ROOT/tests/mdsip_helper.sh"
  start_mdsip "$MDSIP_PORT" "$TREES" "$WORK"
fi

xrootd -c "$CFG" -l "$WORK/xrootd.log" >/dev/null 2>&1 & XRD_PID=$!
for _ in $(seq 1 150); do
  kill -0 "$XRD_PID" 2>/dev/null || fail "xrootd exited during startup"
  grep -q "initialization failed" "$WORK/xrootd.log" 2>/dev/null \
    && fail "xrootd reported an initialization failure"
  (exec 3<>/dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.1
done
(exec 3<>/dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null || fail "http port never opened"
{ exec 3<&- 3>&-; } 2>/dev/null || true   # braces: a bare `exec ... 2>/dev/null` redirects the WHOLE script's stderr

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

echo "--- 6. the C transport reaches the relay with no shim in between ---"
# The real client story: MDSplus loads libMdsIpFDP.so itself, because
# LibFindImageSymbol_C dlopens "lib" + "MdsIpFDP" + ".so" through the normal
# loader search. Nothing about the client is modified except the target string.
TRANSPORT="$ROOT/build/libMdsIpFDP.so"
if [ ! -f "$TRANSPORT" ]; then
  echo "SKIP: $TRANSPORT not built"
else
  LD_LIBRARY_PATH="$ROOT/build:${LD_LIBRARY_PATH:-}" \
  FDP_TUNNEL_SCHEME=http \
  RELAY_TARGET="fdp://127.0.0.1:$HTTP_PORT/mdsip" \
  RELAY_MDSIP_PORT="$MDSIP_PORT" \
    python "$ROOT/tests/integration/relay_client_check.py" \
    || fail "the C transport did not match the direct connection"

  echo "--- 7. transport lifecycle and error paths ---"
  # mdsip -m forks a process per connection, so a leaked relay session is a
  # leaked mdsip process. Counting them is the only check here that would
  # actually notice: the relay's own session cap is 256, so the churn below
  # would pass just as happily while leaking every one of them.
  count_mdsip() {
    if [ -n "${SANDBOXED:-}" ]; then
      podman exec "$MDSIP_NAME" sh -c 'ls -d /proc/[0-9]* | wc -l' 2>/dev/null || echo 0
    else
      # One process per connection, so no -m in the pattern. Matching the old
      # "mdsip -m -p" here would find nothing at all, and a counter stuck at 0
      # reports "no leak" for every possible outcome.
      pgrep -c -f "mdsip -p $MDSIP_PORT" 2>/dev/null || echo 0
    fi
  }

  # Positive control for the counter itself. Idle is now legitimately 0 children,
  # so "0 before, 0 after" is indistinguishable from a pattern that matches
  # nothing. Hold one session open and require the counter to see it.
  LD_LIBRARY_PATH="$ROOT/build:${LD_LIBRARY_PATH:-}" FDP_TUNNEL_SCHEME=http \
  python - "$HTTP_PORT" > "$WORK/holder.log" 2>&1 <<'PYEOF' &
import os, sys, time
from MDSplus import Connection
c = Connection("fdp://127.0.0.1:%s/mdsip" % sys.argv[1])
c.openTree(os.environ.get("RELAY_TREE", "efit01"),
           int(os.environ.get("RELAY_SHOT", "190000")))
print("holding", flush=True)
time.sleep(10)
PYEOF
  HOLDER=$!
  sleep 4
  HELD="$(count_mdsip)"
  grep -q holding "$WORK/holder.log" 2>/dev/null \
    || fail "leak-counter control never opened a session: $(tail -2 "$WORK/holder.log" 2>/dev/null)"
  kill "$HOLDER" 2>/dev/null || true
  wait "$HOLDER" 2>/dev/null || true
  sleep 2
  if [ "${SANDBOXED:-}" = "" ] && [ "$HELD" -lt 1 ]; then
    fail "session-leak counter is broken: saw $HELD processes while a session was open"
  fi
  echo "  leak counter sees $HELD process(es) with one session held"

  BEFORE="$(count_mdsip)"

  LD_LIBRARY_PATH="$ROOT/build:${LD_LIBRARY_PATH:-}" \
  FDP_TUNNEL_SCHEME=http \
  RELAY_HTTP_PORT="$HTTP_PORT" \
  RELAY_SHOTS="$RELAY_SHOTS" \
    python "$ROOT/tests/integration/transport_edge_cases.py" \
    || fail "transport edge cases failed"

  sleep 2   # let the relay notice the closes
  AFTER="$(count_mdsip)"
  echo "  mdsip processes: $BEFORE before, $AFTER after ~50 sessions"
  # A couple of stragglers are fine (teardown races the count); dozens are not.
  if [ "$AFTER" -gt "$((BEFORE + 5))" ]; then
    fail "sessions leaked: $BEFORE -> $AFTER after transport churn"
  fi
fi

echo "--- ALL PASS ---"
