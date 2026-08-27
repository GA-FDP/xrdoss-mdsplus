# `libMdsIpFDP.so` — the MDSplus thin-client transport

**Status:** built and verified end to end.

## Goal, met

Existing DIII-D code that speaks only the MDSplus thin client reaches FDP by
changing **one string**:

```python
conn = MDSplus.Connection(
    'fdp://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip')  # was 'atlas.gat.com'
conn.openTree('efit01', 190000)
conn.get(r'\ipmhd')
```

**Use that hostname, not `d3d-origin.gat.com`.** The origin's Let's Encrypt
certificate carries exactly one SAN:

```
subject = CN = fdp-d3d-origin.nationalresearchplatform.org
X509v3 Subject Alternative Name: DNS:fdp-d3d-origin.nationalresearchplatform.org
```

so connecting by the GA-internal name fails hostname verification. This
document previously used `d3d-origin.gat.com` in that example, which cannot
work from anywhere.

The transport says so plainly:

```
fdp transport: POST https://.../mdsip/connect:
    SSL peer certificate or SSH remote key was not OK
```

but MDSplus's Python wrapper discards it and raises a bare
`Error connecting to fdp://...`, which reads like a network or token problem.
If a connect fails, run the `curl` check below **without** `-k` — with it, the
verification that is failing is exactly the one you switched off:

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X PUT --data-binary '' \
  https://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip/connect
# 401 = reachable and enforcing auth (good). 000 = TLS failure.
```

Verified from omega06 on 2026-08-17: trees, `PTDATA2`, `PTNPTS`, and a stored
record (`\TECE01` in `ece`, whose record calls `PTDATA2`) all return data.

Nothing else about the client changes. MDSplus loads the library itself:
`parse_host()` splits `<scheme>://<host>`, and `LoadIo()` uppercases the scheme,
builds the image name `"MdsIp" + SCHEME`, and resolves the symbol `Io`
(`mdstcpip/mdsipshr/LoadIo.c:41-62`). `LibFindImageSymbol_C` then `dlopen`s
`"lib" + "MdsIpFDP" + ".so"` through the normal loader search, with
`MDSPLUS_LIBRARY_PATH` as a fallback.

So the file must be named **`libMdsIpFDP.so` exactly** — no version suffix.
That is the opposite of the XRootD plugins in this repo, where
`XrdOucPinLoader` appends one and the config must *omit* it.

## What it does

`IoRoutines` is a **byte-stream** vtable — `connect`, `send`, `recv`,
`disconnect` — while an HTTP exchange carries one blob each way. Bridging that
is the whole job:

| Vtable call | What happens |
|---|---|
| `connect` | parse `host[:port][/prefix]`, `PUT /connect`, keep the session token |
| `send` | buffer bytes until a **complete call** is assembled, then `PUT /msg` and hold the answer |
| `recv` | drain the held answer |
| `disconnect` | `PUT /close`, free the session |

`flush`, `listen`, `authorize`, `reuseCheck` and `check` are NULL. All are
optional — the in-tree GSI transport leaves four of them NULL the same way.
`reuseCheck` being NULL is deliberate rather than lazy: connections must never
be silently shared, because the relay keys server-side state to a session token
and two callers on one session would interleave into a byte stream that cannot
survive it.

**Nothing is interpreted or fabricated.** A real mdsip server behind the relay
produces every byte returned, which is why `get`, `getMany`, `put`, `setDefault`
and everything else work without being enumerated anywhere.

## Surviving a session that dies underneath you

A session can go away without the caller doing anything wrong: the relay retires
one whenever a call fails — a call that overran its `timeout=`, most of all —
and reaps one that has been idle past `idle=`. Every later call then comes back
`502 unknown or expired session`, and that used to be terminal. Nothing in the
stack redialled, and `toksearch`'s `MdsConnectionRegistry` hands the same
`MDSplus.Connection` to every caller in the process for its whole life, so one
lost session meant every remaining fetch failed. Measured: GA-FDP/imas_composer
CI run 33033220932, one slow call and then 25 dead ones on the same worker,
while the other worker's session carried on fine.

`Call()` now redials once on that specific 502 and retries. What makes it more
than a retry loop is that mdsip is stateful, so a fresh session has to be told
what the old one knew — in order:

1. **The login.** MDSplus sends it as the first call on a connection; the relay
   only opens a TCP socket and does not speak the protocol, so a session that
   has not been given it is one mdsip answers nothing on. Skipping it produces
   `mdsip did not answer`, which reads like a timeout and is not one.
2. **The tree context.** `openTree` is `TreeOpen($,$)` — an ordinary call, whose
   bytes the transport already buffered — so replaying the latest one restores
   it exactly.

What cannot be restored is TDI's private variables. After `get("_sig = ...")` a
new session answers `get("_sig")` with something else, and quietly returning
that would be worse than the dead tunnel this exists to avoid. So an expression
carrying an `=` that is not a comparison marks the session unreplayable and the
redial is declined, with the reason said out loud. `CallExpression` reads the
expression back out of descriptor 0 of the call's own bytes, and returns empty
for anything it cannot parse — which classifies as unreplayable too, because
"unreadable" and "harmless" are not the same thing.

The bearer token is re-read on every redial rather than reused. FDP access
tokens are short-lived and `/connect` is the only point at which the relay
checks one, so a long pipeline's original token may well be expired by the time
a redial needs it.

