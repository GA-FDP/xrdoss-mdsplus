# xrdoss-mdsplus

An out-of-tree XRootD `XrdOss` plugin that serves MDSplus TDI evaluation results
as virtual files. Evaluation is delegated to MDSplus's own `mdsip` server via
`GetManyExecute($)`, so the plugin itself is a path parser and a pipe.

Loaded into a **stock** Pelican origin — no fork — by pointing
`Xrootd.ConfigFile` at a fragment containing:

```
ofs.osslib ++ /path/to/libXrdOssMdsplus.so prefix=/tdi server=localhost:8000
```

Note the name in the config has **no `-5` suffix** even though the file on disk
is `libXrdOssMdsplus-5.so`. XRootD appends the plugin version itself; referencing
the suffixed name makes it search for `-5-5.so` and only succeed via a fallback.

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

### Not yet built

This repo currently implements the origin side only. Still to come, in order:

| Piece | Status |
|---|---|
| Sandboxing mdsip | **not built** — TDI can `spawn()`, so this is required before any exposure |
| `libMdsIpFDP.so` client transport | not built — lets stock `MDSplus.Connection` use this by changing only its connection string |

Until the first two land, this is a working prototype rather than something to
deploy.

## Layout

| Path | What |
|---|---|
| `src/` | The plugin. Logic lives in small units (`Base64Url`, `TdiPath`, `MdsIpClient`, `ResultCache`); `OssMdsplus.cc` is thin wiring over them. |
| `tests/` | doctest unit tests; `mkpath.py` is the Python-side reference implementation of the path grammar |
| `tests/fed/` | Local Pelican federation in podman — see `tests/fed/FINDINGS.md` |
| `tests/integration/` | Standalone XRootD end-to-end |

## Building

```bash
pixi run build     # -> build/libXrdOssMdsplus-5.so
pixi run test      # doctest suites via ctest
```

End-to-end (starts mdsip and a standalone XRootD for you):

```bash
pixi run bash tests/integration/run_e2e.sh        # ~1s, no container
pixi run bash tests/integration/test_real_tree.sh # needs a staged tree
```

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
