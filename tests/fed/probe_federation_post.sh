#!/usr/bin/env bash
# Does a POST route through the Pelican director the way a GET does?
#
# This is the open question the relay's deployment story depends on. If POST
# routes, clients use a federation URL and get the director's origin selection.
# If it does not, clients must address an origin directly and that selection is
# lost -- worth knowing before publishing a URL either way.
#
# Deliberately curl, not the transport: this isolates the routing question from
# anything our client code does, and curl reports the redirect chain plainly.
#
#   pixi run bash tests/fed/probe_federation_post.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEB=${FEDBOX_URL:-https://localhost:8444}
MDSIP_PORT=${FED_MDSIP_PORT:-8000}
WORK=${FED_WORK:-/tmp/fdp-fed-post}
RELAY_FILE="$ROOT/build/libXrdHttpMdsip-5.so"
OSS_FILE="$ROOT/build/libXrdOssMdsplus-5.so"

[ -f "$RELAY_FILE" ] || { echo "FAIL: build first (pixi run build)"; exit 1; }

if ! podman image exists "${FEDBOX_IMAGE:-localhost/fdp-origin-mdsplus:latest}" 2>/dev/null; then
  echo "FAIL: build the origin image first:"
  echo "      podman build -f Containerfile.runtime -t fdp-origin-mdsplus ."
  exit 1
fi

