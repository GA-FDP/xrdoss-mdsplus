# Spike: mdsip tunnelled over HTTP

**Date:** 2026-08-10 · **Result:** the model works; session affinity is
mandatory · **Code:** [`spikes/relay_spike.py`](spikes/relay_spike.py)

## The question

A translating transport — one that turns `conn.get()` into a cacheable object
GET — has to speak the mdsip protocol, construct batch payloads, and fabricate
replies. A *tunnel* would instead relay bytes and let a real mdsip server do all
of it.

But an HTTP request/response cannot carry an open-ended bidirectional stream. It
carries one blob up and one blob down. So a tunnel only works if the mdsip
conversation is strictly:

```
client sends a COMPLETE call (nargs messages)  ->  server sends ONE answer
```

That was the unknown. Everything else — loading a handler, reading a body,
writing binary back — is well-trodden.

## Result: the model holds

`spikes/relay_spike.py` sits between a stock `MDSplus.Connection` and a real
mdsip server, accumulates whole calls (reading the 48-byte header for `nargs`
and `descriptor_idx`), forwards each as a unit, and relays back exactly one
answer. That is precisely what an ext handler would do with `BuffgetData` /
`SendSimpleResp`.

Against real `efit01` shot 190000, through the relay, with **no client changes
whatsoever**:

```
\ipmhd             MATCH  shape=(256,)
\q95               MATCH  shape=(256,)
dim_of(\ipmhd)     MATCH  shape=(256,)
getMany            MATCH
psirz (4.3 MB)     (256, 65, 65)
```

**`c.get()` works.** That matters more than it looks: `get()` is the dominant
call in real code (55 sites locally against 0 for `getMany`), and in the
translating design it was the expensive case — it would have required linking
`MdsObjectsCppShr` into the client to build one-item batch payloads. Here it
costs nothing, because a real server handles it.

So does everything else, including calls we had written off as out of scope.
There is no fabrication anywhere, and semantics match `atlas.gat.com` because
they *are* `atlas.gat.com`'s, relayed.

## Result: session affinity is mandatory

A variant that opened a fresh mdsip connection per call fails immediately:

```
STATELESS RELAY FAILS: MDSplusERROR %MDSPLUS-E-ERROR
```

mdsip is **stateful**: `openTree` sets the tree context *on the connection*. A
relay that does not keep one mdsip connection per client session loses that
context and every subsequent `get()` fails.

So the HTTP relay needs:

- a session token minted at connect and carried on each request
- a map from session to held mdsip socket
- idle timeout and cleanup for abandoned sessions
- stickiness to one origin — sessions cannot be load-balanced

That is real complexity, but bounded and well-precedented: MDSplus's own (now
bit-rotted) `http://` transport did exactly this, keeping session state in a
per-connection temp directory.

## The XRootD side

`XrdHttpExtHandler` is a clean fit:

```cpp
virtual bool MatchesPath(const char *verb, const char *path) = 0;
virtual int  ProcessReq(XrdHttpExtReq &) = 0;
// XrdHttpExtReq: verb, resource, headers,
//                BuffgetData(...)      read the request body
//                SendSimpleResp(...)   write an arbitrary binary response
```

The mechanism is proven in production — Pelican's generated config already
loads two of them (`libXrdHttpPelican.so`, `libXrdHttpTPC.so`) — and it rides
the origin's existing port, TLS and SciTokens.

One wrinkle: **`XrdHttpExtHandler.hh` is not installed** by `xrootd-devel` or
conda. It exists only in the XRootD source tree, so it would need vendoring the
same way `third_party/mdsplus` handles the MDSplus headers. Being uninstalled
also hints it is treated as less stable than the Oss interface; worth pinning
the XRootD version if we depend on it.

## What this changes

| | translating transport | tunnel |
|---|---|---|
| `conn.get()` | needs `MdsObjectsCppShr` + payload construction | free |
| `put`, events, `setDefault` | unsupported | free |
| reply fabrication | every reply | none |
| federation caching | **yes** | **no** |
| server side | already built | ext handler, unbuilt |
| client side | ~several hundred lines, delicate | ~150 lines, dumb |

Caching is the whole trade. It cannot be recovered in a tunnel: a cache keys on
a URL, and a tunnel has none. Moving the intelligence to the origin does not
help either, because the request that crossed the network was still a POST.

Given the measured picture — real code fetches one ~1 KB signal at a time, the
CHEP benchmark has the origin beating a cache at that size, and one-pass scans
have ~0% hit rates — the caching we would give up is worth less in this workload
than it first appeared.

## Suggested shape

Two tracks over one mdsip deployment, which is why nothing already built is
wasted:

- **thin clients → tunnel** (ext handler + dumb transport): full compatibility,
  no fabrication, unblocks existing code
- **path-based clients → virtual files** (already built and verified): caching
  for `toksearch`, `curl`, `pelicanfs` — the callers who can actually be told to
  batch, and therefore the ones caching pays for

## Reproducing

```bash
mdsip -m -p 8300 -h hosts &                    # hosts: "* | SELF"
python docs/spikes/relay_spike.py 8301 8300 &  # relay in front of it
python -c "from MDSplus import Connection; c=Connection('localhost:8301'); ..."
```
