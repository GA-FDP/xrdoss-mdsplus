#!/usr/bin/env bash
# Start the mdsip sandbox, attack it, tear it down.
#
#   bash tests/security/verify_sandbox.sh
#
# Fails if any containment control is missing. This is the script that decides
# whether the sandbox is real, so it deliberately runs the checks through
# ordinary TDI -- the same channel a client has -- rather than inspecting
# podman's own view of the configuration, which would only prove that the flags
# were typed correctly.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NAME="${MDSIP_NAME:-fdp-mdsip-verify}"
PORT="${MDSIP_VERIFY_PORT:-8201}"
TREES="${FDP_TREES:-/tmp/fdp-trees}"

export MDSIP_NAME="$NAME"
export MDSIP_PUBLISH="127.0.0.1:$PORT"
export MDSIP_NETWORK="${MDSIP_NETWORK:-fdp-mdsip-verify-net}"

if [ ! -d "/run/user/$(id -u)" ]; then
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-$(id -u)}"
  mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
fi

if ! podman image exists "${MDSIP_IMAGE:-localhost/fdp-mdsip:latest}" 2>/dev/null; then
  echo "build the image first:  podman build -f Containerfile.mdsip -t fdp-mdsip ."
  exit 1
fi
[ -d "$TREES" ] || { echo "SKIP: no trees at $TREES"; exit 0; }

cleanup() { bash "$ROOT/scripts/mdsip-sandbox.sh" stop >/dev/null 2>&1 || true; }
trap cleanup EXIT

# Record the host's container stack in the output. The network controls in
# particular are properties of the *backend*, not of the flags: podman 5.0
# removed CNI, so a result obtained under CNI says nothing about a netavark
# host. A run whose output does not say where it ran is not evidence.
echo "Container stack:"
podman info --format '  podman {{.Version.Version}}, network backend {{.Host.NetworkBackend}}, cgroups {{.Host.CgroupsVersion}}, rootless {{.Host.Security.Rootless}}' 2>/dev/null \
  || echo "  (podman info unavailable)"
echo

bash "$ROOT/scripts/mdsip-sandbox.sh" start "$TREES"

for _ in $(seq 1 150); do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.1
done
(exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null || {
  echo "FAIL: sandbox never listened on $PORT"
  podman logs "$NAME" 2>&1 | tail -20
  exit 1
}
exec 3<&- 3>&- 2>/dev/null || true

# Propagated so the verifier can tell "control missing" from "control this host
# cannot enforce, waived on purpose". Unset, the limit checks are hard failures.
MDSIP_ALLOW_NO_LIMITS="${MDSIP_ALLOW_NO_LIMITS:-}" \
  python "$ROOT/tests/security/verify_sandbox.py" "127.0.0.1:$PORT"
