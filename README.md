# xrdoss-mdsplus

Two out-of-tree XRootD plugins that put MDSplus data behind a Pelican origin,
serving the two kinds of client that exist:

| Plugin | For | Gets |
|---|---|---|
| `libXrdOssMdsplus.so` (`XrdOss`) | path-based clients — `toksearch`, `curl`, `pelicanfs` | **caching**; TDI results as virtual files |
| `libXrdHttpMdsip.so` (`XrdHttpExtHandler`) | existing thin-client code holding an `MDSplus.Connection` | **full protocol compatibility**; mdsip relayed over the origin's HTTPS port |

Neither evaluates anything itself: both delegate to MDSplus's own `mdsip`
server, so the first is a path parser and a pipe, and the second is a pipe.

Both load into a **stock** Pelican origin — no fork — by pointing
`Xrootd.ConfigFile` at a fragment containing:

```
ofs.osslib ++ /path/to/libXrdOssMdsplus.so prefix=/tdi server=localhost:8000
http.exthandler mdsip /path/to/libXrdHttpMdsip.so prefix=/mdsip,host=localhost,port=8000
```

Note that neither name in the config carries the **`-5` suffix** the files on
disk do. XRootD appends the plugin version itself; referencing the suffixed name
makes it search for `-5-5.so` and only succeed via a fallback.

