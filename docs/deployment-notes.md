# Deployment and environment notes

Things that cost time to discover and would cost it again.

## The origin image needs the MDSplus runtime

`ConnectToMds` calls `LoadIo("tcp")`, which **`dlopen`s `libMdsIpTCP.so` by
name** (`mdstcpip/mdsipshr/LoadIo.c:54-62`). No amount of static linking removes
that: a library discovered by filename cannot be linked in. When it is missing,
`LoadIo` silently falls back to `tunnel_routines` and the only symptom is
`ConnectToMds(...) failed`.

`Containerfile.runtime` builds a stock Pelican origin plus `mdsplus-kernel_bin`,
which ships the transport. MDSplus installs to `/usr/local/mdsplus/lib64`, which
is not on the default loader path, so the image also drops an
`/etc/ld.so.conf.d/mdsplus.conf` and runs `ldconfig`.

## mdsip run unprivileged needs `* | SELF`

`MAP_TO_LOCAL` makes mdsip attempt a setuid to the **client's** username. When
the client is XRootD inside the Pelican container that username is `xrootd`,
which does not exist on the host, and the connection is refused with **no
server-side log at all**.

`* | SELF` tells mdsip not to switch user (`CheckClient.c:76`). A local test
where the client happens to run as the same user as the server passes with
`MAP_TO_LOCAL` and hides this completely.

## `MDS_PATH` must include `tdi/remote`

`GetManyExecute` is a TDI function (`tdi/remote/GetManyExecute.fun`), not a
builtin. Without that path every request fails with `%TDI-E-UNKNOWN_VAR`, which
reads like a client bug and is not one.

## podman when `/run/user/$UID` is gone

On a host without a live systemd user session — after the login that created
`/run/user/$UID` ends, and `/run/user` is root-owned so it cannot be recreated —
podman fails with:

```
Error: creating events dirs: mkdir /run/user/1122: permission denied
```

`XDG_RUNTIME_DIR` alone does **not** fix it; several paths are derived
independently. What works is a `~/.config/containers/containers.conf`:

```toml
[engine]
events_logger = "file"
events_logfile_path = "/tmp/containers-user-<uid>/tmp/events.log"
tmp_dir  = "/tmp/containers-user-<uid>/tmp"
static_dir = "/local-scratch/<user>/containers/storage/libpod"
volume_path = "/local-scratch/<user>/containers/storage/volumes"
```

plus `XDG_RUNTIME_DIR` pointed at a writable 0700 directory for the rootless
pause process. `tests/fed/fedbox.sh`, `scripts/mdsip-sandbox.sh` and
`tests/security/verify_sandbox.sh` set the latter automatically when
`/run/user/$UID` is absent.

**Create `$XDG_RUNTIME_DIR/libpod/tmp` as well.** podman creates that itself
under a runtime directory it set up, but *not* under one handed to it, and the
failure names neither the variable nor the directory:

```
error creating temporary file: No such file or directory
ERRO invalid internal status, try resetting the pause process with
     "podman system migrate": setting up the process:
     open /tmp/xdg-1122/libpod/tmp/pause.pid: no such file or directory
```

`podman system migrate`, which the message recommends, does **not** fix it —
`mkdir -p "$XDG_RUNTIME_DIR/libpod/tmp"` does. Counter-intuitively, leaving
`XDG_RUNTIME_DIR` *unset* also works once the `containers.conf` above is in
place, because podman then picks and creates its own runtime directory. Setting
it to a hand-made path is what introduces the requirement.

Both this and the `containers.conf` paths live under `/tmp`, which is cleaned
periodically on omega06 — so these directories vanish and the symptom returns
without anything having changed. The same cleaning removes the container
storage under `/local-scratch`, which shows up separately as every image being
unrunnable (`/bin/sh: no such file or directory`) and needs
`podman system reset` plus a rebuild.

**Do not also override the OCI runtime.** Setting `runtime = "runc"` with an
`[engine.runtimes]` table produced `default OCI runtime "runc" not found:
invalid argument`, which masks the real errors and looks like a different
problem entirely. The default runtime is correct; only the paths need changing.

The durable fix is `loginctl enable-linger <user>`, which needs root.


## Deploying the plugins onto the origin

