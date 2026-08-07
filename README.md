# xrdoss-mdsplus

An out-of-tree XRootD `XrdOss` plugin that serves MDSplus TDI evaluation
results as virtual files, plus the out-of-process evaluator it talks to.

Loaded into a **stock** Pelican origin — no fork — by pointing
`Xrootd.ConfigFile` at a fragment containing:

```
ofs.osslib ++ /path/to/libXrdOssMdsplus.so prefix=/tdi socket=/run/fdp/evald.sock
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
                                  │  GET /fdp-d3d/tdi/efit01/00/00/19/00/190000/AQABAAJpcAAAAAZcaXBtaGQA
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
  │ ORIGIN — XRootD managed by Pelican      (stock image, config only)     │
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
  │     └─ Base64UrlDecode  Base64Url.cc                                   │
  │     └─ Request::Parse   Request.cc   strict: version, no trailing bytes│
  │          ▼                                                             │
  │   ResultCache           ResultCache.cc   hit? serve it                 │
  │          │ miss                                                        │
  │          ▼                                                             │
  │   EvalClient            EvalClient.cc    frame + unix socket           │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │  u32 len | u16 tree_len | tree | i64 shot | request
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │ fdp-mdsplus-evald            (evaluator/, separate process)            │
  │                                                                        │
  │   parse_request()          same canonical format the path carried      │
  │   reset_private/public()   ⚠ or TDI state leaks between requests       │
  │   MDSplus.Tree(...)        local disk — this is where the win is       │
  │   data(<expr>)             ⚠ or the result references tree nodes       │
  │   serialize()              → MDSplus descriptor bytes                  │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │  u8 status | u32 len | payload
                                  ▼
                     back up the stack, cached, served as file bytes
```

MDSplus is **never linked into XRootD**. The plugin is a path parser plus a
socket client; a crash, hang or leak in MDSplus takes down a disposable worker
rather than the origin.

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
canonical request   01 0001 0002 6970 00000006 5c69706d686400   (18 bytes)
                    │  │    │    │    │        └─ "\ipmhd", then 0 args
                    │  │    │    │    └─ u32 expression length
                    │  │    │    └─ "ip"
                    │  │    └─ u16 name length
                    │  └─ u16 item count
                    └─ u8 format version

base64url           AQABAAJpcAAAAAZcaXBtaGQA

path                /fdp-d3d/tdi/efit01/00/00/19/00/190000/AQABAAJpcAAAAAZcaXBtaGQA
                                 │      └─────────┬──────┘ └──────┬─────────────┘
                                 │                │               └─ the request
                                 │                └─ shot/100 in digit pairs
                                 └─ tree ('-' means: evaluate with no tree open)

response            an MDSplus serialized dictionary: {"ip": {"value": <array>}}
                    a failed expression yields {"ip": {"error": "..."}} and the
                    response is still 200 — matching GetMany semantics
```

The digit-pair bucketing mirrors the existing MDSplus archive layout and keeps
~100 shots per directory. It is not decoration: a flat namespace makes XrdPfc's
startup scan take hours while holding a global lock on open
([xrootd#2804](https://github.com/xrootd/xrootd/issues/2804)).

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

### Two things that look optional and are not

**`data(...)` wrapping.** The evaluator wraps every expression, exactly as
MDSplus's own `GetMany` does locally (`connection.py:392`). Without it, `\ipmhd`
evaluates to `Build_Signal(Build_With_Units([...], "A"), *, \ATIME)` — values
literal, but the time base still a *reference* to node `\ATIME`. Deserializing
that needs the tree open in the client, which is never true here. The cost is
that dimensions and units do not travel with a value; ask for `dim_of(...)` as
another item in the same batch, which is nearly free.

**TDI state reset.** TDI variables persist across evaluations in a process, so a
long-lived worker would leak them between requests — `_x = 41` in one request
makes `_x + 1` return 42 in the next. That is both a cross-request information
leak and a cache-correctness bug, since the same path would return different
bytes depending on history.

### Not yet built

This repo currently implements the origin side only. Still to come, in order:

| Piece | Status |
|---|---|
| Sandboxing the evaluator | **not built** — TDI can `spawn()`, so this is required before any exposure |
| Version segment in the path | **not built** — without it a re-analysed shot is served stale forever, because XrdPfc never revalidates |
| `libMdsIpFDP.so` client transport | not built — lets stock `MDSplus.Connection` use this by changing only its connection string |

Until the first two land, this is a working prototype rather than something to
deploy.

## Layout

| Path | What |
|---|---|
| `src/` | The plugin. Logic lives in small units (`Base64Url`, `Request`, `TdiPath`, `EvalClient`, `ResultCache`); `OssMdsplus.cc` is thin wiring over them. |
| `evaluator/` | Python daemon that evaluates TDI and returns serialized MDSplus descriptors over a unix socket. Keeps MDSplus out of the XRootD process. |
| `tests/` | doctest unit tests, pytest for the evaluator |
| `tests/fed/` | Local Pelican federation in podman — see `tests/fed/FINDINGS.md` |
| `tests/integration/` | Standalone XRootD end-to-end |

## Building

```bash
pixi run build     # -> build/libXrdOssMdsplus-5.so
pixi run test      # doctest suites via ctest
pixi run pytest    # evaluator tests
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

## Design

`repos/docs/superpowers/specs/2026-08-06-mdsplus-virtual-file-service-design.md`
and the implementation plan alongside it in `plans/`.
