# HTTP point endpoint — server handler

The origin half of the PTData point endpoint. The client half is built and
under review in GA-FDP/ptdata#46; this serves the contract it speaks.

Related: GA-FDP/ptdata#42 (tracking), ptdata#45 (director/307 follow-on),
`ptdata:docs/superpowers/specs/2026-08-19-http-point-provider-design.md` (the
contract and the client).

## Problem

A remote PTData client today either reads shot files itself — five round trips
per pointname, none amortised (ptdata#43) — or drives TDI through the mdsip
relay, which is fast on small points because it spends four round trips per
shot, but decodes on the origin and saturates it on large ones. Measured, three
pointnames of 51,200 samples per shot:

| workers | ptdata/Pelican | TDI/relay | ptdata/athena |
|---|---|---|---|
| 8 | 1.61 shots/s | **9.09** | 1.66 |
| 24 | 3.62 | **17.28** | 3.50 |

and one pointname of 480,256 samples:

| workers | ptdata/Pelican | TDI/relay | ptdata/athena |
|---|---|---|---|
| 8 | **2.99** | 2.25 | 2.96 |
| 24 | **6.54** | 4.68 | 6.26 |

The point endpoint takes the relay's round-trip count and the file path's
client-side decode: the origin locates and reads one record, the client decodes
it. Decode is linear at ~169 ns/sample against ~3.6 ms to locate and serve, so
moving it off the origin removes essentially all per-request origin CPU.

## The constraint that shapes everything

**XRootD allows four HTTP ext handlers, and all four are taken.** From
`XrdHttpProtocol.hh`:

```c
#define MAX_XRDHTTPEXTHANDLERS 4
```

and loading a fifth is a startup abort, not a degradation
(`XrdHttpProtocol.cc`: `"Cannot load one more exthandler. Max is 4"`). Pelican
loads three; `XrdHttpMdsip` is the fourth (`docs/deployment-notes.md`).

So the point endpoint **cannot be a new plugin**. It goes into the existing
`XrdHttpMdsip` handler, which already has the pieces it needs: parameter
parsing, the authorization delegation, and a deployed config line to extend.

This is worth re-verifying on the target origin before building, since it is
the single assumption the whole shape rests on: if Pelican's handler count
changes, a separate plugin becomes possible again and would be cleaner.

### Naming

The library keeps the name `libXrdHttpMdsip.so` even though it will serve
something that is not mdsip. Renaming means a coordinated change to the origin
config line, the bind mount, and the `xrdhttp-mdsip` conda package, for no
functional gain. If it is ever renamed, do it alongside another breaking deploy
change rather than on its own.

## Server obligations

```
GET <prefix>/<shot>/<pointname>[?ext=.MAG]

200  Content-Type: application/octet-stream
     X-Ptdata-Extension: .MAG        <- the extension actually used
     body = the raw record, byte-for-byte ShotFile::read_point output

404  pointname absent from every extension, or shot not found
     -> the client raises ShotNotFound and its provider chain advances
else -> the client propagates a real error rather than reading it as a miss
```

`?ext` is a hint. If it resolves, use it; if it misses, resolve across
extensions and report which one answered. The body is the **raw record** —
preheader, fixed header, variable header, data — undecoded, VAX floats intact.
That is what makes this cheap: the origin never parses.

## Handler

`MatchesPath` gains GET under a second prefix, alongside the existing POST/PUT
under `/mdsip`. The existing prefix-boundary check applies unchanged and for the
same reason: match only where the prefix ends at `/` or end-of-string, or
`/fdp-d3d/ptdata` also claims `/fdp-d3d/ptdatafoo`.

The relay refuses GET deliberately — "GET would shadow the object namespace the
Oss plugin serves". That reasoning does not extend to a disjoint prefix: GET is
claimed only under the point prefix, which is not a storage path.

**Prefix: `/fdp-d3d/ptdata`,** so a client configures
`http_endpoint=https://<origin>:8443/fdp-d3d` and the provider's own
`<endpoint>/ptdata/<shot>/<pointname>` lands on it.

Note this is *not* `/fdp-d3d/archives/ptdata/...`, which is where the real shot
files live in the object namespace. The bare `/fdp-d3d/ptdata` is unused, and
claiming it reserves it — worth recording so nobody later exports storage there
and wonders why GETs never reach it.

## Authorization

Reuse `Authorized()` verbatim: hand the bearer token to `sfs_->stat()` as an
`authz` opaque and let the origin's own SciTokens policy answer. No second token
implementation, and the endpoint is protected by whatever protects the rest of
the origin. `ENOENT` means "policy said yes, nothing there"; anything else
denies, so an unexpected failure fails closed.

**`authpath=/fdp-d3d/archives/ptdata`** — the ptdata archive root, the analogue
of the relay's `/fdp-d3d/archives/mdsplus`. Per the deployment notes, point it
at what clients' tokens actually cover rather than at the handler's own prefix,
or every real token is refused.

**Checked per request, not per session.** The relay authorizes once at
`/connect` and rides a session token afterwards, because re-authorizing every
call would add a round trip to a workload of thousands of gets. That trade does
not carry over: each GET here is independent and stateless, so a session would
buy nothing but a cache of authorization decisions — which is exactly the kind
of optimization not to introduce before it is shown to be needed.

The cost is one `sfs_->stat` per point fetch. **Measure it before deploying at
scale**; the whole design is a round-trip argument, and if token validation is
not cached by `XrdAccSciTokens` it would show up directly in the numbers.

## Data path

Resolution is **index first**, exactly as the mdsip sandbox does it
(`2026-08-13-ptdata-in-the-mdsip-sandbox-design.md`). That spec states the
reason plainly and it applies here unchanged: *a directory scan is too slow*.

```
PTDATA_JSON_INDEX_DIR      /ptdata-index
PTDATA_JSON_INDEX_PATTERN  json_indexes_*
PTDATA_PTSERVERS           none
SYS_D3                     unset
```

Three variables, not four: `JsonIndexPlugin` is compiled into `libptd3d` and
`index_plugin_from_env()` reaches it first, so no `PTDATA_PLUGIN_LIB` is needed
— that names the *dynamic* plugin fallback, which this does not use.

`PTDATA_PTSERVERS=none` is not optional, for the same reason it is not optional
in the sandbox: `-DPTDATA_WITH_FDPIO=OFF` removes remote *file* access but not
ptserver, which is a raw socket. Without the sentinel an index miss becomes a
connection attempt, reported as a network error rather than as absent data.

An earlier revision of this section had the server scanning `SYS_D3` and
resolving extensions through `ShotLocator`'s tier-2b `scan_for_pointname`. That
was wrong twice over, and the corrections are worth stating because both
mistakes are easy to repeat.

**The scan is not cheap here.** Tier 2a costs two stats per `SYS_D3` directory
and the archive spans `ptdata1..ptdatae`, while tier 2b calls `list_files` on
each search directory and then *opens each candidate shotfile* to inspect its
directory. On `/mnt/beegfs` every one of those is a metadata round trip to a
parallel filesystem, not a local `stat`. The sandbox measured this and chose
index-only.

**The index is usable on the origin**, which the earlier text denied on the
grounds that its entries are `pelican://` URLs and a `PTDATA_WITH_FDPIO=OFF`
build cannot open those. The build genuinely cannot — and does not need to.
The sandbox's workaround needs no code at all:

```json
".PCE": "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE"
```

That string does not begin with `/`, so it is a **relative** path, and POSIX
collapses the consecutive slashes. With the working directory at `/` it
resolves as `/pelican:/osg-htc.org:443/fdp-d3d/archives/...`, and the image
carries that directory chain with its last component symlinked at the root the
index paths are relative to:

```
/pelican:/osg-htc.org:443/fdp-d3d  ->  /fdp-archives
/fdp-archives/archives/ptdata          ro mount
```

`LocalIoProvider::resolve()` returns its argument unchanged, so the literal URL
string reaches `::open()` and succeeds. Verified in the sandbox.

### Two ways to make those URLs open, and why the origin differs

The sandbox's symlink chain is proven and costs no code, but it rests on one
thing the sandbox controlled and we do not: **the working directory must be
`/`**. The sandbox launches its own process and can `cd /`; here the handler is
loaded into Pelican's `xrootd`, whose cwd is not ours to set and must not be
changed underneath it. If that cwd is anything else, every lookup fails as
"file not found" with nothing pointing at the cause.

So the handler supplies its own `IoProvider` instead — a thin wrapper over
`LocalIoProvider` that rewrites a configured URL prefix to a local root:

```
pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE
  ->  <archive-root>/ptdata/ptdatac/19887x/198873.PCE
```

**Rewrite in `open()` and `stat()`, not in `resolve()`.** `IoProvider::resolve`
looks like the hook and is not: `ShotLocator` calls `io_provider->stat(path)`
and hands the path to `ShotFile` directly, and nothing on this path calls
`resolve` at all. Overriding only `resolve` would compile, do nothing, and fail
exactly like a missing mount.

This is the consumer half of the fix the sandbox spec defers ("the real fix is
for the indexer to record archive-relative paths, with each consumer prepending
its own root") and it needs no index regeneration. It also drops two of the
sandbox's inherited caveats: no cwd dependency, and the federation host stops
being encoded in a directory name — it becomes a configured string the operator
can see and change.

Keep the symlink chain in mind as the fallback: if the origin's cwd turns out
to be `/` and the rewrite provider proves troublesome, the zero-code path still
works.

### Configuration comes from the exthandler line, not the environment

The sandbox sets `PTDATA_JSON_INDEX_DIR` and friends as container environment
variables because it owns the container. This handler is loaded into Pelican's
origin, whose environment belongs to Pelican — but the config fragment is
already ours, and the existing handler already takes `prefix`, `host`, `port`,
`auth` and `authpath` that way. So:

```
http.exthandler mdsip /plugins/libXrdHttpMdsip.so \
    prefix=/mdsip,host=<mdsip-host>,port=8000,auth=xrootd,authpath=/fdp-d3d/archives/mdsplus,\
    pointprefix=/fdp-d3d/ptdata,pointauthpath=/fdp-d3d/archives/ptdata,\
    pointindex=/ptdata-index,pointindexpattern=json_indexes_*,\
    pointurlprefix=pelican://osg-htc.org:443/fdp-d3d/archives,pointroot=/fdp-archives
```

Omitting `pointprefix` leaves the point endpoint off entirely, so this ships
dark and is turned on deliberately — and an origin that only wants the relay is
unaffected.

`JsonIndexPlugin::Config` takes an explicit `index_dir` and `IoProvider*`, so
none of this needs an env var. `PTDATA_PTSERVERS=none` is still worth setting
in the container as defence in depth, but the handler builds its `ShotLocator`
with an empty `point_providers` vector regardless, so an index miss cannot
become a socket attempt even if the environment says otherwise.

### What this costs the origin container

Two read-only bind mounts, and no more:

| Host | Container | Contents |
|---|---|---|
| `<archive>/ptdata` | `<pointroot>/ptdata` | shotfiles, ro |
| `<archive>/index/json` | `/ptdata-index` | index snapshots, ro |

### Reading the record

`ShotLocator` is public (`ptdata/shot_locator.h`), and `locate_point` returns
`Located{ variant<ShotFile, FetchedPoint>, source_description }`. Using it
directly gives the handler both halves of the response in one lookup: the
`ShotFile` to `find` and `read_point` from, and a `source_description` of
`"index: <path>"` naming the file that answered.

`PtDataReader::read_point_bytes` is the simpler call and returns only bytes.
Either works; the locator is preferred because of the header below.

### `X-Ptdata-Extension` needs no ptdata change

The earlier revision called this a gap requiring a new API. It is not, once
resolution is index-first. `IndexPlugin::resolve(pointname, shot)` returns the
path it chose, and the extension is its suffix — reachable either from that
call directly or from `Located::source_description`.

Note that tier 1 **ignores the requested source**: `resolve` takes only
`(pointname, shot)`, so the index decides which extension holds a pointname.
That is compatible with the contract — `?ext` is a hint, and a server is free
to resolve — but it means the hint is not consulted at all in this deployment,
and `X-Ptdata-Extension` is how the client learns what actually answered.

## Threading

XrdHttp dispatches ext-handler requests on its own threads and `PtDataReader`
makes no thread-safety promise. Use a `thread_local` reader: no contention, no
pool to size, and the reader holds no persistent file handles — ptdata#43's
reopen-per-fetch is a *remote* round-trip problem, and on the origin those
reopens are local `open`/`read` calls.

Confirm the thread count XrdHttp actually uses before assuming `thread_local` is
cheap here.

## Packaging and deployment

The handler links libptd3d, so the origin needs it. Two options:

1. **Bundle it into the `.so`.** ptdata's CMake builds `ptd3d` as `SHARED`
   only, so this needs a static target upstream. One artifact to bind-mount,
   no version skew between two mounted libraries.
2. **Mount `libptd3d.so` beside the handler.** No upstream change; two mounts
   and a version-skew surface.

Recommend (1) if the upstream static target is cheap, since the deployment
delta is the expensive part of this change, not the build.

On the origin the delta is otherwise small: the existing `http.exthandler` line
gains the point prefix and its authpath, and a restart. No new port, no new TLS,
no new authorization model, no new namespace export — unlike the relay, which
needed `/mdsip` exported with `Writes` before the director would route `PUT`.

**ABI.** Production origin is `origin:v7.23.3` carrying XRootD v5.9.1 while the
plugin builds against v5.9.2. That mismatch is known to work for the relay,
tested in that image. Re-check when either moves: XRootD refuses plugins it
considers incompatible, and the failure is a startup abort.

## Testing

- **Contract equivalence.** ptdata#46 ships seven tests against a stub
  implementing this contract. Point them at the real handler and they become
  the server's acceptance suite — particularly "the record matches a local
  reader on data, times and units".
- **Authorization.** The relay's own failure mode is the one to copy tests
  from: unauthenticated GET under the point prefix must be refused, and a
  token valid for `/fdp-d3d/archives/ptdata` must be accepted. `tests/fed/`
  already probes this shape for the relay.
- **The extension header**, once the ptdata addition lands: a point present in
  a non-default extension must come back with `X-Ptdata-Extension` naming it.

## Out of scope

- Following a Pelican director's 307 (ptdata#45). Until then an endpoint is an
  origin addressed directly.
- Any change to the relay's own behaviour.
- Serving anything but single point records: no header-only call, no batching.
  Batching is the obvious next lever if round trips still dominate, and
  deliberately not designed in before there is a measurement asking for it.
