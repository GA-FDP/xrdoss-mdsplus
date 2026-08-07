# Spike: replace the evaluator with mdsip

**Date:** 2026-08-06 · **Result:** works, and is the better design
· **Code:** [`mdsip-spike.cc`](mdsip-spike.cc)

## The question

We wrote a Python evaluator daemon that parses a request, opens a tree,
evaluates TDI, and serializes the result. MDSplus already ships a server that
does exactly that. Why not use it?

## What MDSplus already provides

`GetMany.execute()` does not send N requests. It calls one server-side TDI
function (`connection.py:396`):

```python
ans = self.connection.get("GetManyExecute($)", self.serialize())
```

which resolves to `tdi/remote/GetManyExecute.fun` → `MdsObjectsCppShr` →
`getManyObj()` (`mdsobjects/cpp/mdsipobjects.cpp:121`). That walks a list of
`{name, exp, args}`, evaluates each, and returns a serialized dictionary of
`{name: {value|error}}` — the same shape, and the same in-band error
convention, that our evaluator reimplements.

Crucially `GetManyExecute` **returns already-serialized bytes**
(`mdsdata.c:921` calls `MdsSerializeDscOut` before returning). So the answer is
byte-for-byte what the plugin would write out as the virtual file's payload.

## What the spike shows

`docs/mdsip-spike.cc` links **`MdsIpShr` only** — no MDSplus object model, no
`treeshr`, no TDI evaluation in the caller. It:

1. `ConnectToMds("localhost:8000")`
2. `MdsOpen(id, tree, shot)`
3. wraps the request bytes in a plain `struct descriptor_a` (`DTYPE_B`,
   `CLASS_A`) — a POD from `mdsdescrip.h`, nothing heavier
4. `MdsValueDsc(id, "GetManyExecute($)", &arg, &answer, NULL)`
5. writes the answer bytes out verbatim

Against a real `efit01` shot 190000, a five-item batch returned **4,329,910
bytes**, and every item matched a direct MDSplus read exactly:

```
ip     MATCH  shape=(256,)         dtype=float32
q95    MATCH  shape=(256,)         dtype=float32
psirz  MATCH  shape=(256, 65, 65)  dtype=float32
times  MATCH  shape=(256,)         dtype=float32
bad    in-band error
```

**The `data()` wrap turns out to be unnecessary.** Unwrapped `\ipmhd` came back
correctly through mdsip, with no `%TREE-W-NOT_OPEN`. So mdsip is *more* correct
out of the box than our evaluator was — our wrap was compensating for a problem
mdsip does not have.

## What this would change

The request format becomes MDSplus's own — the path carries
`GetMany.serialize()` output — and the plugin never interprets it. That deletes:

- `src/Request.hh` / `.cc` (our invented canonical format)
- `src/EvalClient.hh` / `.cc`
- `evaluator/` entirely

keeping `Base64Url`, `TdiPath`, `ResultCache` and the Oss wiring. Roughly half
the code, replaced by upstream. It also makes the Part 4 client transport nearly
trivial, since the client already has `GetMany.serialize()`.

And spec §1's compatibility goal — behave as `atlas.gat.com` does — becomes true
by construction rather than by testing.

## Costs, and what is now known about them

| Concern | Status after the spike |
|---|---|
| Per-request timeout | **Solved.** `MdsValueDsc` takes none, but `GetAnswerInfoTO(..., int timeout)` is exported (`mdsip_connections.h:397`). Use `SendArg` + `GetAnswerInfoTO` instead of `MdsValueDsc`. |
| Result-size cap | Still ours to enforce, now in the plugin after the answer arrives. |
| Byte-stability of cached objects | Now depends on the MDSplus version, not on us. An upgrade changing serialization would leave old and new bytes under one path. Pin the mdsip version alongside the plugin. |
| MDSplus code in the XRootD process | `MdsIpShr` is a socket protocol client. It does not open trees or evaluate TDI, so the reason for keeping MDSplus out — crashes and hangs in `treeshr` — does not apply. |
| Fork per request | `GetManyExecute` uses `static EMPTYXD(xd)` and is not thread-safe, so mdsip needs `-m` (process per connection). Buys parallelism our lock-serialised daemon lacks, at higher per-request cost. Unmeasured. |

## Reproducing

```bash
printf '* | MAP_TO_LOCAL\n' > /tmp/mdsip.hosts
P=$(pixi run printenv CONDA_PREFIX | tail -1)
pixi run env efit01_path=/tmp/fdp-trees MDS_PATH="$P/tdi;$P/tdi/remote" \
  mdsip -m -p 8000 -h /tmp/mdsip.hosts &
```

`MDS_PATH` must include `tdi/remote` or the server cannot find
`GetManyExecute.fun` and fails with `%TDI-E-UNKNOWN_VAR`.

Two gotchas that cost time: `ConnectToMds` returns **0 as a valid connection
id** (only `-1` is failure), and the descriptor field is `class_`, not `dclass`.
