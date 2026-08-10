#!/usr/bin/env bash
# Run the mdsip sandbox.
#
#   mdsip-sandbox.sh start [<tree-dir>]
#   mdsip-sandbox.sh stop
#   mdsip-sandbox.sh status
#
# A client of this server has arbitrary native code execution inside it --
# measured, see docs/security.md. Every flag below exists to bound what that
# code can reach. Dropping any of them silently removes a control, so they are
# grouped and commented rather than golfed into one line.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

NAME="${MDSIP_NAME:-fdp-mdsip}"
IMAGE="${MDSIP_IMAGE:-localhost/fdp-mdsip:latest}"
NETWORK="${MDSIP_NETWORK:-fdp-mdsip-net}"
TREES="${2:-${FDP_TREES:-/tmp/fdp-trees}}"

# Published only on loopback by default. The production deployment should
# instead put the origin and mdsip on the internal network and publish nothing.
PUBLISH="${MDSIP_PUBLISH:-127.0.0.1:8000}"

MEMORY="${MDSIP_MEMORY:-2g}"
CPUS="${MDSIP_CPUS:-2}"
PIDS="${MDSIP_PIDS:-256}"

# podman derives several paths from /run/user/$UID, which does not survive the
# login session that created it. See docs/deployment-notes.md.
if [ ! -d "/run/user/$(id -u)" ]; then
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-$(id -u)}"
  mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
fi

case "${1:-}" in
  stop)
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    echo "stopped"
    exit 0
    ;;
  status)
    podman ps --filter "name=$NAME" --format '{{.Names}} {{.Status}} {{.Ports}}'
    exit 0
    ;;
  start) ;;
  *) echo "usage: $0 {start|stop|status} [tree-dir]"; exit 2 ;;
esac

[ -d "$TREES" ] || { echo "no tree directory at $TREES"; exit 1; }
TREES="$(readlink -f "$TREES")"

# podman ACCEPTS --pids-limit/--memory/--cpus on a cgroups v1 rootless host and
# then ignores them, announcing it in one line among its startup noise. A
# silently absent denial-of-service control is worse than an absent one, so
# refuse rather than pretend. Rootful podman, or a cgroups v2 host, enforces
# them normally.
CGROUPS="$(podman info --format '{{.Host.CgroupsVersion}}' 2>/dev/null || echo unknown)"
ROOTLESS="$(podman info --format '{{.Host.Security.Rootless}}' 2>/dev/null || echo unknown)"
if [ "$CGROUPS" = "v1" ] && [ "$ROOTLESS" = "true" ]; then
  if [ -z "${MDSIP_ALLOW_NO_LIMITS:-}" ]; then
    cat >&2 <<EOF
REFUSING TO START: this host is cgroups v1 + rootless, where podman silently
ignores --pids-limit, --memory and --cpus. The sandbox would start with no
resource limits at all, and a client can call fork() -- see docs/security.md.

Fix it (best first):
  * run on a cgroups v2 host
  * run podman rootful
  * accept the risk deliberately:  MDSIP_ALLOW_NO_LIMITS=1 $0 start
EOF
    exit 1
  fi
  echo "WARNING: cgroups v1 rootless -- resource limits are NOT enforced" >&2
fi

# An "internal" podman network has no route off the host. This is the control
# that turns code execution from a pivot into a dead end -- without it, a
# client can reach anything the origin host can reach, including the rest of
# the internal network and the internet.
if ! podman network exists "$NETWORK" 2>/dev/null; then
  podman network create --internal "$NETWORK" >/dev/null
fi

podman rm -f "$NAME" >/dev/null 2>&1 || true

podman run -d --name "$NAME" \
  --network "$NETWORK" \
  -p "$PUBLISH:8000" \
  \
  `# --- filesystem: nothing writable that survives, nothing readable that matters ---` \
  --read-only \
  --tmpfs /tmp:rw,noexec,nosuid,nodev,size=64m \
  -v "$TREES:/trees:ro" \
  \
  `# On RHEL-family hosts podman mounts the HOST's subscription credentials at` \
  `# /run/secrets by default -- entitlement certs and rhsm config that nobody` \
  `# asked for and a client with code execution can read. Mask it with an empty` \
  `# tmpfs. Found by tests/security/verify_sandbox.py, not by reading docs.` \
  --tmpfs /run/secrets:rw,noexec,nosuid,nodev,size=1m \
  \
  `# --- privilege: no capabilities at all, and no way to regain any ---` \
  --cap-drop=ALL \
  --security-opt=no-new-privileges \
  \
  `# --- resources: bound a fork bomb and a memory hog ---` \
  --pids-limit "$PIDS" \
  --memory "$MEMORY" \
  --cpus "$CPUS" \
  \
  `# Trees are read-only above; this tells MDSplus where they are. The path is` \
  `# a template per tree name, matching the origin plugin's treepath.` \
  -e "efit01_path=/trees" \
  -e "default_tree_path=/trees" \
  \
  "$IMAGE" >/dev/null

echo "started $NAME on $PUBLISH -> 8000 (trees $TREES, read-only)"
echo "verify with: bash tests/security/verify_sandbox.sh"
