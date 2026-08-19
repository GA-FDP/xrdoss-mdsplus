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

`PtDataReader::read_point_bytes(shot, source, pointname)` — made public in
ptdata#46 for exactly this. It locates, opens, finds and reads one record, and
reads only what it needs rather than pulling a whole shot file (a `.MAG` can be
309 MB).

libptd3d is built `-DPTDATA_WITH_FDPIO=OFF`: the origin reads local files, and
a library physically incapable of remote I/O cannot be turned into an egress
path. `Containerfile.mdsip` already builds it this way, and
`scripts/fetch-ptdata-src.sh` already delivers the source into a build context
without putting credentials in an image — both reusable.

### Extension resolution comes for free

`ShotLocator` already does what the contract asks. Tier 2a tries the requested
source pointname-aware, so a shot file that lacks the pointname falls through
rather than answering; tier 2b (`scan_for_pointname`, legacy PTSEARCH0)
enumerates `<shot>.*` and returns the first whose directory holds it.

Crucially that scan is **local-SYS_D3 only** — `IoProvider::list_files` is a
no-op for remote providers — which is precisely the origin's situation. So the
handler passes `?ext` straight through as `source` and lets ptdata resolve. No
candidate loop server-side. (ptdata's own test stub loops a hardcoded candidate
list; the real server should not, and the difference is worth knowing when
reading those tests.)

### One gap: `X-Ptdata-Extension`

The resolved extension is not recoverable from `read_point_bytes`. It exists
internally — `ShotLocator::Located::source_description` carries `"file: <path>"`
— but that is not on any public return, and `FetchedPoint::actual_extension` is
documented as `nullopt` for local file readers precisely because the file path
never had a way to report it.

**Prerequisite: a small ptdata addition** giving `read_point_bytes` a way to
report the extension it resolved — an overload with an out-parameter, or a
struct return. Parsing it back out of `source_description` would work and should
not be done.

The header is informational: it lets a client detect server-side fallthrough
(`.MAG` → `.MGB`). If it is absent the client sets `nullopt`, which is what the
local-file path already does, so the contract degrades gracefully and this does
not block a first cut. It should not ship missing, though — a contract term that
is never populated rots.

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
