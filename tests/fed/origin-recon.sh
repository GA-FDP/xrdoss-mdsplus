#!/usr/bin/env bash
# Read-only reconnaissance of the Pelican origin container.
#
# Answers: where its configuration comes from, whether Xrootd.ConfigFile is
# already claimed, whether our plugin's XRootD ABI matches, and whether there is
# an ext-handler slot left. Changes nothing.
#
#   bash origin-recon.sh [container-name]        # default: pelican-origin
#
# Paste the whole output back; it is all non-secret configuration metadata.
NAME="${1:-pelican-origin}"

say() { printf '\n=== %s ===\n' "$*"; }

# Output-or-fallback. Deliberately NOT `cmd | grep ... || echo`: the exit status
# of a pipeline is the LAST command's, so a `| head` or `| sort` on the end makes
# every pipeline "succeed" and the fallback never fires -- or fires wrongly.
report() {  # report "<fallback text>" "<shell pipeline>"
  local fallback="$1" out
  out="$(eval "$2" 2>/dev/null)"
  if [ -n "$out" ]; then printf '%s\n' "$out"; else printf '  %s\n' "$fallback"; fi
}

if ! podman container exists "$NAME" 2>/dev/null; then
  echo "'$NAME' is not visible to $(id -un)."
  echo "Rootless podman is per-user, so a container started by someone else"
  echo "does not appear here. Containers this user can see:"
  report "(none)" "podman ps -a --format '{{.Names}}  {{.Image}}  {{.Status}}'"
  echo "--- as root ---"
  report "(need: sudo podman ps -a)" "sudo -n podman ps -a --format '{{.Names}}  {{.Image}}  {{.Status}}'"
  echo
  echo "Re-run as whoever owns it, e.g.:  sudo bash $0 $NAME"
  exit 1
fi

RUNNING="$(podman inspect --format '{{.State.Status}}' "$NAME" 2>/dev/null)"

say "identity"
podman inspect --format 'name:    {{.Name}}
image:   {{.ImageName}}
state:   {{.State.Status}}
started: {{.State.StartedAt}}' "$NAME"
echo "seen by: $(id -un) (uid $(id -u))"

say "the original podman run, reproduced"
report "(unavailable)" \
  "podman inspect --format '{{range .Config.CreateCommand}}{{.}} {{end}}' '$NAME' | fold -s -w 100"

say "bind mounts -- config usually arrives through one of these"
report "(no bind mounts: config is in the image or in env)" \
  "podman inspect --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}  [{{.Mode}}] rw={{.RW}}{{\"\\n\"}}{{end}}' '$NAME'"

say "PELICAN_* / XROOTD_* environment"
report "(none, so configuration is file-based)" \
  "podman inspect --format '{{range .Config.Env}}{{.}}{{\"\\n\"}}{{end}}' '$NAME' | grep -iE '^(PELICAN|XROOTD|XRD)_'"

say "is Xrootd.ConfigFile already claimed? (Pelican allows only one)"
report "NOT set anywhere -- free for the relay fragment to use" \
  "{ podman inspect --format '{{range .Config.Env}}{{.}}{{\"\\n\"}}{{end}}' '$NAME' | grep -i configfile; \
     [ '$RUNNING' = running ] && podman exec '$NAME' sh -c 'grep -ris configfile /etc/pelican 2>/dev/null'; }"

say "config files inside the container"
report "(container not running -- start it, or read the mounts above)" \
  "[ '$RUNNING' = running ] && podman exec '$NAME' sh -c 'ls -la /etc/pelican/ /etc/pelican/config.d/ 2>&1'"

say "XRootD version in the image (the plugin is built against v5.9.2)"
report "(container not running)" \
  "[ '$RUNNING' = running ] && podman exec '$NAME' sh -c 'xrootd -v 2>&1 | head -1'"

# podman logs on a container running for days can take minutes, which reads
# as a hang. Bound it and read only the tail.
LOGS="$(timeout 30 podman logs --tail 5000 "$NAME" 2>&1 || true)"
say "ext handler slots -- XRootD allows 4 in total"
report "(none seen in the logs)" \
  "printf '%s\\n' \"$LOGS\" | grep -oE 'exthandlerlib [^ \"]*' | sort -u"
echo "  in use: $(printf '%s\\n' "$LOGS" | grep -c 'exthandlerlib') of 4"

say "namespaces currently exported"
report "(none found in the logs)" \
  "printf '%s\\n' \"$LOGS\" | grep -oE 'Path:/[a-zA-Z0-9_/.-]+' | sort -u | head -20"

say "is anything managing this container?"
report "(no matching user unit)" \
  "systemctl --user list-units --type=service --no-legend 2>/dev/null | grep -iE 'pelican|origin'"
report "(no matching system unit, or sudo unavailable)" \
  "sudo -n systemctl list-units --type=service --no-legend 2>/dev/null | grep -iE 'pelican|origin'"

printf '\n=== done -- nothing was modified ===\n'
