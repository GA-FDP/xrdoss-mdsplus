#!/usr/bin/env bash
# The client's contract suite, run against the real XRootD handler.
#
# The point of writing the wire contract down was that one suite could check
# both halves. GA-FDP/ptdata ships seven tests against a stub implementing it
# (cpp/tests/python/test_http_point_provider.py); this runs the same assertions
# against a container serving the real plugin, so a passing client suite and a
# passing server are the same claim.
#
#   tests/integration/test_point_endpoint.sh
#
# Requires: podman, an fdp-origin image (see Containerfile.origin), a
# ptdata checkout with the http_endpoint client, and the shot cache.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PTDATA="${PTDATA_CHECKOUT:-$ROOT/../ptdata}"
SHOTS="${PTDATA_TEST_SHOTS_DIR:-/cscratch/sammuli/ptdata_test_files}"
PORT="${PORT:-8080}"
NAME=point-e2e-$$
WORK="$(mktemp -d "${TMPDIR:-/tmp}/point-e2e.XXXXXX")"

cleanup() { podman rm -f "$NAME" >/dev/null 2>&1 || true; rm -rf "$WORK"; }
trap cleanup EXIT

test -f "$SHOTS/165920.MAG" || { echo "no shot cache at $SHOTS"; exit 77; }

# The versioned store, shaped exactly as the origin serves it (verified on
# d3d-origin 2026-09-07):
#
#   <root>/catalog/catalog_<stamp>/latest/<bucket:04d>.json   {"<shot>": <ver>}
#   <root>/views/shots/<bucket:04d>/<shot:06d>/v<N>/meta/index.json
#   <root>/views/shots/<bucket:04d>/<shot:06d>/v<N>/ptdata/<shot><EXT>
#
# Note there is no ext_location and no recorded path anywhere: the handler
# CONSTRUCTS the shotfile path from the version directory, which is why this
# fixture needs no URL prefix and no rewrite root.
STAMP=catalog_20260907T232802Z
VDIR="$WORK/store/views/shots/1659/165920/v1"
mkdir -p "$WORK/store/catalog/$STAMP/latest" "$VDIR/meta" "$VDIR/ptdata"
echo '{"165920": 1}' > "$WORK/store/catalog/$STAMP/latest/1659.json"
cat > "$VDIR/meta/index.json" <<JSON
{"shot":165920,"exts":[".MAG"],"pointname_ext":{"IP":".MAG"}}
JSON

# The shotfile is bind-mounted to its exact place in the version directory. A
# symlink would not do: it would resolve inside the container, where the shot
# cache is not mounted anywhere else.
podman run -d --name "$NAME" -p "127.0.0.1:$PORT:8080" --user 1000 \
    -v "$WORK/store:/fdp-d3d:ro,z" \
    -v "$SHOTS/165920.MAG:/fdp-d3d/views/shots/1659/165920/v1/ptdata/165920.MAG:ro" \
    -v "$HERE/point-endpoint.cfg:/etc/xrootd/point.cfg:ro,z" \
    --entrypoint '["xrootd","-c","/etc/xrootd/point.cfg"]' \
    xrdoss-mdsplus-build >/dev/null

for _ in $(seq 1 30); do
    curl -sf -o /dev/null "http://127.0.0.1:$PORT/fdp-d3d/ptdata/165920/IP" && break
    sleep 1
done

# The handler must actually have loaded. Without TLS, XRootD skips ext handlers
# that lack +notls and says nothing -- the symptom is XRootD answering 403 for
# the endpoint's paths, which reads like an authorization problem rather than a
# handler that is not there.
podman logs "$NAME" 2>&1 | grep -q "XrdHttpMdsip point endpoint" \
    || { echo "handler did not load"; podman logs "$NAME" 2>&1 | tail -20; exit 1; }

# The provenance headers. PtDataReader does not expose them, so the contract
# script below cannot check them -- and they have no other coverage anywhere.
# They are what lets a client detect a run that straddled a snapshot swap, so
# an endpoint that silently stopped emitting them would be a real regression.
HDRS="$(curl -sD- -o /dev/null "http://127.0.0.1:$PORT/fdp-d3d/ptdata/165920/IP")"
for h in "X-Ptdata-Extension: .MAG" \
         "X-Ptdata-Snapshot: $STAMP" \
         "X-Ptdata-Version: 1"; do
    printf '%s' "$HDRS" | grep -qi "^$h" \
        || { echo "missing or wrong header: $h"; printf '%s\n' "$HDRS"; exit 1; }
done
echo "PASS  provenance headers: extension, snapshot, version"

# The body must not have been truncated by a stray CRLF in that header block --
# XrdHttpProtocol appends the terminator itself, so a second one ends the block
# early and eats the response. A header-only check would not notice.
LEN="$(curl -s -o /dev/null -w '%{size_download}' \
        "http://127.0.0.1:$PORT/fdp-d3d/ptdata/165920/IP")"
[ "$LEN" -gt 0 ] || { echo "empty body: the header block truncated it"; exit 1; }
echo "PASS  body survives the header block ($LEN bytes)"

PTDATA_CHECKOUT="$PTDATA" \
ENDPOINT="http://127.0.0.1:$PORT/fdp-d3d" \
SHOTS_DIR="$SHOTS" \
    "$PTDATA/../ptdata/.pixi/envs/default/bin/python" "$HERE/point_contract.py" \
    || pixi run --manifest-path "$PTDATA/pixi.toml" python "$HERE/point_contract.py"

# A half-configured endpoint must REFUSE to load rather than answer 404 for
# everything. This is the only automated check of that logic, and it is the
# logic that takes fdp:// down with it when it is wrong.
podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" -p "127.0.0.1:$PORT:8080" --user 1000 \
    -v "$WORK/store:/fdp-d3d:ro,z" \
    -v "$HERE/point-endpoint-refuse.cfg:/etc/xrootd/point.cfg:ro,z" \
    --entrypoint '["xrootd","-c","/etc/xrootd/point.cfg"]' \
    xrdoss-mdsplus-build >/dev/null
sleep 3
if podman logs "$NAME" 2>&1 | grep -q "refusing to load"; then
    echo "PASS  a half-configured endpoint refuses to load"
else
    echo "FAIL  no refusal logged -- the handler loaded with no store root,"
    echo "      so every point would 404 and read as absent data"
    podman logs "$NAME" 2>&1 | tail -20
    exit 1
fi
