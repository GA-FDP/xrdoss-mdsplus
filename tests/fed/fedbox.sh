#!/usr/bin/env bash
# Start/stop a local Pelican federation (director + registry + origin) in podman.
#
#   fedbox.sh start [<extra-xrootd.cfg>] [<plugin.so>]
#   fedbox.sh stop
#
# Serves $FEDBOX_DATA (default tests/fed/data) at federation prefix /test.
# Extra bind mounts may be passed via $FEDBOX_EXTRA_MOUNTS.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${FEDBOX_IMAGE:-hub.opensciencegrid.org/pelican_platform/origin:latest}"
NAME="fdp-fedbox"
DATA="${FEDBOX_DATA:-$ROOT/tests/fed/data}"
WEB_PORT="${FEDBOX_WEB_PORT:-8444}"

case "${1:-}" in
  stop)
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    echo "stopped"
    exit 0
    ;;
  start) ;;
  *) echo "usage: $0 {start|stop} [extra-xrootd.cfg] [plugin.so]"; exit 2 ;;
esac

# podman reads a relative bind-mount source as a *volume name*, so make these
# absolute or the failure is an inscrutable "names must match" error.
EXTRA_CFG="${2:-}"; [ -n "$EXTRA_CFG" ] && EXTRA_CFG="$(readlink -f "$EXTRA_CFG")"
PLUGIN_SO="${3:-}"; [ -n "$PLUGIN_SO" ] && PLUGIN_SO="$(readlink -f "$PLUGIN_SO")"

podman rm -f "$NAME" >/dev/null 2>&1 || true
mkdir -p "$DATA/tdi"    # nominal StoragePrefix for the virtual-file export

MOUNTS=(
  -v "$DATA:/data:z"
  -v "$ROOT/tests/fed/fed.yaml:/etc/pelican/config.d/10-fed.yaml:ro,z"
  # The registry module refuses to start without these, though the contents are
  # never used in a public test federation. Pelican's own fed_test_utils does
  # exactly the same thing.
  -v "$ROOT/tests/fed/oidc-client-id:/etc/pelican/oidc-client-id:ro,z"
  -v "$ROOT/tests/fed/oidc-client-secret:/etc/pelican/oidc-client-secret:ro,z"
)

# Xrootd.ConfigFile is always referenced by fed.yaml, so provide an empty file
# when the caller supplies none.
if [ -n "$EXTRA_CFG" ]; then
  MOUNTS+=(-v "$EXTRA_CFG:/etc/pelican/xrootd-extra.cfg:ro,z")
else
  : > /tmp/fdp-empty-xrootd.cfg
  MOUNTS+=(-v "/tmp/fdp-empty-xrootd.cfg:/etc/pelican/xrootd-extra.cfg:ro,z")
fi
[ -n "$PLUGIN_SO" ] && MOUNTS+=(-v "$PLUGIN_SO:/plugins/$(basename "$PLUGIN_SO"):ro,z")

# shellcheck disable=SC2206
[ -n "${FEDBOX_EXTRA_MOUNTS:-}" ] && MOUNTS+=($FEDBOX_EXTRA_MOUNTS)

podman run -d --name "$NAME" --network host "${MOUNTS[@]}" \
  --entrypoint /entrypoint.sh "$IMAGE" \
  pelican-server serve --module director --module registry --module origin -d \
  >/dev/null

echo -n "waiting for federation"
for _ in $(seq 1 120); do
  if curl -ks "https://localhost:$WEB_PORT/api/v1.0/health" >/dev/null 2>&1; then
    echo " up"
    echo "FEDBOX_URL=https://localhost:$WEB_PORT"
    exit 0
  fi
  # Fail fast if the container died rather than waiting out the full timeout.
  if ! podman container exists "$NAME" 2>/dev/null || \
     [ "$(podman inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" != "true" ]; then
    echo " CONTAINER EXITED"
    podman logs "$NAME" 2>&1 | tail -40
    exit 1
  fi
  echo -n "."; sleep 1
done

echo " FAILED"
podman logs "$NAME" 2>&1 | tail -40
exit 1