Three artifacts, three different destinations, and only one of them needs to
touch the origin at all to get the tunnel working.

| Artifact | Goes | Needs |
|---|---|---|
| `libXrdHttpMdsip-5.so` (relay) | into the origin container | **a stock Pelican origin** — nothing else |
| `libXrdOssMdsplus-5.so` (virtual files) | into the origin container | an image with the **MDSplus runtime** (`Containerfile.runtime`) |
| `libMdsIpFDP.so` (client transport) | onto **clients**, not the origin | packaging on `ga-fdp` |

**The relay needs no custom image.** It links `libXrdUtils`, `libXrdHttpUtils`
and libc, and nothing else — verified by loading it into the unmodified
`pelican_platform/origin:latest`, which contains no MDSplus whatsoever
(`/usr/local/mdsplus` absent, zero `libMdsIp*` in `ldconfig`), and running a
full session through it. The Oss plugin is the one that forces a derived image,
because `ConnectToMds` `dlopen`s `libMdsIpTCP.so` by name at runtime.

So the relay can go first, as a bind mount and a config fragment, with the Oss
plugin deferred to whenever an image rebuild is convenient.

### Finding where the origin's config actually lives

When the origin is started by a hand-rolled `podman run`, the configuration is
in one of two places and `podman inspect` shows both. It also reproduces the
original command, which is what has to be edited:

```bash
podman ps --format '{{.Names}} {{.Image}}'
podman inspect --format '{{range .Config.CreateCommand}}{{.}} {{end}}' <name>
podman inspect --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{"\n"}}{{end}}' <name>
podman inspect --format '{{range .Config.Env}}{{.}}{{"\n"}}{{end}}' <name> | grep -i pelican
```

Pelican reads, in increasing precedence: `/etc/pelican/pelican.yaml`, then
drop-ins `/etc/pelican/config.d/*.yaml`, then `PELICAN_<SECTION>_<KEY>`
environment variables, then command-line flags. So the settings are either in a
bind-mounted YAML (visible in `Mounts`) or in `PELICAN_*` variables (visible in
`Env`) — one of those two will have them.

**Prefer a drop-in.** Adding `/etc/pelican/config.d/50-mdsip.yaml` composes with
whatever is already there instead of editing it, and is exactly what the
federation test does (`tests/fed/fedbox.sh` mounts `fed.yaml` as
`config.d/10-fed.yaml`), so the pattern is proven.

**Check whether `Xrootd.ConfigFile` is already set** before adding it. Pelican
takes one, so if something already uses it the fragment has to merge with that
file rather than claim the setting.

### The d3d-origin case, concretely

Its `podman run` mounts the whole config directory from the host:

```
-v /var/pelican/config:/etc/pelican          # so /etc/pelican/X is /var/pelican/config/X
-v /mnt/beegfs/data:/fdp-d3d/                # the archive, mounted READ-WRITE
--restart always --replace
serve -f https://osdf-director.osg-htc.org   # the PRODUCTION OSDF director
```

Two consequences worth having up front.

**Config needs no image or mount change.** `/etc/pelican` is already a bind
mount of `/var/pelican/config`, and `Xrootd.ConfigFile` is already set to
`/etc/pelican/xrootd.cfg` — so the relay line is *appended* to
`/var/pelican/config/xrootd.cfg` on the host. There is no `config.d/` in use and
none is needed. The plugin `.so` can be delivered the same way, by dropping it
in `/var/pelican/config/`, if changing the `podman run` is undesirable; a
dedicated `-v /var/pelican/plugins:/plugins:ro` is tidier but means recreating
the container.

**It is federated with the production OSDF director, not a test one.** So do
NOT register a `/mdsip` namespace for the first deployment: that would mean a
writable namespace in the real federation (the director only routes `PUT`, and
only to namespaces with the `Writes` capability). Point clients at the origin
directly instead —

```
MDSplus.Connection('fdp://d3d-origin.gat.com:8443/mdsip')
```

— which needs no namespace at all, because the ext handler claims the path
before XRootD consults any namespace. Port 8443 is already published. What this
gives up is director-based origin selection, which is worth close to nothing
here: relay sessions are sticky to one origin regardless, so the director could
only ever choose which origin a session starts on.

