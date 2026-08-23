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
pause process.

A second failure mode looks unrelated and is not: containers starting with an
**empty rootfs**, so the first command in any image fails with
`stat /bin/sh: no such file or directory`, or `exec: "echo": executable file
not found in $PATH`, or `determining run uid: user: unknown user error looking
up user "root"` — all the same thing, seen through whatever the image was
asked to do first. A bare `podman run almalinux:9 echo` reproduces it.

**Observed, on omega06:** the graph root keeps its layer *directories* and its
metadata while the layer contents go missing. A store holding an 868 MB base
image measured 76 MB with 29 layer directories still present, so podman
believes the image is there and mounts nothing. It has happened overnight to
two independent stores under `/local-scratch`, hours apart, with 295 GB free
and no `tmpfiles.d` or `cron.daily` entry naming that path — **the cause is not
established**, and the earlier guess here (runRoot and graphRoot going out of
step) does not fit the evidence: it is the graph root's contents that vanish.

What is established is the recovery. `podman system migrate` does not clear it.
`podman system reset` does, at the cost of every local image. Either way the
images must be pulled again, so treat container images on this host as
**ephemeral** and make any build that depends on one able to re-pull
(`podman build --pull=always`) rather than assuming a warm cache.

To work without resetting someone else's store, give podman a wholly separate
state by overriding `HOME`, `XDG_DATA_HOME`, `XDG_CONFIG_HOME` and
`XDG_RUNTIME_DIR` together, with a `storage.conf` naming fresh
`graphRoot`/`runRoot` paths. That isolates the blast radius; it does **not**
prevent recurrence — the separate store was hit the next day too. `tests/fed/fedbox.sh`, `scripts/mdsip-sandbox.sh` and
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
MDSplus.Connection('fdp://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip')
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
- **`authpath`.** Point it at the path whose tokens clients actually hold, not
  at the relay's own prefix, or every real token is refused. On d3d-origin the
  single export is `FederationPrefix: /fdp-d3d` with
  `Capabilities: [Reads, Writes, Listings]` — note **no `PublicReads`**, so
  reads genuinely require a token and `auth=xrootd` has something to enforce.
  Use **`/fdp-d3d/archives/mdsplus`**: it is the tree root, so the question the
  relay asks is exactly "may you read the trees?". It is also the more permissive
  of the sensible choices — a token scoped narrowly to the archive satisfies it,
  whereas `/fdp-d3d` would demand read on the namespace root and refuse that
  same token.

## The point endpoint

`GET <prefix>/<shot>/<pointname>` returning one PTData record's raw bytes, so a
remote client needs neither a shot index nor XRootD. Client half and wire
contract: GA-FDP/ptdata#42, #46. Design:
`docs/superpowers/specs/2026-08-19-http-point-endpoint-server-design.md`.

### It rides the relay's handler, and has to

XRootD allows **four** HTTP ext handlers — `MAX_XRDHTTPEXTHANDLERS` in
`XrdHttpProtocol.hh`, and a fifth is a startup abort, not a degradation:

```
Config: Cannot load one more exthandler. Max is 4
```

Pelican loads three and `XrdHttpMdsip` is the fourth, so the point endpoint is a
second route inside the existing handler rather than a plugin of its own. That
also means it consumes **no additional slot** and needs no second config line.

### The config line

```
http.exthandler mdsip /plugins/libXrdHttpMdsip.so \
    prefix=/mdsip,host=<mdsip-host>,port=8000,auth=xrootd,authpath=/fdp-d3d/archives/mdsplus,\
    pointprefix=/fdp-d3d/ptdata,pointauthpath=/fdp-d3d/archives/ptdata,\
    pointindex=/fdp-d3d/archives/index/json,pointindexpattern=json_indexes_*,\
    pointurlprefix=pelican://osg-htc.org:443/fdp-d3d/archives,pointroot=/fdp-d3d/archives
```

Omit `pointprefix` and the endpoint does not exist — the relay behaves exactly
as before, so this can ship dark and be turned on deliberately.

