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

---

# Follow-up: static-linking experiment (2026-08-07)

**Question:** can the plugin static-link `MdsIpShr` so the origin image needs no
MDSplus runtime?

**Answer: yes technically, but it must be built against the target image.**

`libMdsIpShr.a` alone is not enough — the objects the linker pulls in
(`MdsValue.c.o`, `Connections.c.o`) reference `MdsShr` and `TdiShr`, leaving
`MdsGetMsg`, `MdsSerializeDscIn`, `TdiRestoreContext` and friends unresolved.
Linking the full chain does work:

```
libMdsIpShr.a libMdsShr.a libTdiShr.a libTreeShr.a  (repeat the first two for
                                                     circular refs) -lz -lxml2
```

That produces a 2.6 MB plugin whose only `DT_NEEDED` entries are
`libXrdUtils`, `libz`, `libxml2`, and the C/C++ runtime — **no MDSplus
libraries at all**. It passes the full real-tree suite: `\ipmhd`, `\q95` and
`\psirz` all match a direct read.

**The blocker is toolchain, not linkage.** A conda-built plugin still cannot
load in the stock Pelican image:

| | conda build | Pelican image (EL9) |
|---|---|---|
| libxml2 soname | `libxml2.so.16` | `libxml2.so.2` |
| `GLIBCXX_3.4.30` in libstdc++ | present | **absent** |

So the packaging conclusion is to **build in an image derived from the Pelican
origin image** (plus `xrootd-devel` and the MDSplus static libs), producing a
plugin that drops into the **stock** runtime image. That is a better outcome
than a derived runtime image: the origin stays as shipped.

**Correction to the motivation.** When proposing this experiment I said static
linking would mean "MDSplus exists only inside the sandboxed mdsip container."
That was wrong. The mdsip *client* code is inside the plugin either way —
statically welded in rather than loaded as separate `.so` files. Static linking
buys packaging simplicity, **not** isolation. The isolation argument rests
entirely on evaluation happening in the mdsip process, which is unchanged.


---

# Correction: static linking does NOT remove the MDSplus runtime dependency

The section above concluded that a statically-linked plugin needs nothing from
MDSplus at runtime, on the evidence that its `DT_NEEDED` list contains no
MDSplus libraries. **`DT_NEEDED` was the wrong thing to measure.**

`ConnectToMds` calls `LoadIo("tcp")`, which does
`LibFindImageSymbol_C("MdsIpTCP", "Io")` — it **`dlopen`s `libMdsIpTCP.so` by
name at runtime** (`mdstcpip/mdsipshr/LoadIo.c:54-62`). A static link cannot
possibly include a library discovered by filename. When it is absent, `LoadIo`
silently falls back to `tunnel_routines` and the connect fails with nothing
more informative than `ConnectToMds(...) failed`.

Confirmed in a real federation: the statically-linked plugin loads cleanly into
the stock origin image, resolves every symbol, and then cannot connect, because
the image has no `libMdsIpTCP.so`.

`libMdsIpTCP.so` ships in `mdsplus-kernel_bin`, so **the origin image needs the
MDSplus runtime installed** regardless of how the plugin is linked. Static
linking still reduces the reconciliation surface, but it does not eliminate the
dependency, and the packaging plan must assume a derived *runtime* image.

## Two deployment facts learned the hard way

**`mdsip.hosts` must map to `SELF` when mdsip runs unprivileged.** `MAP_TO_LOCAL`
makes the server attempt a setuid to the *client's* username — which, when the
client is XRootD inside the Pelican container, is `xrootd`, an account that does
not exist on the host. The connection is refused with no server-side log at all.
`* | SELF` tells mdsip not to switch user (`CheckClient.c:76`). A local test
where the client happens to run as the same user as the server will pass with
`MAP_TO_LOCAL` and hide this completely.

**`MDS_PATH` must include `tdi/remote`**, or `GetManyExecute` is not found and
every request fails with `%TDI-E-UNKNOWN_VAR`.


---

# Follow-up: per-request timeout (2026-08-10)

`MdsValueDsc` takes no timeout, so a slow or hostile expression pinned an XRootD
thread indefinitely — the cheapest denial of service in the system. Replaced
with `SendDsc` x2 + `GetAnswerInfoTO(..., timeout)`.

Verified: `wait(20)` with a 3-second budget returns at 3 seconds.

**One trap worth recording.** `GetAnswerInfoTO` hands back mdsip's *own*
serialization wrapping the payload (`dtype == 24`, `DTYPE_SERIAL` from
`ipdesc.h`), not the bytes `GetManyExecute` produced. `MdsValueDsc` was peeling
that layer internally via `MdsSerializeDscIn`, and dropping it silently returned
the outer wrapper — the real-tree test caught it immediately as a data
mismatch. An earlier note here claimed the switch would also avoid a
deserialize round trip; it does not, and `MdsValue.c`'s handling was right all
along.

A result-size cap rides along on the same path. It cannot prevent the bytes
being received — the API offers no way to bound that — but it stops an absurd
result being cached and served, which is what actually costs the origin.