MDSIP_PID=""
cleanup() {
  [ -n "$MDSIP_PID" ] && kill "$MDSIP_PID" 2>/dev/null || true
  bash "$ROOT/tests/fed/fedbox.sh" stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

rm -rf "$WORK"; mkdir -p "$WORK"

# shellcheck source=../mdsip_helper.sh
. "$ROOT/tests/mdsip_helper.sh"
start_mdsip "$MDSIP_PORT" "${FDP_TREES:-/tmp/fdp-trees}" "$WORK"

FEDBOX_EXTRA_MOUNTS="-v ${FDP_TREES:-/tmp/fdp-trees}:/trees:ro,z -v $OSS_FILE:/plugins/$(basename "$OSS_FILE"):ro,z" \
bash "$ROOT/tests/fed/fedbox.sh" start \
    "$ROOT/tests/fed/xrootd-relay.cfg" "$RELAY_FILE" >/dev/null \
  || { echo "FAIL: federation did not start"; exit 1; }

echo
echo "=== did the ext handler load inside the federation origin? ==="
# Dumped to a file first: `podman logs | grep -q` reports a FALSE NEGATIVE under
# pipefail, because grep -q closes the pipe on its first match and podman exits
# on SIGPIPE.
podman logs fdp-fedbox > "$WORK/fedbox.log" 2>&1 || true
if grep -q "XrdHttpMdsip relay" "$WORK/fedbox.log"; then
  grep "XrdHttpMdsip relay" "$WORK/fedbox.log" | tail -1 | sed 's/.*msg=//'
else
  echo "NOT LOADED -- everything below is meaningless. XRootD log:"
  grep -iE "exthandler|mdsip|Config warning|unable" "$WORK/fedbox.log" | tail -20
  exit 1
fi

# Find the origin's own XRootD HTTPS port: the baseline for "the relay works
# when addressed directly" has to come from somewhere real, not an assumption.
ORIGIN_PORT=""
for p in 8443 8447 8448 1094; do
  code=$(curl -ks -o /dev/null -w '%{http_code}' -X POST --data-binary '' \
         "https://localhost:$p/mdsip/nonsense" 2>/dev/null || true)
  # 404 means our handler answered; anything else is some other listener.
  if [ "$code" = "404" ]; then ORIGIN_PORT="$p"; break; fi
done
echo
echo "=== origin XRootD port carrying the relay: ${ORIGIN_PORT:-NOT FOUND} ==="
[ -n "$ORIGIN_PORT" ] || { echo "could not locate it; listeners:"; ss -ltnp 2>/dev/null | head -20; }

probe() {  # probe <label> <url>
  local label="$1" url="$2"
  local out
  out=$(curl -ks -o /dev/null -X POST --data-binary '' \
        -w 'code=%{http_code} redirects=%{num_redirects} final=%{url_effective}' \
        "$url" 2>&1)
  printf '  %-46s %s\n' "$label" "$out"
}

probe_follow() {  # follow redirects, preserving method (curl does for 307/308)
  local label="$1" url="$2"
  local out
  out=$(curl -ksL -o /dev/null -X POST --data-binary '' \
        -w 'code=%{http_code} redirects=%{num_redirects} final=%{url_effective}' \
        "$url" 2>&1)
  printf '  %-46s %s\n' "$label" "$out"
}

echo
echo "=== BASELINE: POST straight at the origin ==="
if [ -n "$ORIGIN_PORT" ]; then
  probe "origin /mdsip/nonsense (expect 404)" "https://localhost:$ORIGIN_PORT/mdsip/nonsense"
  probe "origin /mdsip/connect  (expect 200)" "https://localhost:$ORIGIN_PORT/mdsip/connect"
fi

echo
echo "=== THROUGH THE DIRECTOR ==="
probe        "director /mdsip/connect (no follow)" "$WEB/mdsip/connect"
probe_follow "director /mdsip/connect (follow)   " "$WEB/mdsip/connect"
probe        "director api origin/mdsip/connect  " "$WEB/api/v1.0/director/origin/mdsip/connect"
probe_follow "director api origin (follow)       " "$WEB/api/v1.0/director/origin/mdsip/connect"

echo
echo "=== is PUT routable? (the director's CORS list allows GET, PUT, OPTIONS, PROPFIND) ==="
# If PUT routes with a body preserved, the relay could use it instead of POST
# and keep federation-level origin selection. Worth knowing before concluding
# that clients must address an origin directly.
putprobe() {
  local label="$1" url="$2" extra="${3:-}"
  local out
  # shellcheck disable=SC2086
  out=$(curl -ks -o /dev/null -X PUT --data-binary 'probe' $extra \
        -w 'code=%{http_code} redirects=%{num_redirects} final=%{url_effective}' \
        "$url" 2>&1)
  printf '  %-46s %s\n' "$label" "$out"
}
putprobe "director PUT /mdsip/connect (no follow)" "$WEB/mdsip/connect"
putprobe "director PUT /mdsip/connect (follow)   " "$WEB/mdsip/connect" "-L"
putprobe "director PUT /test/probe.txt (control) " "$WEB/test/probe.txt"

echo
echo "=== control: does a GET to the same namespace route? ==="
GETOUT=$(curl -ks -o /dev/null -X GET \
         -w 'code=%{http_code} redirects=%{num_redirects}' "$WEB/mdsip/connect" 2>&1)
echo "  GET director /mdsip/connect                    $GETOUT"

echo
echo "=== what the director actually says (headers) ==="
curl -ks -D - -o /dev/null -X POST --data-binary '' "$WEB/mdsip/connect" 2>&1 \
  | head -20

echo
echo "=== a full PUT session THROUGH THE DIRECTOR ==="
# The decisive test. If this works, clients can use a federation URL and keep
# the director's origin selection; if not, they must address an origin directly.
FTOKEN=$(curl -ksL -X PUT --data-binary '' "$WEB/mdsip/connect" 2>/dev/null)
if [ -n "$FTOKEN" ] && [ ${#FTOKEN} -eq 32 ]; then
  echo "  connect via director -> session ${FTOKEN:0:16}... (len ${#FTOKEN})"
  CODE=$(curl -ksL -o /dev/null -w '%{http_code}' -X PUT \
         -H "X-Fdp-Session: $FTOKEN" --data-binary 'junk' "$WEB/mdsip/msg")
  echo "  /msg through director with a junk body (expect 502): $CODE"
  curl -ksL -o /dev/null -X PUT -H "X-Fdp-Session: $FTOKEN" "$WEB/mdsip/close"
  echo "  closed through the director"
  echo "  => THE FEDERATION PATH WORKS over PUT"
else
  echo "  connect via director returned: '${FTOKEN:0:60}'"
  echo "  => federation routing still blocked; clients must address an origin"
fi

echo
echo "=== AUTHORIZATION: does the ext handler bypass it? ==="
# XrdHttpReq.cc dispatches ext handlers at reqstate == 0, BEFORE the file-access
# path where ofs.authorize and SciTokens checks live. If that means an
# unauthenticated client can open a session, the sandbox is the only thing
# between anyone on the network and arbitrary code execution.
if [ -n "$ORIGIN_PORT" ]; then
  NOAUTH=$(curl -ks -o /dev/null -w '%{http_code}' -X PUT --data-binary '' \
           "https://localhost:$ORIGIN_PORT/mdsip/connect")
  JUNK=$(curl -ks -o /dev/null -w '%{http_code}' -X PUT --data-binary '' \
         -H 'Authorization: Bearer not-a-real-token' \
         "https://localhost:$ORIGIN_PORT/mdsip/connect")
  # A normal object write to a namespace we do NOT claim, for contrast.
  NORMAL=$(curl -ks -o /dev/null -w '%{http_code}' -X PUT --data-binary 'x' \
           "https://localhost:$ORIGIN_PORT/tdi/should-be-denied")
  echo "  PUT /mdsip/connect, no credentials at all : $NOAUTH"
  echo "  PUT /mdsip/connect, garbage bearer token  : $JUNK"
  echo "  PUT /tdi/... (not claimed by us), no creds: $NORMAL   <- contrast"
  if [ "$NOAUTH" = "200" ]; then
    echo "  => THE RELAY IS UNAUTHENTICATED. The ext handler runs before authz."
  fi
fi

echo
echo "=== a stock MDSplus.Connection through the DIRECTOR ==="
# The end of the line: an unmodified client, a federation URL, and real data.
if [ -f "$ROOT/build/libMdsIpFDP.so" ]; then
  DIRECTOR_HOSTPORT="${WEB#https://}"
  LD_LIBRARY_PATH="$ROOT/build:${LD_LIBRARY_PATH:-}" \
  FDP_TUNNEL_INSECURE=1 \
  FED_TARGET="fdp://$DIRECTOR_HOSTPORT/mdsip" \
    python "$ROOT/tests/fed/fed_client_check.py" || echo "  (see above)"
else
  echo "  SKIP: build/libMdsIpFDP.so not built"
fi

echo
echo "=== a full session directly against the origin ==="
if [ -n "$ORIGIN_PORT" ]; then
  TOKEN=$(curl -ks -X POST --data-binary '' "https://localhost:$ORIGIN_PORT/mdsip/connect")
  if [ -n "$TOKEN" ]; then
    echo "  session token: ${TOKEN:0:16}... (len ${#TOKEN})"
    CODE=$(curl -ks -o /dev/null -w '%{http_code}' -X POST \
           -H "X-Fdp-Session: $TOKEN" --data-binary 'x' \
           "https://localhost:$ORIGIN_PORT/mdsip/msg")
    echo "  /msg with a junk body (expect 502): $CODE"
    curl -ks -o /dev/null -X POST -H "X-Fdp-Session: $TOKEN" \
         "https://localhost:$ORIGIN_PORT/mdsip/close"
    echo "  closed"
  else
    echo "  /connect returned nothing"
  fi
fi
