#!/usr/bin/env bash
# PTData end to end: through mdsip, in the sandbox, over a mounted fixture.
#
#   bash tests/integration/test_ptdata_tdi.sh
#
# The unit-level checks (tests/integration/check_tdi_wrappers.py) run
# in-process, where MDS_PATH and the working directory are whatever the shell
# had. This one goes through socat and mdsip, so it is the only check that
# covers `cd /`, the image's environment, and the container's MDS_PATH.
#
# It exercises three things the direct calls cannot:
#
#   * a STORED RECORD, evaluated by the server when a client reads an ordinary
#     node -- the failure that returns %TDI-E-UNKNOWN_VAR on the deployed
#     origin today, and the reason this whole effort exists
#   * the legacy PTNPTS shim, reaching our PTHEAD2 through the site library
#   * SYS_D3 set to a literal Pelican URL, the form the index records, which
#     resolves only through the image's symlink chain with cwd at /
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${MDSIP_IMAGE:-localhost/fdp-mdsip:latest}"
NAME="${MDSIP_NAME:-fdp-mdsip-ptdata-e2e}"
PORT="${MDSIP_PORT:-8105}"
WORK="${TMPDIR:-/tmp}/fdp-ptdata-e2e.$$"

# podman derives paths from /run/user/$UID, which does not survive the login
# session that created it. Assign unconditionally: the variable is typically
# still SET to a directory that no longer exists, and `:-` would preserve it.
if [ ! -d "/run/user/$(id -u)" ]; then
    export XDG_RUNTIME_DIR="/tmp/xdg-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR/libpod/tmp" && chmod 700 "$XDG_RUNTIME_DIR"
fi

cleanup() {
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/sysd3" "$WORK/trees"
python "$ROOT/tests/make_ptdata_fixture.py" "$WORK/sysd3" >/dev/null
python "$ROOT/tests/make_ptdata_tree.py" "$WORK/trees" IP 99001 >/dev/null

podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" -p "127.0.0.1:$PORT:8000" \
    -v "$WORK/sysd3:/fdp-archives/archives/ptdata:ro" \
    -v "$WORK/trees:/trees:ro" \
    `# The literal Pelican-URL form the index records: relative, doubled` \
    `# slash, resolved through the image's chain with cwd at /.` \
    -e "SYS_D3=pelican://osg-htc.org:443/fdp-d3d/archives/ptdata" \
    -e SYS_D3_DELIM=';' -e PTDATA_VAX_FLOATS=0 -e fdpt_path=/trees \
    "$IMAGE" >/dev/null

# Poll rather than sleep: mdsip is ready when it accepts a connection.
for _ in $(seq 1 30); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then break; fi
    sleep 0.5
done

python - "$PORT" <<'EOF'
import sys
import numpy as np
import MDSplus

port = sys.argv[1]
c = MDSplus.Connection(f"127.0.0.1:{port}")
failures = []

def check(label, got, want):
    ok = np.array_equal(np.asarray(got, dtype=float), np.asarray(want, dtype=float))
    print(f"  {'ok  ' if ok else 'FAIL'} {label}: {got}")
    if not ok:
        failures.append(f"{label}: expected {want}, got {got}")

# Client-sent expressions: the server evaluates a string we handed it.
check("PTDATA2 direct", c.get('PTDATA2("IP", 99001, 0)').data(), [10, 20, 30, 40, 50])
check("PTDATA2 second point", c.get('PTDATA2("BT", 99001, 0)').data(), [-7, -14, -21])
check("PTNPTS (site shim)", c.get('PTNPTS("IP", 99001)').data(), 5)
check("absent pointname", c.get('PTDATA2("NOPE", 99001, 0)').data(), [0])

# Stored records: the server evaluates an expression baked into the tree. This
# is the path that fails on the deployed origin, and the reason for all of it.
c.openTree("fdpt", 99001)
check("stored PLAIN (control)", c.get(r"\FDPT::TOP:PLAIN").data(), 42)
check("stored PTNPTS record", c.get(r"\FDPT::TOP:PTNPTS_EMB").data(), 5)
check("stored PTDATA2 record", c.get(r"\FDPT::TOP:PTDATA_EMB").data(),
      [10, 20, 30, 40, 50])

# Units live on the server side: Connection.get returns the evaluated array,
# so a client-side .dim_of() would be meaningless here.
units = str(c.get('UNITS(DIM_OF(PTDATA2("IP", 99001, 0)))').data())
print(f"  {'ok  ' if units == 'ms' else 'FAIL'} dim units: {units!r}")
if units != "ms":
    failures.append(f"dim units: expected 'ms', got {units!r}")

try:
    c.get('PTDATA2("IP", 99001, 3)')
    failures.append("unsupported ical did not raise -- a calibration was substituted")
    print("  FAIL unsupported ical did not raise")
except Exception as exc:  # noqa: BLE001 -- raising is the pass condition
    print(f"  ok   unsupported ical raises ({type(exc).__name__})")

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(f"{len(failures)} failure(s)")
print("ok: ptdata works end to end through the sandbox")
EOF