## The part that needed care

**Call framing.** `send()` receives whatever chunking MDSplus chose — not one
message, not one call, and it will split a header mid-field. A call is `nargs`
messages, each a 48-byte header plus payload, ending at
`descriptor_idx == nargs - 1`.

`CallAssembler` (`src/MdsipCall.cc`) does only that, and is unit tested without
a server because it is the piece most likely to harbour a silent off-by-one:
byte-at-a-time feeding, splits mid-header and mid-payload, a 4 MB call in 8 KB
chunks, successive calls, and trailing bytes belonging to the next call.

Two traps it exists to avoid:

- **`nargs` is `unsigned char`.** A naive `idx >= nargs - 1` wraps to 255 for a
  zero-argument call, so the transport waits forever — a hang rather than an
  error, which is the worst way to be wrong. The comparison is done in `int`.
- **A message shorter than its own header** would leave the scan offset
  standing still or running backwards. It is rejected, and the rejection is
  sticky: a desynchronised stream cannot be resynchronised, and framing the
  next call from the wrong offset would produce plausible garbage.

Field offsets are `static_assert`ed against a verbatim mirror of `MsgHdr`
(`mdstcpip/mdsip_connections.h`) rather than trusted, because hand-transcribed
offsets are exactly how a header gets mis-parsed silently.

## Configuration

| Setting | Where |
|---|---|
| Target | the connection string: `fdp://host[:port][/prefix]`, prefix defaulting to `/mdsip` |
| Bearer token | `BEARER_TOKEN`, then `~/.fdp/token` — the same precedence the `fdp` CLI uses |
| Scheme | `https` always, unless `FDP_TUNNEL_SCHEME=http` (**test only**, for a local relay with no TLS) |
| CA | `FDP_TUNNEL_CAINFO` for a custom CA; `FDP_TUNNEL_INSECURE=1` skips verification (**test only**) |
| Method | `PUT`, overridable with `FDP_TUNNEL_METHOD` for debugging |

**Why PUT and not POST.** The Pelican director will not route POST — 404 at the
namespace path, 405 at its API endpoint, and its own CORS header advertises only
`GET, PUT, OPTIONS, PROPFIND`. It routes PUT with a 307 that preserves method
and body. Both reach a directly-addressed origin, so PUT is the one that works
everywhere. Measured in `tests/fed/probe_federation_post.sh`.

`https` is the default rather than something the target string can quietly
downgrade, so an insecure deployment is not the easy mistake. A missing token
sends no `Authorization` header at all, which is right for an unauthenticated
local relay and yields a clean 401 against a real origin.

The curl handle is reused for the life of the session, so TLS is negotiated once
rather than per signal — which matters, because the workload this exists for is
thousands of one-signal `get()` calls.

## Verified

`pixi run relay-e2e` runs the full chain — stock `MDSplus.Connection` →
`libMdsIpFDP.so` → XRootD relay → mdsip — and compares every result against the
same call made directly to mdsip:

```
\ipmhd, \q95, dim_of(\ipmhd), 10+32, \psirz (4.3 MB), getMany, re-open   all MATCH
```

`MDSIP_SANDBOX=1 pixi run relay-e2e` does it against the sandboxed server, which
is the production topology.

**Through a real federation**, `pixi run fed-post` points a stock
`MDSplus.Connection` at the *director* rather than an origin:

```
target: fdp://localhost:8444/mdsip
\ipmhd OK (256,) · \q95 OK · 10+32 OK · getMany OK 256 samples · \psirz OK (256, 65, 65)
=> A STOCK MDSplus.Connection WORKS THROUGH THE DIRECTOR
```

so a 4.3 MB answer survives the 307 redirect intact.

Lifecycle and error paths are covered separately
(`tests/integration/transport_edge_cases.py`): 25 sequential `get()` on one
session, state surviving repeated `openTree`, 8 independent connections, 40
connect/disconnect cycles, errors raising as errors, the session still usable
afterwards, and an unreachable origin failing rather than hanging.

Recovery is driven for real rather than mocked
(`tests/integration/relay_session_recovery.py`): a second handler instance
configured `idle=1` reaps the session out from under a client that is behaving
perfectly, and the next read must return the *same* array — which only holds if
the tree context came back with it. The check fails if the relay log shows no
lost session at all, because a recovery test that never loses anything passes
for the wrong reason.

Session leaks are checked by counting mdsip processes — `mdsip -m` forks per
connection, so a leaked relay session is a leaked process. Measured 1 before and
1 after ~50 sessions. This is the only check there that would notice: the
relay's own session cap is 256, so the churn would pass just as happily while
leaking every one of them.

## Known limits

- **One call in flight per connection.** `MDSplus.Connection` is not concurrent
  anyway, and the relay answers a second concurrent call on one session with
  409 rather than interleaving it.
- **No events.** Async server push cannot cross a request/response tunnel, and
  `Connection` has no event API regardless.
- **The bearer token is checked once, at connect.** The relay delegates to the
  origin's authorization there and then trusts the session token, so a session
  can outlive the token that opened it. See `docs/security.md`.
- **Sessions are sticky to one origin.** The director picks an origin at
  `/connect` and every later call must reach the same one. With a single origin
  this is invisible; with several, a client whose calls are spread across them
  breaks. Untested against a multi-origin federation.
