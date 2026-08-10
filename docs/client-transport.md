# `libMdsIpFDP.so` — the MDSplus thin-client transport

**Status:** built and verified end to end.

## Goal, met

Existing DIII-D code that speaks only the MDSplus thin client reaches FDP by
changing **one string**:

```python
conn = MDSplus.Connection('fdp://d3d-origin.gat.com:8443/mdsip')  # was 'atlas.gat.com'
conn.openTree('efit01', 190000)
conn.get(r'\ipmhd')
```

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
| `connect` | parse `host[:port][/prefix]`, `POST /connect`, keep the session token |
| `send` | buffer bytes until a **complete call** is assembled, then `POST /msg` and hold the answer |
| `recv` | drain the held answer |
| `disconnect` | `POST /close`, free the session |

`flush`, `listen`, `authorize`, `reuseCheck` and `check` are NULL. All are
optional — the in-tree GSI transport leaves four of them NULL the same way.
`reuseCheck` being NULL is deliberate rather than lazy: connections must never
be silently shared, because the relay keys server-side state to a session token
and two callers on one session would interleave into a byte stream that cannot
survive it.

**Nothing is interpreted or fabricated.** A real mdsip server behind the relay
produces every byte returned, which is why `get`, `getMany`, `put`, `setDefault`
and everything else work without being enumerated anywhere.

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

Lifecycle and error paths are covered separately
(`tests/integration/transport_edge_cases.py`): 25 sequential `get()` on one
session, state surviving repeated `openTree`, 8 independent connections, 40
connect/disconnect cycles, errors raising as errors, the session still usable
afterwards, and an unreachable origin failing rather than hanging.

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
- **Sessions are sticky to one origin** and cannot be load balanced — a property
  of the relay, inherited here.
- **The federation path is untested.** Everything above was verified against an
  origin directly. Whether a `POST` under a namespace prefix routes through the
  Pelican director the way a `GET` does is an open question, and the reason the
  target string accepts an arbitrary prefix.
