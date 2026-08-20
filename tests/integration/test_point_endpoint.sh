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
# Requires: podman, an xrdoss-mdsplus-build image (see Containerfile.build), a
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

# One index entry, recording an absolute pelican:// URL exactly as the
# production indexer does -- so the handler's prefix rewrite is what makes this
# resolve, not a convenient local path.
mkdir -p "$WORK/idx/1659"
cat > "$WORK/idx/1659/165920.json" <<JSON
{"shot":165920,
 "ext_location":{".MAG":"pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/165920.MAG"},
 "pointname_ext":{"IP":".MAG"}}
JSON

podman run -d --name "$NAME" -p "127.0.0.1:$PORT:8080" --user 1000 \
    -v "$WORK/idx:/ptdata-index:ro,z" \
    -v "$SHOTS:/fdp-archives/ptdata:ro" \
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

PTDATA_CHECKOUT="$PTDATA" \
ENDPOINT="http://127.0.0.1:$PORT/fdp-d3d" \
SHOTS_DIR="$SHOTS" \
    "$PTDATA/../ptdata/.pixi/envs/default/bin/python" "$HERE/point_contract.py" \
    || pixi run --manifest-path "$PTDATA/pixi.toml" python "$HERE/point_contract.py"