The handler **refuses to load** rather than start half-configured: `pointprefix`
without `pointindex`, without `pointauthpath` under `auth=xrootd`, or with only
one of `pointurlprefix`/`pointroot`. Each of those would otherwise answer 404
for every point, which reads as missing data rather than as a mistake.

- **`pointprefix=/fdp-d3d/ptdata`** is what a client's
  `http_endpoint=https://<origin>:8443/fdp-d3d` resolves against, since the
  client appends `/ptdata/<shot>/<pointname>` itself. Note it is *not*
  `/fdp-d3d/archives/ptdata`, where the shot files actually live in the object
  namespace; the bare path is unused and claiming it reserves it.
- **`pointauthpath=/fdp-d3d/archives/ptdata`** — the ptdata archive root, the
  analogue of the relay's `/fdp-d3d/archives/mdsplus`. Point it at what clients'
  tokens actually cover, not at the handler's own prefix, or every real token is
  refused. Authorization is checked per request; unlike the relay's session,
  each GET is independent.
- **`pointurlprefix` / `pointroot`** rewrite the absolute `pelican://` URLs the
  JSON index records onto the local archive. The mdsip sandbox does the same job
  with a symlink chain that depends on the process working directory being `/`;
  that is not available here, because the handler is loaded into Pelican's
  `xrootd` and its cwd is not ours to set.

### No new bind mounts are needed

An earlier revision of this section called for two. That was wrong, and the
truth is better: the origin already mounts the whole archive root
(`-v /mnt/beegfs/data:/fdp-d3d/`, see `d3d-origin-state.md`), so both paths the
point endpoint needs are already inside the container:

| what | path in the container |
|---|---|
| shot files | `/fdp-d3d/archives/ptdata/...` |
| index snapshots | `/fdp-d3d/archives/index/json/...` |

Better still, that mount point matches the federation prefix, so a path inside
the container is the same string as a `pelican://` URL with the host stripped —
which makes the rewrite very nearly an identity. It is still worth doing rather
than relying on the coincidence: it keeps the federation host out of a path and
survives the archive being mounted elsewhere later.

Note that mount is **read-write**, because the origin writes to the archive.
The point endpoint only reads, and libptd3d is built incapable of remote I/O,
but nothing in this design gives the plugin a read-only view.

Resolution is **index-only**: no `SYS_D3` scan and no ptserver tier exist in
this path at all, so an index miss is absent data and can never become a socket
attempt. Coverage therefore equals the snapshot's coverage — a shot on disk but
absent from the index is unavailable, and the fix is a fresher index.

### The plugin must be built in-image

Not a preference — measured:

| built by | max GLIBCXX required | loads on the origin? |
|---|---|---|
| conda toolchain | `3.4.31` | **no** |
| in-image toolchain | `3.4.29` | yes |

AlmaLinux 9 provides up to `GLIBCXX_3.4.29`. The relay alone needs only
`3.4.21`, which is why it has shipped from conda; libptd3d is C++20 and moves
the floor past what the image has. `Containerfile.origin` builds it in-image and
asserts the result, and the CMake option is opt-in (`BUILD_POINT=ON`) so a conda
build cannot quietly acquire an unloadable plugin.

Note `3.4.29` is exactly the ceiling: there is no headroom, and any future
dependency wanting a newer libstdc++ feature breaks loading. The build assertion
catches that rather than the origin failing to start.

