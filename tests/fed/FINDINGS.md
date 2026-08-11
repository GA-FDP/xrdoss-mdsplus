# Task 0 findings — local Pelican federation harness

Run 2026-08-06 on omega06.gat.com. All three load-bearing premises of the design
were validated locally, with no access to the production origin.

## Environment

| Thing | Value |
|---|---|
| Image | `hub.opensciencegrid.org/pelican_platform/origin:latest` (868 MB, anonymously pullable) |
| Pelican | 7.26.0, build commit `4b5c65aa57f6f11f98305c1a333711f65c70b9b3` |
| **XRootD in image** | **v5.9.2** |
| XRootD in conda env | **v5.9.2** — identical, so the ABI concern for Task 1 Step 6 is resolved |
| podman | 4.9.4, rootless, graphroot already on `/local-scratch` (local disk, not NFS) |

## Premise 1 — `Xrootd.ConfigFile` + `ofs.osslib ++` works

Confirmed. The generated `/run/pelican/xrootd/origin/xrootd.cfg` ends with:

```
# Continue onto the next set of configuration
continue /etc/pelican/xrootd-extra.cfg
```

and the XRootD log shows a three-deep stack, our layer included:

```
ofs.osslib default
ofs.osslib ++ libXrdOssStats.so                 <- Pelican's own
ofs.osslib ++ /usr/lib64/libXrdOssStats-5.so    <- ours, via Xrootd.ConfigFile
Plugin loaded fsstats v5.9.2 from osslib /usr/lib64/libXrdOssStats-5.so
```

Pass-through survived: `/test/hello.txt` still returned `hello-federation`.

## Premise 2 — base64url survives the director

A **240-character** base64url segment came back from the director verbatim,
including the alphabet-specific `-` and `_`:

```
location: https://omega06.gat.com:8443/test/AAECAw...PD0-P0BB...fn-AgYKD...sbKz
```

and the object's bytes were served correctly.

Two controls prove the test discriminates:

| Control | Result |
|---|---|
| Segment of all `/` (standard base64 worst case) | **Collapsed entirely** — `location: .../test`, the whole segment vanished |
| `AAA%2FBBB` — can percent-encoding protect a slash? | **No** — became `AAA/BBB`, a real separator |

This is the empirical basis for spec §4.4's requirement that the alphabet
exclude `/` and `%`. Standard base64 would be catastrophically broken here, not
merely awkward.

## Premise 3 — bearer tokens do not fragment the cache key

`http.header2cgi Authorization authz` is present at line 27 of the generated
config. Effect on the director's redirect:

```
without token:  .../test/hello.txt
with token:     .../test/hello.txt?authz=dummy.token.value
```

The token lands in the query string, and XrdPfc keys on `URL.GetPath()`, which
splits at the first `?`. Path identical in both cases. Spec §4.2 confirmed.

## Corrections to the plan

1. **The registry module will not start without OIDC client files.** Contents are
   irrelevant; `fedbox.sh` mounts dummy `oidc-client-id` / `oidc-client-secret`.
   Without them the container exits with
   `failed to load server OIDC client config`.

2. **Pelican *does* emit an unconditional `ofs.osslib` for `posix`** —
   `ofs.osslib ++ libXrdOssStats.so`. The plan claimed it emitted none. This is
   good news rather than bad: it proves stacking is Pelican's own normal mode of
   operation, and our `++` simply adds a further layer.

3. **Never put `-5` in an `osslib` path.** XRootD appends the plugin version
   itself. Referencing `/usr/lib64/libXrdOssStats-5.so` produced:

   ```
   Config warning: osslib path '...-5.so' should not use '-5' version syntax in its name!
   Plugin osslib /usr/lib64/libXrdOssStats-5-5.so not found; falling back to ...
   ```

   It worked, but only via a fallback. **Our plugin must install as
   `libXrdOssMdsplus-5.so` and be referenced as `libXrdOssMdsplus.so`.** This
   affects `CMakeLists.txt` and every config fragment.

4. `oss.localroot` is `/run/pelican/xrootd/origin/export`, and each export
   produces an `all.export` line (`/test`, `/tdi`). Relevant to the LFN-prefix
   discovery in Task 9.

5. `ofs.authorize 1` and `ofs.authlib ++ libXrdAccSciTokens.so` are active even
   with `EnablePublicReads: true`.

## Task 1 addendum — ABI probe result

A conda-built plugin **loads cleanly into the image's XRootD**. The derived-image
fallback contemplated in the plan is not needed.

```
Plugin loaded XrdOssMdsplus v5.9.2 from osslib /plugins/libXrdOssMdsplus-5.so
++++++ XrdOssMdsplus scaffold loaded (pass-through only)
```

Both environments are XRootD v5.9.2, which is why this works — worth re-checking
if either side ever moves.

The naming convention from correction 3 above is confirmed in both directions:

| Config says | XRootD loads | Warning |
|---|---|---|
| `/plugins/libXrdOssMdsplus.so` (unsuffixed) | `/plugins/libXrdOssMdsplus-5.so` | **none** |
| `/usr/lib64/libXrdOssStats-5.so` (suffixed) | same file, via fallback | yes, `should not use '-5' version syntax` |

Resulting stack, with pass-through verified working:

```
ofs.osslib default
ofs.osslib ++ libXrdOssStats.so
ofs.osslib ++ /plugins/libXrdOssMdsplus.so
```

MDSplus `serialize()` / `deserialize()` also round-trips in this pixi
environment, including the `{name: {value|error}}` dictionary shape the
evaluator will return, so Task 4's core mechanism is confirmed available here.

## Task 4 addendum — the generated grammar survives the director

Task 0 proved a *synthetic* 240-char base64url segment survives. Task 4 repeated
it with a path actually produced by `BuildTdiPath`, for a deliberately awkward
two-item request (backslashes, parentheses, a comma inside quotes, a space,
arithmetic, and a binary argument containing NUL and 0xFF):

```
/tdi/efit01/00/00/19/00/190000/AQACAAJpcAAAAC5cdG9wLnJlc3VsdHMuYWVxZHNrOnE5NSAq...AwD_AQ
```

The director returned the 126-character final segment verbatim — note the `_`,
which would be `/` in standard base64 and would have split the segment — and the
object's bytes came back. Prefix, digit-pair bucketing, shot number and chunk
all round-trip.

## Task 8 addendum — end-to-end through the federation, and the no-tree sentinel

The plugin receives the LFN **exactly as the federation path**, with no
`oss.localroot` prefix applied:

```
ossmdsplus_Open: /tdi/-/00/00/00/00/0/AQABAAJyMAAAAA1bMS4wLDIuMCwzLjBdAA
```

So `prefix=/tdi` needs no adjustment — the discovery step contemplated in Tasks
9 and 10 is resolved.

A full fetch works: director -> origin -> XRootD -> Oss plugin -> unix socket ->
MDSplus evaluator -> serialized descriptor.

```
HTTP 200, 99 bytes  ->  r0 = [1. 2. 3.]
```

A three-item batch also works, with a deliberately broken expression returned as
an in-band error while the response stays 200. Pass-through is unaffected.

**Gap found by running it.** The path grammar always carries a tree segment, so
there was no way to express "evaluate without opening a tree" — and `/tdi/none/`
means a tree literally named `none`, which fails with
`%TREE-E-NOCURRENT, No current shot number set for this tree`. Treeless
evaluation is legitimate in production (pure computation, or `PTDATA2` which
takes the shot as an argument), so `-` is now a reserved tree segment meaning
"no tree open". It is not a legal MDSplus tree name, so it cannot collide.

## Task 10 addendum — chunking must agree across languages

The federation test initially failed its multi-chunk case with `malformed tdi
path`. The plugin was right: the test's own Python path builder emitted the
whole ~840-character base64 as **one** segment, while the C++ `BuildTdiPath`
chunks at 249. The parser rejected the over-long segment, as it should — such a
segment also exceeds `NAME_MAX` and could never be materialised by a cache.

Same class of problem as the base64url alphabet: **any second implementation of
the path grammar has to agree with the first, or requests silently fail.** The
client transport in Part 4 will be a third. Worth a shared conformance vector
set rather than three hand-written encoders.

With chunking fixed, a 120-element expression occupying **5 path chunks** was
reassembled correctly through the director.

## Task 11 addendum — serialized descriptors are not self-contained

The first test against a real tree failed on decode:

```
MDSplus.mdsExceptions.TreeNOT_OPEN: %TREE-W-NOT_OPEN, Tree not currently open
```

`\ipmhd` does not evaluate to literal data. It evaluates to a descriptor that
still **references tree nodes**:

```
Build_Signal(Build_With_Units([358837.,408932.,...], "A"), *, \ATIME)
```

The values are literal but the time base is the node reference `\ATIME`.
Deserializing that requires the originating tree to be open in the *client*
process — which is never true for a client of this service.

| Form | Deserializes with no tree open? |
|---|---|
| `\ipmhd` | **no** — resolves `\ATIME` |
| `data(\ipmhd)` | yes |
| `make_signal(data(x),*,dim_of(x))` | **no** — `dim_of` is itself a node ref |

**Fix, and it is precedent rather than invention:** MDSplus's own `GetMany`
wraps every expression in `data(...)` when evaluating locally
(`connection.py:392`). The evaluator now does the same, which both fixes decode
and makes our semantics byte-identical to what `GetMany` callers already get.

Consequence: **dimensions and units do not ride along with a value.** The spec
previously claimed a serialized `Signal` would carry them in one object; that
claim was wrong and §6 is corrected. Callers wanting a time base add an explicit
`dim_of()` item, which batching makes nearly free.