### Getting the relay to the sandbox: the hop that needs thought

The origin container publishes ports (`-p 8000:8000 -p 8443:8443`) rather than
using host networking, so it is on a podman bridge. Two consequences:

**`localhost` in the relay config is wrong.** Inside the origin container that
is the container itself, not the host — measured. The relay must address the
host, via `host.containers.internal` or an explicit address.

**Port 8000 is already taken** by the origin, so the sandbox cannot use its
default. Pick another (8100 below).

The security requirement makes this more than a plumbing detail: **mdsip must be
reachable by the origin container and by nothing else.** A client who can reach
mdsip directly bypasses the relay, and with it `auth=xrootd` — and an mdsip
session is arbitrary code execution. So:

- `127.0.0.1:8100` is safe but the origin container **cannot reach it**.
- `0.0.0.0:8100` is reachable but exposes unauthenticated code execution to the
  whole network. Never this.
- The right answer is the address the origin sees as "the host", provided that
  address is host-local. Find it:

```bash
sudo podman exec pelican-origin getent hosts host.containers.internal
```

Rootful podman answers with the bridge gateway, which is host-local and exactly
what is wanted — on d3d-origin it is **`10.88.0.1`**. (Rootless answers with the
host's LAN address instead — measured, `10.1.1.6` — which would **not** be
acceptable to bind mdsip to without a firewall rule.)

Rootless podman is permitted to bind a specific non-loopback host address, so
the sandbox can publish there even though it runs as `fdp-mdsip`: verified by
binding a test container to a host IP and seeing the listener appear.

Note what `10.88.0.1` admits: the origin container, and **anything else on the
host's default podman bridge**. That is the right blast radius if the origin is
the only container there; if untrusted containers share that bridge, they can
reach mdsip directly and bypass the relay.

Then publish the sandbox there and point the relay at it:

```bash
MDSIP_PUBLISH=10.88.0.1:8100 scripts/mdsip-sandbox.sh start /mnt/beegfs/data/archives/mdsplus
#            ^ whatever the command above returned
```

If that address turns out to be routable, the alternative is to run the sandbox
**rootful on the origin's own podman network**, so the two talk over the bridge
by container name and nothing is published at all. That trades away the
`fdp-mdsip` service account, though `--userns=auto` would map container-root to
an unprivileged subuid, which is a better mapping than the rootless default.

### What has to change on the origin

1. **Two bind mounts** into the origin container: the `.so`, and a config
   fragment.
2. **`Xrootd.ConfigFile`** in the Pelican config pointing at that fragment.
   Pelican allows one, so if it is already in use the fragment has to merge with
   what is there.
3. **A `/mdsip` namespace export**, with the `Writes` capability — the director
   will not route `PUT` to a namespace without it, and `PUT` is the only method
   it routes (`tests/fed/FINDINGS.md`). Safe despite how it reads: the handler
   claims every `PUT` under its prefix before XRootD consults storage, so none
   can write a file.
4. **A restart** to pick it up.

The fragment itself is one line — `tests/fed/xrootd-relay-only.cfg` is exactly
its production shape:

```
http.exthandler mdsip /plugins/libXrdHttpMdsip.so prefix=/mdsip,host=<mdsip-host>,port=8000,auth=xrootd,authpath=/fdp-d3d/archives
```

Note the **unsuffixed** library name: XRootD appends the plugin version itself.

### Checks worth doing before the change

- **ABI.** The production origin is `origin:v7.23.3`, carrying XRootD
  **v5.9.1**, while the plugin is built against **v5.9.2**. That mismatch turns
  out not to matter: the relay loads and serves a full session in that exact
  image, tested directly. Re-check when either version moves, since XRootD
  refuses plugins whose declared version it considers incompatible and the
  failure is a startup abort, not a degradation.
- **Ext handler headroom.** XRootD allows 4 and Pelican already loads 3
  (`XrdHttpPelican`, `HttpTPC`, and one more), so ours is the fourth and last.
  Anything else wanting one later will not fit.
- **`authpath`.** Point it at the namespace whose tokens clients actually hold —
  `/fdp-d3d/archives` rather than `/mdsip` — or every real token will be
  refused.