libptd3d is linked **statically** (`-DPTDATA_BUILD_STATIC=ON`, ptdata#47), built
`-DPTDATA_WITH_FDPIO=OFF -DPTDATA_WITH_HTTP=OFF` so it is incapable of remote
I/O. The plugin's `NEEDED` list is asserted to contain no `ptd3d`, `curl`,
`fdpio`, `XrdCl` or MDSplus entry.

### Two traps that cost real time

- **The library name in the config must be UNSUFFIXED.** `XrdOucPinLoader`
  appends the plugin version, so writing `libXrdHttpMdsip-5.so` has XRootD look
  for `libXrdHttpMdsip-5-5.so`. Already true for the relay; easy to repeat.
- **Without TLS, XRootD loads only ext handlers declared `+notls`** — and says
  nothing about the ones it skips. The directive is still echoed at startup, so
  the symptom is XRootD answering **403** for the endpoint's own paths, which
  reads like an authorization problem rather than a handler that never loaded.
  Production configures HTTPS and must **not** use `+notls`;
  `tests/integration/point-endpoint.cfg` does, because it serves plain HTTP on
  loopback.

### Verifying it after the change

The origin log should carry, at startup:

```
Plugin loaded XrdHttpMdsip ... from exthandlerlib .../libXrdHttpMdsip-5.so
++++++ XrdHttpMdsip point endpoint: /fdp-d3d/ptdata -> index /ptdata-index
```

Absence of the second line means the handler loaded without the endpoint, which
is a configuration problem, not a runtime one.

Then, end to end, from a client with a token:

```bash
curl -sD - -o /dev/null -H "Authorization: Bearer $BEARER_TOKEN" \
  "https://<origin>:8443/fdp-d3d/ptdata/165920/IP"
# HTTP/1.1 200 OK
# X-Ptdata-Extension: .MAG
```

`tests/integration/test_point_endpoint.sh` runs the client's full contract suite
against a container serving the real plugin, which is the same set of assertions
GA-FDP/ptdata makes against its stub.

### The three configuration surfaces still have to move together

`scripts/mdsip-sandbox.sh`, `deploy/fdp-mdsip.container`, and
`d3d-origin-admin`'s `site.env` + `etc/fdp-mdsip.container.in`. A control present
in one and missing from another is exactly the failure that arrangement exists
to prevent, and the point endpoint adds six parameters and two mounts to keep in
step.

## Launching the origin: `podman kube play`, and why not compose

`deploy/kube/fdp-origin.yaml` describes the origin declaratively;
`deploy/fdp-origin.kube` is a quadlet unit so systemd supervises it. Deploying
becomes a change to one `image:` line, and rolling back is putting the previous
tag back — the old image is still in the registry.

**Compose was considered and is not available here.** On d3d-origin, `docker`
is the podman-docker emulation shim, `docker-compose` is absent, and
`podman compose` reports "looking up compose provider failed". Adopting compose
would mean installing a provider on the origin to gain convenience, whereas
`podman kube play` is already present and gives the same single declarative
file, with systemd supervision through a `.kube` unit.

### The mdsip sandbox stays a quadlet

Only the origin moves. `deploy/fdp-mdsip.container` keeps the sandbox, for two
reasons that are worth stating so nobody "finishes the job" later.

**Its isolation is a host-account boundary, not a container UID.** The sandbox
runs under rootless podman as the `fdp-mdsip` account with its own subuid range
(`/etc/subuid: fdp-mdsip:755360:65536`), so container-root maps to an
unprivileged host uid shared with nothing else. A `user:` field — in compose or
in a pod spec — sets the UID *inside* the container and does not reproduce
that. Keeping the two under separate accounts is what preserves it, and that
means separate invocations either way, so a single file buys nothing.

**Kubernetes YAML cannot express all of its hardening.** Measured against
podman 5.4.0 on the origin, which has `memory pids` cgroup delegation:

| quadlet setting | expressible in a pod spec? |
|---|---|
| `ReadOnly=true` | yes — `securityContext.readOnlyRootFilesystem` |
| `DropCapability=ALL` | yes — `securityContext.capabilities.drop: [ALL]` |
| `NoNewPrivileges=true` | yes — `allowPrivilegeEscalation: false` |
| `SeccompProfile=` | yes — `securityContext.seccompProfile` |
| `--memory=8g` | yes — `resources.limits.memory` (verified: 512Mi → 536870912) |
| **`--pids-limit=512`** | **no Kubernetes field** |
| **`--dns=none`** | only approximately, via `dnsConfig` |

A fork bomb inside the sandbox is precisely the threat `--pids-limit` answers,
and the sandbox's premise is that a client already has code execution inside
it. Trading that for uniformity would be a poor bargain.

Note when testing hardening locally: **a host without delegated cgroup
controllers silently ignores memory and pids limits**, whether they come from
`kube play` or from `podman run`. On omega, `podman run --memory=512m` yields
`Memory: 0`; the same YAML on the origin yields the real value. Check
`/sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service/cgroup.controllers`
before concluding a limit was dropped by the tool.