With the wrap in place, `\ipmhd`, `\q95` and `\psirz` (4,326,400 bytes) all
match a direct MDSplus read exactly in shape, dtype and values, and
`dim_of(\ipmhd)` returns the real time base of 256 points over 100–5320 ms.

## Migration to mdsip — what the linkage actually costs

The evaluator was replaced by mdsip + `GetManyExecute($)` (see
[`../../docs/mdsip-spike.md`](../../docs/mdsip-spike.md)). One claim made when
justifying that did not survive contact:

> "MdsIpShr is a socket protocol client. It does not open trees or evaluate
> TDI, so the reason for keeping MDSplus out of XRootD does not apply."

`objdump -p libMdsIpShr.so` says otherwise — these are **direct** `DT_NEEDED`
entries, not transitive:

```
NEEDED  libTdiShr.so
NEEDED  libTreeShr.so
NEEDED  libMdsShr.so
```

So linking it loads TDI and TreeShr into the XRootD process. We still never
*call* them — evaluation happens in the mdsip process, and a crash there takes
down mdsip rather than the origin — but the isolation is weaker than claimed:
their static initialisers run and their global state exists in-process.

**Packaging consequence.** The stock Pelican origin image has no MDSplus
runtime, so the plugin cannot load there:

```
Plugin No such file or directory loading osslib /plugins/libXrdOssMdsplus.so
```

Mounting the conda build in does not work either — conda's `libicuuc` needs
`GLIBCXX_3.4.30` and the image ships an older `libstdc++`. A production origin
needs an image with MDSplus installed from OSG/EPEL. Until that exists,
`run_fed_test.sh` skips with an explanation; `tests/integration/` covers the
plugin end to end without a container.

## The director does not route POST; it does route PUT

**Measured 2026-08-11** by `probe_federation_post.sh`, against the local
federation with the relay ext handler loaded on the origin.

| Request | Result |
|---|---|
| `POST https://director/mdsip/connect` | **404**, no redirect |
| `POST https://director/api/v1.0/director/origin/mdsip/connect` | **405 Method Not Allowed** |
| `GET https://director/mdsip/connect` (control) | **307** — the namespace *is* registered and routable |
| `PUT https://director/mdsip/connect` | **307** to the origin, method and body preserved |

The director says so itself, in a header on every response:

```
access-control-allow-methods: GET, PUT, OPTIONS, PROPFIND
```

This is why `libXrdHttpMdsip.so` claims **both POST and PUT**, and why the
client transport sends **PUT** by default: PUT is the only method that works
both against a directly-addressed origin and through the director.

**A control saved this from a wrong conclusion.** The first PUT probe returned
405 and looked like "PUT is not routable either" — but the control, a PUT to
`/test/probe.txt`, returned 405 as well, and that namespace has `Writes: false`.
The 405 was the missing capability, not the method. Adding `Writes` to the
namespace turned the same probe into a 307.

So a production federation must declare the relay namespace **writable** for the
director to route to it. That sounds worse than it is: the ext handler claims
every PUT under its prefix before XRootD consults storage, so no PUT under it
can ever write a file.

## The relay bypasses XRootD authorization entirely

**This is a deployment blocker, found while testing the above.**

`XrdHttpReq.cc:990` dispatches ext handlers at `reqstate == 0` — *before* the
file-access path where `ofs.authorize` and the SciTokens plugin live. So
nothing an ext handler serves is authenticated by XRootD. Measured against the
federation origin, which has TLS configured:

| Request | Result |
|---|---|
| `PUT /mdsip/connect`, no credentials at all | **200** |
| `PUT /mdsip/connect`, garbage bearer token | **200** |
| `PUT /tdi/...` (a path we do *not* claim), no credentials | **403** ← contrast |

The same request was **403 before** the handler claimed PUT and **200 after** —
same URL, same absence of credentials. XRootD was enforcing authorization until
the ext handler took the path away from it.

Since an mdsip session is arbitrary code execution, an unauthenticated relay
must never exist by accident. The handler now **refuses to load without an
explicit `auth=` setting**, and when refused the endpoint falls back to normal
XRootD handling (measured: `PUT` → 403, `POST` → 501), so no unauthenticated
relay endpoint exists. `auth=none` is the only supported value today and logs a
prominent warning; real token validation is not implemented. See
`docs/security.md`.

## Reproduce

```bash
bash tests/fed/fedbox.sh start                    # plain federation
bash tests/fed/fedbox.sh start /tmp/extra.cfg     # with an osslib fragment
bash tests/fed/fedbox.sh start /tmp/extra.cfg /path/to/plugin.so
bash tests/fed/fedbox.sh stop
```

```bash
pixi run fed-post        # the POST/PUT routing and authorization probe
```