The rest of this document covers the virtual-file plugin first, since it is
where the design work is; the relay has its own section further down:
[The other half](#the-other-half-mdsip-tunnelled-over-the-same-port).

## How a request flows

The central idea: **the object path *is* the query.** A client asks for a path
that encodes `(tree, shot, TDI expression)`, and gets back the evaluated result
as if it were a file. Nothing about the request lives outside the path — not in
a query string, not in a header — because that is the only channel that survives
the federation intact (see [Why the path carries everything](#why-the-path-carries-everything)).

```
  ┌────────────────────────────────────────────────────────────────────────┐
  │ CLIENT                                                                 │
  │   asks for an object path, deserializes the bytes it gets back         │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │  GET /fdp-d3d/tdi/efit01/00/00/19/00/190000/BADWxBAA...
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │ PELICAN DIRECTOR                        (stock Pelican, unmodified)    │
  │   path.Clean() on the decoded path, then 307 to an origin              │
  │   ⚠ this is why the encoding may contain no '/' and no '%'             │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │ OSDF CACHE (XrdPfc)                     ── future; see caveat below ── │
  │   keys on URL path only, so the token in ?authz= never fragments it    │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │  (miss)
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │ ORIGIN — XRootD managed by Pelican      (stock image + MDSplus runtime)│
  │                                                                        │
  │   ofs.osslib  default                  ← native POSIX storage          │
  │   ofs.osslib ++ libXrdOssStats.so      ← Pelican's own layer           │
  │   ofs.osslib ++ libXrdOssMdsplus.so    ← THIS PLUGIN, injected via     │
  │                                          Xrootd.ConfigFile             │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │ libXrdOssMdsplus.so                                    (src/)          │
  │                                                                        │
  │   IsTdiPath(lfn, prefix)?                                              │
  │     no  ──────────────────────────────► forward to the wrapped Oss     │
  │                                          (the raw tree-file archive    │
  │                                           keeps working, same origin)  │
  │     yes ─┐                                                             │
  │          ▼                                                             │
  │   ParseTdiPath          TdiPath.cc   split path, validate shot bucket  │
  │     └─ join chunks                   ≤249 bytes each (NAME_MAX)        │
  │     └─ Base64UrlDecode  Base64Url.cc → opaque payload, NOT parsed      │
  │          ▼                                                             │
  │   ResultCache           ResultCache.cc   hit? serve it                 │
  │          │ miss                                                        │
  │          ▼                                                             │
  │   MdsIpClient           MdsIpClient.cc   ConnectToMds / MdsOpen,        │
│                                          bounded by a per-request timeout│
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │  SendDsc x2 + GetAnswerInfoTO(timeout)  -> GetManyExecute($)
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │ mdsip -m                     (MDSplus's own server, separate process)  │
  │                                                                        │
  │   GetManyExecute($)        upstream C++: evaluates every item and      │
  │                            returns an ALREADY-serialized dictionary    │
  │                            (mdsobjects/cpp/mdsdata.c:921)              │
  │   process per connection   -m; GetManyExecute uses a static XD and is  │
  │                            not thread-safe                             │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │  serialized {name: {value|error}}
                                  ▼
                     back up the stack, cached, served as file bytes
```

The plugin never interprets the payload. It decodes the path, hands the bytes to
`GetManyExecute($)`, and writes the answer out verbatim — MDSplus is the only
thing that understands the request format, so there is one definition of it
rather than ours plus theirs. Semantics therefore match `atlas.gat.com` by
construction rather than by testing.

**On isolation.** Evaluation happens in the mdsip process, so a crash or hang in
tree opening or TDI takes down a disposable worker rather than the origin. But
the separation is not total: `libMdsIpShr.so` directly `DT_NEEDED`s `libTdiShr`,
`libTreeShr` and `libMdsShr`, so those libraries are *loaded* into the XRootD
process even though we never call them. See `tests/fed/FINDINGS.md`.

### Stat, then Read

XRootD asks for a size before it asks for bytes, and the Pelican director parses
`Content-Length` with `strconv.Atoi` — a failure there becomes a 404 rather than
a redirect. So `Stat` cannot be lazy: it evaluates the expression, caches the
payload, and reports the real length. The `Open`/`Read` that follows serves from
that cache.

A cache miss between `Stat` and `Read` simply re-evaluates, so correctness never
depends on the cache. That is what lets it be a fixed-size LRU with no
invalidation logic at all.

### A worked example

`\ipmhd` from `efit01`, shot 190000, labelled `ip`:

```
request      apd.List([apd.Dictionary({'name':'ip','exp':'\\ipmhd','args':()})])
             .serialize()                                        → 135 bytes

             i.e. exactly what MDSplus's GetMany builds and sends. We produce it
             with tests/mkpath.py; a real client already has GetMany.serialize().

base64url    BADWxBAAAAAAAAABBAAAABQ...ANjEEAAAAAAAAAEYAAAAKAAAA...

path         /tdi/efit01/00/00/19/00/190000/vfe49badc9b703486/BADWxBAA...
                  │      └────────┬───────┘ └───────┬───────┘ └────┬────┘
                  │               │                 │              └─ payload,
                  │               │                 │       chunked at 249 B
                  │               │                 └─ version (see below)
                  │               └─ shot/100 in digit pairs
                  └─ tree ('-' means: evaluate with no tree open)

response     a serialized dictionary {"ip": {"value": <array>}}; a failed
             expression yields {"ip": {"error": "..."}} and the response is
             still 200 — GetMany semantics, because it IS GetMany
```

The digit-pair bucketing mirrors the existing MDSplus archive layout and keeps
~100 shots per directory. It is not decoration: a flat namespace makes XrdPfc's
startup scan take hours while holding a global lock on open
([xrootd#2804](https://github.com/xrootd/xrootd/issues/2804)).

### The version segment

XrdPfc **never revalidates**. Once an object is cached it is served until purged
for capacity, and Pelican has no per-namespace "do not cache" flag. So without a
version in the name, a re-analysed shot would be served stale forever.

The token is derived from a `stat` of the tree's `.datafile` — inode, size and
mtime, hashed — so regenerating the tree changes the object name. Old cache
entries simply age out, and copies of superseded versions stay *correct*: they
hold exactly what that version was.

The plugin recomputes the current token on every request and refuses anything
else, so a stale path cannot be served afresh. It locates the file through
`treepath=`, a semicolon-delimited list of templates mirroring MDSplus's own
`<tree>_path` convention (`%T` tree, `%S` shot, `%B` digit-pair bucket) — a list
because a tree may live under `codes/`, `shots/` or another branch.

A remote client cannot derive the token itself — it has no access to the tree
files — so it asks:

```
GET /fdp-d3d/tdi-version/<tree>/<d1>/<d2>/<d3>/<d4>/<shot>   ->  vfe49badc9b703486
```

That answer is deliberately **not** cached, by us or by anyone: its whole
purpose is to report a value that changes. Fetch it with `?directread`. The
natural place to call it is `openTree()`, once per tree-open, after which every
expression in that session reuses the token.

**Without `treepath=` the plugin refuses every request that names a tree.**
Failing closed is deliberate: serving an unverifiable object is exactly the bug
versioning exists to prevent. Requests naming no tree carry version `-`.

### Why the path carries everything

Client-supplied headers do **not** reach the origin through a cache. On a miss
XrdPfc reads upstream via `XrdOucCacheIO` with no HTTP request context, and the
XrdCl layer builds its request from URL parameters. That is precisely why
Pelican has `http.header2cgi Authorization authz` — moving a header into the URL
is the only way to get it across. So there is no side channel: no headers, no
request body, no out-of-band registry. The path is the entire request.

A corollary is that the encoding alphabet matters for correctness, not tidiness.
A `/` inside a path segment is eaten by the director's `path.Clean`, and `%2F`
cannot protect it because the decoded path is what gets normalised. base64url
(`A-Za-z0-9-_`) survives; standard base64 does not. Measured, both ways, in
[`tests/fed/FINDINGS.md`](tests/fed/FINDINGS.md).

### One thing that looks optional and is not

**mdsip needs `MDS_PATH` to include `tdi/remote`.** `GetManyExecute` is a TDI
function (`tdi/remote/GetManyExecute.fun`), not a builtin. Without that path the
server answers every request with `%TDI-E-UNKNOWN_VAR`, which reads like a
client bug and is not one.

Two things that used to be required here are no longer: wrapping expressions in
`data(...)`, and resetting TDI state between requests. Both were compensating
for a bespoke evaluator that no longer exists — mdsip returns unwrapped nodes
correctly and gives each connection its own process. See `docs/mdsip-spike.md`.

## The other half: mdsip tunnelled over the same port

Everything above serves *path-based* clients — callers who can be told to batch,
and therefore the ones caching pays for. But most existing DIII-D code is not
that. It holds an `MDSplus.Connection` and calls `conn.get()` one signal at a
time (55 such call sites in the local repos, against 0 for `getMany`).

For those callers there is a second plugin, `libXrdHttpMdsip.so`: an
`XrdHttpExtHandler` that **relays the mdsip protocol** over the origin's
existing HTTPS port, TLS and SciTokens.

```
  MDSplus.Connection            (stock, unmodified)
        │  mdsip over TCP
        ▼
  client transport              accumulates one COMPLETE call, POSTs it
        │  HTTPS
        ▼
  XRootD :443 ── XrdHttpMdsip   holds one mdsip socket per session
        │  mdsip over TCP
        ▼
  mdsip server                  does the actual work
```

Three POSTs make up the whole protocol:

| Request | Body up | Body down |
|---|---|---|
| `POST <prefix>/connect` | — | an opaque session token |
| `POST <prefix>/msg` + `X-Fdp-Session` | one complete mdsip call | exactly one mdsip answer |
| `POST <prefix>/close` + `X-Fdp-Session` | — | — |

Loaded alongside the osslib, again with the **unsuffixed** name:

```
http.exthandler mdsip /path/to/libXrdHttpMdsip.so prefix=/mdsip,host=localhost,port=8000
```

Parms are a single `XrdOucStream::GetWord()` token, so they **cannot contain
spaces** — comma is the separator. Add `+notls` before the library path only
when running without TLS, as the test harness does.

### What the tunnel buys and what it costs

Full protocol compatibility, for free: `get`, `getMany`, `put`, `setDefault`,
anything — because a real mdsip server handles it, and the semantics match
`atlas.gat.com` because they *are* `atlas.gat.com`'s, relayed. There is no
reply fabrication anywhere, so there is no class of bug where a mistake yields
plausible wrong data instead of an error.

The cost is caching, and it cannot be recovered: a cache keys on a URL, and a
tunnel has none. Moving the intelligence to the origin does not help either,
because the request that crossed the network was still a POST. The two plugins
are complementary rather than competing — see [`docs/relay-spike.md`](docs/relay-spike.md).

### Sessions, and why they are mandatory

**mdsip is stateful.** `openTree` sets the tree context *on the connection*, so
a relay that opened a fresh mdsip connection per HTTP request loses it and every
subsequent `get()` fails. A stateless variant was tried and dies immediately
with `%MDSPLUS-E-ERROR`.

So the handler holds one mdsip socket per session, which has consequences worth
stating plainly:

- sessions are **sticky to one origin** and cannot be load-balanced
- an abandoned session holds an mdsip connection until the idle reaper takes it
  (`idle=`, default 300s), and `maxsessions=` bounds the damage from a flood
- one call at a time per session; a concurrent second call gets `409` rather
  than being interleaved into the byte stream, which would corrupt it
- a broken stream retires the session instead of resynchronising — a half-written
  call cannot be recovered from, and the alternative is handing the next caller
  misaligned bytes

The relay links **no MDSplus library at all** (`readelf -d` shows only
`libXrdUtils`, `libXrdHttpUtils` and libc), speaking mdsip over a plain socket.
An origin container serving only the tunnel therefore needs no MDSplus runtime.

## Sandboxing, which is not optional

Both plugins end at the same place: a client-supplied TDI expression evaluated
by an MDSplus server. **That is arbitrary native code execution** — measured,
not assumed. `MdsShr->system("touch /tmp/x")` creates the file, because
`image->routine()` is a generic FFI and `dlsym` reaches libc through the
library's dependency chain. So removing `spawn()` would accomplish nothing, and
there is no subset of TDI that is both useful and safe.

The security boundary therefore cannot be inside the mdsip process. The
container is the boundary:

```bash
pixi run sandbox-build     # -> localhost/fdp-mdsip
pixi run sandbox-verify    # starts it, attacks it, tears it down
MDSIP_SANDBOX=1 pixi run relay-e2e   # the relay against the sandbox
```

`scripts/mdsip-sandbox.sh` runs mdsip in its own container with no route off
the host, a read-only root, read-only trees, no capabilities, and resource
limits. It is a **separate container from the origin** — the single most
important control, because a stolen `issuer.jwk` mints arbitrary federation
tokens.

`tests/security/verify_sandbox.sh` asserts each control by attacking it through
ordinary TDI, the same channel a client has. It first confirms the client
*does* have code execution, so a passing run cannot pass for the wrong reason.

**Run it on the target host, not just a dev box.** The network controls are
properties of podman's network backend rather than of the flags, and podman 5.0
removed CNI — so a result from a podman 4.x/CNI machine says nothing about a
netavark one. The script prints the stack it ran on for that reason. See
[the caveat in `docs/security.md`](docs/security.md#that-result-does-not-transfer-to-the-origin-host).

Read [`docs/security.md`](docs/security.md) before deploying either plugin. It
has the threat model, the measured capabilities, and three findings that
reading documentation would not have produced — including that podman mounts
the host's credentials into the container by default, and that it silently
ignores resource limits on cgroups v1 rootless.

### Not yet built

| Piece | Status |
|---|---|
| `libMdsIpFDP.so` client transport | not built — the tunnel's client half. `tests/integration/mdsip_http_client.py` is a working Python equivalent and deliberately the same shape; see `docs/client-transport.md` |
| microVM isolation | not built — `/dev/kvm` is available, and it would replace a shared kernel with a virtualised one. A hardening step, not a prerequisite |
| Tailored seccomp profile | not built — podman's default profile is in force |

Deployment also needs ops access, which is the actual gate on going live.

## Layout

| Path | What |
|---|---|
| `src/` | Both plugins. The osslib's logic lives in small units (`Base64Url`, `TdiPath`, `MdsIpClient`, `ResultCache`) with `OssMdsplus.cc` thin wiring over them; the relay is `HttpRelay.cc` over `MdsipSession.cc`. The two share no code. |
| `tests/` | doctest unit tests; `mkpath.py` is the Python-side reference implementation of the path grammar |
| `tests/fed/` | Local Pelican federation in podman — see `tests/fed/FINDINGS.md` |
| `tests/integration/` | Standalone XRootD end-to-end, for both plugins |
| `tests/security/` | What a client can do (`probe_capability.py`) and what the sandbox must stop it doing (`verify_sandbox.py`) |
| `scripts/` | `mdsip-sandbox.sh` — the isolation flags, which are where the containment actually lives |

## Building

```bash
pixi run build     # -> build/libXrdOssMdsplus-5.so, build/libXrdHttpMdsip-5.so
pixi run test      # doctest suites via ctest
```

End-to-end (each starts mdsip and a standalone XRootD for you):

```bash
pixi run e2e                                      # virtual files, ~1s, no container
pixi run relay-e2e                                # the tunnel, with a stock MDSplus.Connection
pixi run bash tests/integration/test_real_tree.sh # needs a staged tree
```

`relay-e2e` is the one that proves the tunnel: it runs a real
`MDSplus.Connection` through the handler and compares every result against the
same call made straight to mdsip. Anything short of exact agreement fails.

XRootD is pinned `<6` to match the deployed v5 plugin chain. The Pelican origin
image ships XRootD v5.9.2 and the pixi environment resolves to the same version,
so the plugin is built against the ABI it is loaded into.

## Testing against a real federation

```bash
bash tests/fed/fedbox.sh start                    # plain federation
bash tests/fed/fedbox.sh start /tmp/extra.cfg     # with an osslib fragment
bash tests/fed/fedbox.sh start /tmp/extra.cfg /path/to/plugin.so
bash tests/fed/fedbox.sh stop
```

`tests/fed/run_fed_test.sh` needs the MDSplus-capable origin image, because
`ConnectToMds` dlopens `libMdsIpTCP.so` by name at runtime:

```bash
podman build -f Containerfile.runtime -t fdp-origin-mdsplus .
pixi run bash tests/fed/run_fed_test.sh
```

This is the only layer that exercises the director. See
[`docs/deployment-notes.md`](docs/deployment-notes.md) if podman complains about
`/run/user/$UID`.

## Design

`repos/docs/superpowers/specs/2026-08-06-mdsplus-virtual-file-service-design.md`
and the implementation plan alongside it in `plans/`.
