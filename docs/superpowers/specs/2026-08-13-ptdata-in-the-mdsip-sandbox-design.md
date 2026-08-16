# PTData in the mdsip sandbox

**Status:** design, revised 2026-08-13 after measuring real `ical` usage. Not
yet implemented.

**This spans two repos and wants two implementation plans, in order:**

1. **`ptdata`** — `CalibrationMode::Volts` (`ical=2`) plus the non-variadic C
   entry points the TDI wrapper calls. Self-contained, unit-testable, and
   useful on its own.
2. **`xrdoss-mdsplus`** — the sandbox: TDI functions, the modern
   `PTDATA2`/`PTHEAD2`, mounts, index configuration, tests.

Plan 2 cannot be verified without plan 1, since the equivalence tests need
`ical=2`.

## The problem

DIII-D trees fetch data through embedded TDI calls — `PTDATA2(\ECPTNAM[i],
$SHOT, 4)` and similar. Those nodes fail in the sandbox, for two independent
reasons.

**The functions are missing.** The sandbox installs the upstream MDSplus RPM,
which ships 182 `.fun` files and none of DIII-D's. The site functions live in
`DIII-D/css-d3d-mdsplus` under `tdi/`, which production's `MDS_PATH` points at.

**The data is unreachable.** `PTDATA2` calls into a native library named by
`PTDATA_LIBRARY`, and the sandbox has neither that library nor — by design — a
network.

This is not sandbox-only. Reading a PTDATA2-backed node's record from the
**deployed origin** today fails with `%TDI-E-UNKNOWN_VAR`, because the origin's
mdsip has no `tdi/d3d` either. Twelve trees are affected.

## What the trees actually do

Measured by decompiling stored node records offline across three campaigns.
Offline on purpose: reading a record through mdsip makes the *server* evaluate
it, which is precisely what fails today.

| Shot | Era | Calls | `ical` values |
|---|---|---|---|
| 198873 | 2024 | 417 | default (1), 4 |
| 160062 | ~2015 | 752 | default (1), 4, **2** |
| 140054 | ~2010 | 544 | default (1), 4, **2** |

Across ~1,700 calls spanning 14 years the set is **`{1, 2, 4}`**. `ical=3`,
`10–19` and `20` never appear. `ical` is omitted in the large majority of calls,
and `ptdata2.fun` defaults it to 1 (`IF (NOT PRESENT(_ical)) _ical = 1;`).

Two cautions this measurement earned:

- **String counts lie.** `bci` shows 960 `PTDATA2` strings in its 2024 datafile
  and decompiles to **zero** calls — it embedded them in 2015 (80 calls) and
  stopped, leaving heap residue. Only decompilation gives a true count.
- **The tree list is biased to the present.** Candidate trees came from a 2024
  sweep; `bcixp`, `llama` and `rfdev` do not exist for the older shots. A tree
  retired before 2024 would not appear at all, so `{1, 2, 4}` is well-supported
  but not provably exhaustive.

## Architecture: a thin TDI wrapper over the modern C++

Rather than shipping the legacy `ptdata2.fun` and libd3's `index_plugin.c`, the
sandbox gets its own `PTDATA2`/`PTHEAD2` that are thin wrappers over
`ptd3d`'s `PtDataReader`.

`PtDataReader::fetch()` already returns what a signal needs:

```cpp
struct ExtractedData {
    std::vector<double> data;
    std::vector<double> times;   // milliseconds -- the convention TDI wants
    std::string units;
    std::vector<int> shape;
    bool data_revised;  bool fewer_points;  int n_over, n_under;
};
```

and `ReaderConfig` takes an `IndexPlugin*` — the **modern C++** plugin
(`index_plugin.h`, `json_index_plugin.h`), not the legacy C loader. Three
reasons this is better than wrapping the legacy stack:

1. **No `index_plugin.c`.** The bit-rot risk is avoided rather than tolerated.
2. **Logic becomes testable.** `ptdata2.fun` does header parsing and npts
   arithmetic in TDI — where the fork has already had to fix a bug ("missing
   `||` operators in ptdata2.fun IF condition"). In C++ that is unit-testable.
3. **We own the error semantics.** The site function passes through only
   `{0, 2, 31, 33}` and returns `[0]` for everything else, so a not-found
   (`RMS_FNF`, `0x18292`) reaches the client as an empty array with no error.
   Owning the wrapper lets a genuine failure be a genuine failure.

### Prerequisite: `CalibrationMode::Volts`

```cpp
enum class CalibrationMode { Raw = 0, Full = 1, Linear = 4 };   // today
```

`ical=2` is used by `spectroscopy` and `d3d` on historical shots and is not
implemented. **This must land in the `ptdata` repo before the wrapper is
useful**, or those nodes are wrong for exactly the archived shots this origin
exists to serve. It is one mode, not the full legacy surface.

### The C entry point

TDI reaches native code through `BUILD_CALL`, and TDI's FFI **cannot call
variadic functions** — it returns -1 silently. The entry points must therefore
be plain non-variadic C. Shape (exact signature to be settled in the plan):

- a header call returning sizes and metadata, mirroring `PTHEAD2`
- a fetch call filling caller-allocated `data` / `times` buffers, returning an
  explicit status

The TDI side then does only `MAKE_SIGNAL(_data, *, MAKE_DIM(*,
MAKE_WITH_UNITS(_times,"ms")))`. Keeping signal assembly in TDI means the C++
never links MDSplus.

The wrapper must preserve the public signature, since trees call it positionally:

```
PUBLIC FUN PTDATA2(IN _pointname, OPTIONAL IN _shot, OPTIONAL IN _ical,
                   OPTIONAL OUT _error, OPTIONAL IN _double)
```

**Status:** the fetch half shipped in `ptdata` as
`ptdata_capi_size` / `ptdata_capi_copy` (see that repo's
`docs/superpowers/plans/2026-08-16-tdi-c-entry-points.md`). Its ABI was
measured against a running MDSplus rather than assumed: `BUILD_CALL(8, ...)`
delivers an int return, arguments arrive as pointers, text arrives
NUL-terminated with padding intact, and a `REF()`'d array is filled to its
full extent. `VAL()` must never be used — it passes the integer where a
pointer is expected and takes SIGSEGV.

### `PTHEAD2` needs a second entry point, and it is the harder one

The survey above found that `PTHEAD2` is reached 596 times through the legacy
`PTHEAD_*` shims. Those shims read `PTHEAD2`'s **side effects**, not its return
value:

```
PTHEAD_RFIX   → pthead2(...); return(__rarray)
PTHEAD_REAL32 → pthead2(...); return(__real32[2:*])
PTHEAD_IFIX   → pthead2(...); return(_ifix)
PTHEAD_ASCII  → pthead2_ascii(...)
```

So a modern `PTHEAD2` must set `PUBLIC __iarray`, `__rarray`, `__ascii`,
`__int16`, `__int32`, `__real32`, `__real64` — the legacy Fortran header
arrays — not merely return an npts. 548 of those 596 calls depend on the
globals.

That is a larger legacy surface than the fetch path, but most of it already
exists: `ptdata`'s `python/ptdata/header.py` has `_build_legacy_arrays` and
`_build_legacy_var_header_arrays`, written and parity-tested during the
"PtDataHeader 1.x parity" work. They produce the `iarray`/`rarray` pair and the
`[n, n, values...]` var-header layout that `pthead2.fun` builds and
`PTHEAD_REAL32` skips two words into. The work is to move that projection into
C++ (so the ABI and Python cannot drift) and expose it through
`ptdata_capi_header_size` / `ptdata_capi_header_copy`.

One thing to settle rather than assume: `_build_legacy_arrays` produces 50
slots, while `pthead2.fun` requests 64. Every index observed in use stops at 39,
and 41-50 hold the VAX `0xFAFAFAFA` sentinel — but `PTHEAD_IFIX` hands the whole
array to tree code whose subscripts have not been measured.

### Which TDI functions we own

Only three reach native code, so only three are ours to write:

| Ours | Comes free (pure TDI on top) |
|---|---|
| `PTDATA2` | `PTDATA`, `PTNPTS`, `ECEPROF`, `IP_PROBES`, `IP_PROBES_Z` |
| `PTHEAD2` | `PTHEAD_IFIX`, `PTHEAD_RFIX`, `PTHEAD_REAL32` |
| `PTHEAD2_ASCII` | `PTHEAD_ASCII` |

`PTNPTS` is `return(_ifix[31])`; the four `PTHEAD_*` shims are one line each.
The remaining site functions — `DAMPHASE`, `USING_SIGNAL`, `LOADDATA`,
`MULTIPHASE`, `TECEPROF`, `ECHPWRC`, `SLEEP`, 866 calls between them — touch
ptdata not at all and are used as-is.

## Which site functions are actually needed

Measured 2026-08-16 by `tests/survey_tdi_calls.py`, decompiling every stored
record across the same 36 trees (12 trees × 3 shots) and classifying every call
against the MDSplus builtin opcodes, the MDSplus kernel `.fun` set, and
`css-d3d-mdsplus/tdi`.

Our modern `PTDATA2` needs none of the legacy helpers — the C++ reader handles
segmentation, PCS timing and calibration internally — so it was worth asking
whether the site library is needed at all. **It is.** Fourteen site functions
are called directly from stored records, 1,473 times:

| Function | Calls | Trees |
|---|---|---|
| `DAMPHASE` | 537 | d3d |
| `PTHEAD_REAL32` | 286 | nb, neutrals, spectroscopy |
| `PTHEAD_RFIX` | 262 | neutrals, spectroscopy |
| `USING_SIGNAL` | 161 | mhd, transport |
| `LOADDATA` | 113 | d3d, ece |
| `MULTIPHASE` | 51 | d3d, mhd |
| `PTHEAD_ASCII` | 24 | nb |
| `PTHEAD_IFIX` | 24 | nb |
| `ECEPROF`, `TECEPROF` | 3 each | ece |
| `IP_PROBES`, `IP_PROBES_Z` | 3 each | mhd |
| `SLEEP` | 2 | d3d |
| `ECHPWRC` | 1 | rf |

Zero unclassified names remain, so the list is complete for this sample.

Two things this changes:

- **The legacy `PTDATA()` shim is the *majority* caller**, not a compatibility
  afterthought: 1,259 calls against `PTDATA2`'s 1,210. It reaches our
  implementation only because it delegates, and it pulls the legacy
  `PTHEAD_*` family (`_REAL32`, `_RFIX`, `_ASCII`, `_IFIX`, 596 calls) with it.
  Those are separate functions from the `PTHEAD2_*` family and must be present.
- **Installing the whole `tdi/` tree is right**, not merely convenient. The
  fourteen are only the directly-called set; each may call further site
  functions in turn, and the transitive closure was not measured.

## TDI function installation

**Upstream MDSplus already packages the DIII-D site library.** Measured
2026-08-16: `mdsplus-d3d-7.158-2.el9.noarch.rpm`, from the same
`mdsplus.org/dist/rhel9/stable` repo the image already installs its other two
RPMs from, is 61 KB, declares no dependencies beyond `rpmlib`, and installs all
118 `.fun` files to `/usr/local/mdsplus/tdi/d3d` — including every one of the
fourteen above and the whole `ptdata/` and `ptdata2/` subtrees.

So the install is one more URL on the existing `dnf -y install` line. No clone,
no vendored copy, no credentials at build time, and nothing for the mdsip
service account to reach. (The `mdsplus-xrdcl` fork vendors the same 118 files
at `tdi/d3d/`, and conda-forge's stock `mdsplus` ships them too — the library
travels with MDSplus itself, not only with the DIII-D site repo.)

**Drift from `css-d3d-mdsplus`, and why it is acceptable.** The RPM's copy is
not byte-identical to the site checkout: 15 of the 118 differ, and three of
those are in the needed fourteen — `damphase` (537 calls), `using_signal`
(161), `loaddata` (113). The other eleven, including the entire `PTHEAD_*`
family, are identical.

`DAMPHASE` and `LOADDATA` are dispatch-side: both `SPAWN` an `ssh` to a named
GA host (the two copies disagree about *which* host, which is most of the
diff). Neither can function in a no-network sandbox under either version, and a
read-only data server never dispatches the action nodes that call them. The
drift there is moot.

`USING_SIGNAL` is the one that matters, and the difference is substantive
rather than cosmetic: the site copy opens the tree explicitly through
`TreeShr->TreeOpen` and returns `[0]` on failure, while the RPM's uses
`using(..., _shot, _tree)` and lets TDI resolve it. **Verify this one against a
real tree during implementation** — it is the only drifted function on the
data-retrieval path.

The five `ptdata2/` files that differ are ones we replace, so their drift is
irrelevant by construction.

`MDS_PATH` is searched in order, first match wins, so our directory goes
**first**:

```
/usr/local/fdp/tdi                       <- our modern PTDATA2/PTHEAD2
/usr/local/mdsplus/tdi/d3d               <- mdsplus-d3d, everything else
/usr/local/mdsplus/tdi/d3d/ptdata
/usr/local/mdsplus/tdi/d3d/ptdata2
/usr/local/mdsplus/tdi/d3d/ptdata_historic
/usr/local/mdsplus/tdi/d3d/global
/usr/local/mdsplus/tdi/d3d/nimrod
/usr/local/mdsplus/tdi
/usr/local/mdsplus/tdi/remote
```

Those five subdirectories are the RPM's own layout, confirmed from its file
list, not a guess at one.

`MDS_PATH` is a flat list, not recursive — naming only a parent resolves
`PTDATA2` while leaving `PTHEAD2` unresolved, a partial install that presents as
a data problem rather than a packaging one. The legacy `PTDATA()`/`PTHEAD*()`
shims delegate to `PTDATA2`, so they pick up our implementation for free.

Shadowing rather than deleting keeps the override explicit and reversible:
removing one `MDS_PATH` entry restores production behaviour exactly.

## Data access

Built with `-DPTDATA_WITH_FDPIO=OFF`, `libptd3d.so` links only `${CMAKE_DL_LIBS}`
— it is *physically incapable* of remote I/O, so the sandbox's no-network
property holds by construction rather than only by policy. A multi-stage build
copies just the shared objects into the runtime image; no compiler, no
`libfdpio2`, no XRootD. The floor is **whichever release adds
`CalibrationMode::Volts`** (see Prerequisite); it must in any case be
`>=2.0.15`, the release that initialises `ier=0` in `ptfile()` and so fixes a
`PTSEARCH0` infinite loop in `SYS_D3`-less environments — and this deployment is
`SYS_D3`-less.

Resolution is **index only**; a directory scan is too slow.

```
PTDATA_PLUGIN_LIB          /usr/local/ptdata/lib/libjson_index_plugin.so
PTDATA_JSON_INDEX_DIR      /ptdata-index
PTDATA_JSON_INDEX_PATTERN  json_indexes_*
```

Two additional read-only bind mounts, mirroring the existing tree mount:

| Host | Container | Contents |
|---|---|---|
| `/mnt/beegfs/data/archives/ptdata` | `/fdp-archives/archives/ptdata` | shotfiles |
| `/mnt/beegfs/data/archives/index/json` | `/ptdata-index` | index snapshots |

The shotfile mount is nested to match what the index records (see below).
Mounting only `.../ptdata` keeps the rest of `/mnt/beegfs/data` out of the
sandbox. The index needs no such treatment: `PTDATA_JSON_INDEX_DIR` is a value we
set ourselves.

`site.env` gains the host paths and the environment block, in the same shape as
`ARCHIVE_ROOT` and `MDSIP_TREE_ENV`.

## The Pelican-path workaround — REVISIT THIS

Index entries record **absolute Pelican URLs**:

```json
".PCE": "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE"
```

`LocalIoProvider::resolve()` returns its argument unchanged, so that string
reaches `::open()` verbatim.

The workaround needs no code. The string does not begin with `/`, so it is a
**relative** path, and POSIX collapses consecutive slashes — with the working
directory at `/` it resolves as
`/pelican:/osg-htc.org:443/fdp-d3d/archives/...`. The image carries that chain,
its last component symlinked to the root the index paths are relative to:

```
/pelican:/osg-htc.org:443/fdp-d3d  ->  /fdp-archives
/fdp-archives/archives/ptdata          ro mount
```

Verified: opening the literal URL string succeeds through `open(2)` when the
chain exists relative to the working directory.

- **The working directory must be `/` explicitly.** socat's child inherits
  socat's cwd; `fdp-mdsip-connection` must `cd /` rather than rely on the
  image's default `WORKDIR`.
- **The Pelican host and federation prefix are encoded in a directory name.** If
  either changes, every lookup fails as "file not found" with nothing pointing
  at the cause.

> **Return to this.** The real fix is for the indexer to record archive-relative
> paths, with each consumer prepending its own root. Deferred because it needs
> the index regenerated and its consumers updated.

## Error semantics

Owning the wrapper means choosing these deliberately rather than inheriting
them:

| Condition | Behaviour |
|---|---|
| Pointname legitimately absent for the shot | Return empty — matches what `PTDATA2` users already expect, and what production does |
| Shot absent from the index | Return empty, indistinguishable from the above (see Accepted consequences) |
| Unsupported `ical` | **Raise.** Never silently substitute a different calibration |
| Unreadable shotfile, malformed header, index unreadable | **Raise** |

The distinction is between "there is no data" and "I could not get the data".
The legacy function collapses both into `[0]`; this one does not. Silently
substituting a calibration would be the worst available outcome — wrong numbers
that look right — and is explicitly ruled out.

**Measured status codes** (2026-08-16, pinned by `ptdata`'s
`cpp/tests/test_capi_absent_cases.cpp`; see
`ptdata/docs/superpowers/plans/2026-08-16-tdi-c-entry-points.md`):

| Condition | Status | Name |
|---|---|---|
| Known shot, unknown pointname | 1 | `PointnameNotFound` |
| Unknown shot | 3 | `ShotNotFound` |
| Unsupported `ical` | 110 | `InvalidConfiguration` |
| Buffer larger than the fetch | 200 | `PTDATA_CAPI_BUFFER_TOO_SMALL` |
| NULL argument / internal error | 201 | `PTDATA_CAPI_INTERNAL` |

So `PTDATA2.fun` returns `[0]` for **{1, 3}** and raises for everything else.

Two further measurements the `.fun` has to respect:

- **`ntimes` can be 0.** A point whose DFI has no dedicated handler is served
  by `GenericDfi`, which produces no time base. The `.fun` must cope with a
  zero-length time axis rather than assume `MAKE_DIM` always has something.
- **Never `VAL()` a value the callee will dereference.** TDI's `VAL()` passes
  the integer itself where the C entry points expect a pointer; the
  dereference takes SIGSEGV and kills the mdsip connection rather than
  returning an error. Pass real arguments with `REF()` or bare — both pass a
  pointer.

  `VAL(0)` is the deliberate exception: it is how TDI passes a **NULL**
  pointer, which is what the header entry point's optional sections use
  (`PTHEAD_RFIX` wants only `rarray`). Safe because the callee guards.
  Measured, along with a 16-argument call arriving intact — the arity
  `ptdata_capi_header_copy` needs.

## Accepted consequences

**Coverage equals the index's coverage.** A shot whose shotfiles are on disk but
absent from the index is unavailable. The newest snapshot observed is
`json_indexes_2026-06-23`, so the gap is real.

**Our `PTDATA2` is a drop-in for tree-embedded calls, not for every call.** A
user calling `PTDATA2` interactively with `ical=3`, `10–19` or `20` gets an
error where production would answer. No tree measured uses those, and the
sandbox supports none of them today, so this is a strict improvement — but it is
a divergence and belongs on the record.

## Security

Unchanged posture. The data added is already inside the boundary — any token
holder may read everything under `/fdp-d3d/archives` read-only, so a client with
code execution in the sandbox gains nothing it could not already fetch through
the origin. Both new mounts are read-only, no network is added, and the ptdata
library is built unable to open one. No new syscalls; the seccomp capture should
still be re-run, which is owed for the socat entrypoint regardless.

## Testing

- A fixture tree node with an embedded `PTDATA2()` call, so the sandboxed e2e
  covers the real path end to end.
- Per-`ical` equivalence against the legacy implementation for `{1, 2, 4}` —
  same pointname, same shot, same numbers. This is the check that makes
  replacing site code defensible.
- A check that fails loudly when `MDS_PATH` resolves `PTDATA2` but not its
  helpers.
- A check that the Pelican-path chain resolves, and fails loudly if the working
  directory is not `/`. Note when writing it: the symlink is absolute, so it
  only resolves inside the container; a host test needs a relative symlink or it
  fails with `ENOENT` for reasons unrelated to the design.
- `readelf` assertions that the built `libptd3d.so` has no `libfdpio2` or
  `libXrd*` in `NEEDED`, and that the existing no-network checks still pass with
  ptdata present.

## Follow-ups

1. `CalibrationMode::Volts` (`ical=2`) in the `ptdata` repo — a prerequisite,
   not a follow-up, but tracked there.
2. Make the indexer record archive-relative paths and retire the trick above.
3. Re-run `tests/security/capture_syscalls.sh` against the socat entrypoint.
4. `rfdev_160062.tree` was reported "Corrupted/truncated" during the survey.
   Unverified whether that is a bad file on the origin or a download artefact;
   there is precedent for corrupt archive files. Worth a separate look.
