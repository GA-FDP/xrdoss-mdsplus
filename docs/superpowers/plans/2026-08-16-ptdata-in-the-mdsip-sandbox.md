# PTData in the mdsip Sandbox — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make PTDATA2-backed tree nodes resolve and return correct data inside
the mdsip sandbox, with no network and no writable filesystem.

**Architecture:** The DIII-D site TDI library comes from an upstream RPM. Three
functions — `PTDATA2`, `PTHEAD2`, `PTHEAD2_ASCII` — are replaced by thin TDI
wrappers over `ptdata`'s C ABI (`ptdata_capi_*`), shadowing the site versions
via `MDS_PATH` order. `libptd3d` is built without fdpio so it is physically
incapable of remote I/O; shotfiles and the JSON index arrive as read-only bind
mounts.

**Tech Stack:** MDSplus TDI, `ptdata` ≥2.2.0 (`capi.h`), CMake, podman,
almalinux 9, socat.

**Spec:** `docs/superpowers/specs/2026-08-13-ptdata-in-the-mdsip-sandbox-design.md`

**Prerequisite:** `ptdata` 2.2.0 must be released. Plan 1
(`ptdata/docs/superpowers/plans/2026-08-16-tdi-c-entry-points.md`) is complete
through Task 10; only its Task 9 step 4 (release + `fdp-core` bless) remains.
Nothing below can be verified until that tag exists, because the equivalence
tests need `CalibrationMode::Volts` and every wrapper calls `capi.h`.

---

## Measured facts this plan is built on

Do not re-derive these. Each cost a measurement.

**The site library ships as an RPM.** `mdsplus-d3d-7.158-2.el9.noarch.rpm`,
from the same `mdsplus.org/dist/rhel9/stable` repo the image already uses, is
61 KB, requires nothing beyond `rpmlib`, and installs 118 `.fun` files to
`/usr/local/mdsplus/tdi/d3d` plus five subdirectories: `ptdata`, `ptdata2`,
`ptdata_historic`, `global`, `nimrod`.

**The legacy contract our three wrappers must reproduce**, read from the
shipped `.fun` files:

| Function | Signature | Must produce |
|---|---|---|
| `PTHEAD2` | `(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error)` | returns `_iarray` (50 longs); sets `PUBLIC __iarray __rarray __ascii __int16 __int32 __real32 __real64`; maps error 33 → 0 |
| `PTHEAD2_ASCII` | same | returns a **string**, decoded from `__ascii[2:*]` with NULs dropped |
| `PTDATA2` | `(IN _pointname, OPTIONAL IN _shot, OPTIONAL IN _ical, OPTIONAL OUT _error, OPTIONAL IN _double)` | returns `MAKE_SIGNAL(_data, *, MAKE_DIM(*, MAKE_WITH_UNITS(_time,"ms")))`; sets `PUBLIC __ptdata_signal`, `__branch`, `__crate`, `__slot` |

**The shims that come free**, verbatim from `tdi/d3d/ptdata/`:

```
PTNPTS        _ifix = pthead2(...); return(_ifix[31])
PTHEAD_IFIX   _ifix = pthead2(...); return(_ifix)
PTHEAD_RFIX   _ifix = pthead2(...); return(__rarray)
PTHEAD_REAL32 _ifix = pthead2(...); IF (_error) ABORT();
              IF (LE(SIZE(__real32),2)) ABORT(); return(__real32[2:*])
PTHEAD_ASCII  return(pthead2_ascii(...))
```

Two consequences. `PTNPTS` reads `_ifix[31]`, 0-based — legacy `IARRAY(32)`,
which `legacy_header::build` fills from `fh.data_word_count`; the ABI's own
test pins it. And `PTHEAD_REAL32` **ABORTs when `SIZE(__real32) <= 2`**, which
is exactly the length our empty-section layout produces — matching legacy,
whose `[n, ZERO(n+1)]` for `n=0` is also length 2. Do not "fix" that.

**`ntimes` can be 0**, and the units differ between the two sources. The engine
returns `times` already in **milliseconds**. The legacy header's
`rarray[7]`/`rarray[8]` (0-based; `RARRAY(8)` start, `RARRAY(9)` delta) are in
**seconds**, which is why the legacy fallback multiplies by 1000. A wrapper
that synthesizes a time base from the header must scale; one that uses the
engine's must not. Getting this backwards is a 1000× error that looks like a
plausible time axis.

**TDI FFI**, measured in plan 1 (`ptdata/cpp/tests/spikes/`):

- `BUILD_CALL(8, lib, entry, ...)` returns an int. `BUILD_CALL(0, ...)`
  **discards** the return value — which is why every legacy `.fun` uses a
  `REF(_ier)` out-parameter. Our entry points return status, so use `8`.
- Arguments arrive as pointers; `REF(x)` and a bare argument are equivalent.
- Text arrives NUL-terminated, trailing spaces intact. The engine normalizes
  pointnames, so the legacy `//"          "` padding is harmless but pointless.
- A `REF()`'d array is filled to its full extent — verified at 1000 elements.
- **`VAL()` faults** when the callee dereferences, with one exception:
  `VAL(0)` is how TDI passes NULL, verified against a 16-argument call.

**Status codes** the wrappers must classify (`ptdata/docs/capi.md`):

| Status | Name | Wrapper behaviour |
|---|---|---|
| 0 | ok | proceed |
| 1 | `PointnameNotFound` | return `[0]` |
| 3 | `ShotNotFound` | return `[0]` |
| 110 | `InvalidConfiguration` | **raise** (unsupported `ical`) |
| 200 | `BUFFER_TOO_SMALL` | **raise** (our bug) |
| 201 | `INTERNAL` | **raise** |

---

## File structure

| File | Responsibility |
|---|---|
| `tdi/fdp/PTD3D_LIBRARY.fun` | resolve the `libptd3d.so` path, one place |
| `tdi/fdp/PTDATA2.fun` | fetch → signal |
| `tdi/fdp/PTHEAD2.fun` | header → `_iarray` + the seven globals |
| `tdi/fdp/PTHEAD2_ASCII.fun` | ASCII header → string |
| `Containerfile.mdsip` | ptdata build stage, `mdsplus-d3d`, `MDS_PATH`, env, Pelican-path chain |
| `scripts/mdsip-sandbox.sh` | the two read-only mounts + env |
| `deploy/fdp-mdsip.container` | same, for the quadlet |
| `tests/integration/test_ptdata_tdi.sh` | e2e through a real mdsip connection, including a stored record that embeds the call |
| `tests/integration/check_mds_path.py` | every function we depend on resolves |
| `tests/integration/check_pelican_path.py` | the URL-as-relative-path chain resolves, and cwd is `/` |
| `tests/ptdata_equivalence.py` | per-`ical` numbers vs production |

`tdi/fdp/` is a new directory, deliberately separate from anything the RPM
owns, so that removing one `MDS_PATH` entry restores production behaviour
exactly.

---

### Task 1: Find out what else reads `PTDATA2`'s globals

**Why first.** `PTDATA2.fun` sets `PUBLIC __branch`, `__crate`, `__slot` from
`__int16[6..8]`, and `PUBLIC __ptdata_signal`. Whether any stored record reads
them decides whether our wrapper must set them or may drop them. Guessing costs
either dead code or a silent regression, and `tests/survey_tdi_calls.py`
already decompiles every record — this is a one-line extension of a tool that
exists.

**Files:**
- Modify: `tests/survey_tdi_calls.py`

- [ ] **Step 1: Add a global-reference scan**

`survey_tdi_calls.py` classifies *calls*. Add a second pass over the same
decompiled text that counts references to the public globals, since a read of
`__rarray` is not a call and the existing regex will not see it.

```python
# Public globals the legacy ptdata functions set as side effects. A record
# that reads one of these depends on a wrapper still setting it -- the
# functions' return values are not the whole contract.
PTDATA_GLOBALS = [
    "__iarray", "__rarray", "__ascii", "__int16", "__int32",
    "__real32", "__real64", "__branch", "__crate", "__slot",
    "__ptdata_signal",
]


def scan_globals(text):
    """Count public-global references in one decompiled record.

    Word-boundary matched: `__int16` must not also match `__int168`. Leading
    underscores are word characters, so \b before `__` only fires after a
    non-word character -- which is what we want, since `x__int16` is not a
    reference to the global.
    """
    counts = {}
    for name in PTDATA_GLOBALS:
        n = len(re.findall(r"\b" + re.escape(name) + r"\b", text))
        if n:
            counts[name] = n
    return counts
```

Wire it into the per-record loop alongside the call classification and
aggregate into a `Counter` keyed `(global, tree)`, printed as its own table.

- [ ] **Step 2: Run it over the same 36 trees**

Run: `pixi run python tests/survey_tdi_calls.py --globals`
Expected: a table. Record the totals in this plan under Task 4 before writing
`PTDATA2.fun`.

Interpretation, decided now so the result is not rationalized later:

- **Any nonzero count for `__branch`/`__crate`/`__slot`** → `PTDATA2.fun` must
  set them, from `__int16[6..8]` as the legacy does.
- **Zero** → set them to 0 anyway (three assignments, no cost) but do not
  reproduce the DFI-list guard, and say so in a comment citing this survey.
- The seven header globals are needed regardless: `PTHEAD_RFIX` and
  `PTHEAD_REAL32` read them, 548 calls' worth, which the earlier survey already
  measured.

- [ ] **Step 3: Commit**

```bash
git add tests/survey_tdi_calls.py
git commit -m "test(survey): count reads of the ptdata public globals"
```

---

### Task 2: `PTD3D_LIBRARY.fun`, and prove BUILD_CALL reaches the library

**Why a separate task.** Every wrapper depends on this resolving. If it does
not, all three fail identically and the cause is invisible — `BUILD_CALL` on a
missing library is not a loud error.

**Files:**
- Create: `tdi/fdp/PTD3D_LIBRARY.fun`
- Create: `tests/integration/check_ptd3d_call.py`

- [ ] **Step 1: Measure what TranslateLogical returns when unset**

The legacy `PTDATA_LIBRARY.fun` returns `TranslateLogical("PTDATA_LIBRARY")`
with no guard. Before copying that shape, find out what an unset logical
actually yields, because the guard depends on it:

```bash
cd ../ptdata && pixi run -e mds-validate python -c "
import MDSplus
r = MDSplus.Data.execute('TranslateLogical(\"NO_SUCH_LOGICAL_XYZ\")')
print(repr(r), type(r))
print('len:', MDSplus.Data.execute('LEN(TranslateLogical(\"NO_SUCH_LOGICAL_XYZ\"))'))
"
```

Expected: one of `\$MISSING`, an empty string, or an exception. Write the guard
in Step 2 against whichever it is — do not guess.

- [ ] **Step 2: Write the function**

```
/* PTD3D_LIBRARY()
 *
 * Where libptd3d.so lives. One place, because three functions BUILD_CALL into
 * it and a wrong path fails all three the same silent way.
 *
 * Mirrors the legacy PTDATA_LIBRARY(), which reads $PTDATA_LIBRARY, except
 * that this one has a default: the sandbox image installs to a path we choose,
 * so requiring the environment variable would add a way to misconfigure it
 * with no corresponding benefit.
 */
PUBLIC FUN PTD3D_LIBRARY() {
    _lib = TranslateLogical("PTD3D_LIBRARY");
    IF (NOT PRESENT(_lib) || LEN(_lib) == 0) {
        _lib = "/usr/local/ptdata/lib/libptd3d.so";
    }
    RETURN(_lib);
}
```

The guard covers both shapes Step 1 can return. Keep whichever half the
measurement justifies and delete the other rather than leaving a condition
whose second clause is never reached:

- returns `$MISSING` → keep `NOT PRESENT(_lib)`
- returns `""` → keep `LEN(_lib) == 0`
- raises → replace the whole guard with the literal path and drop
  `TranslateLogical`, since an override that throws is not an override.

- [ ] **Step 3: Write the reachability check**

```python
#!/usr/bin/env python
"""Prove TDI can reach libptd3d, before anything depends on it.

Run inside the sandbox image, or anywhere MDS_PATH names tdi/fdp:

    python tests/integration/check_ptd3d_call.py

Calls ptdata_capi_last_error(), which needs no data, no index and no shot --
it is the one entry point that cannot fail for environmental reasons, so a
failure here is the library not loading rather than the data not being there.
"""
import sys

import MDSplus

expr = (
    '_e = BUILD_CALL(2, PTD3D_LIBRARY(), "ptdata_capi_last_error"); _e'
)
try:
    result = MDSplus.Data.execute(expr).data()
except Exception as exc:  # noqa: BLE001 -- report every failure shape
    sys.exit(f"FAIL: BUILD_CALL into libptd3d raised {type(exc).__name__}: {exc}")

print(f"ok: last_error = {result!r}")
```

`2` is `DTYPE_B`… **verify the right rtype for a `const char *` return before
relying on this.** If TDI has no dtype that materializes a returned pointer as
a string, replace the check with `ptdata_capi_reset` under `BUILD_CALL(0, ...)`
— it returns void, takes no arguments, and still proves the symbol resolved.
The point of the check is loading, not the string.

- [ ] **Step 4: Run it**

Run: `pixi run -e mds-validate python tests/integration/check_ptd3d_call.py`
with `MDS_PATH` including `tdi/fdp` and `PTD3D_LIBRARY` pointing at a locally
built `libptd3d.so`.
Expected: `ok:` line, not a `%TDI-E-` error.

- [ ] **Step 5: Commit**

```bash
git add tdi/fdp/PTD3D_LIBRARY.fun tests/integration/check_ptd3d_call.py
git commit -m "feat(tdi): resolve libptd3d, and prove BUILD_CALL reaches it"
```

---

### Task 3: `PTHEAD2.fun`

**Why before `PTDATA2`.** `PTDATA2` calls it, and so do 596 shim calls. It is
also the one whose contract is entirely side effects, so getting it right first
means `PTDATA2` can just use it.

**Files:**
- Create: `tdi/fdp/PTHEAD2.fun`

- [ ] **Step 1: Write it**

```
/* PTHEAD2(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error)
 *
 * The legacy header call, over ptdata's C ABI instead of libd3's ptdata64_.
 *
 * The return value is the smaller half of the contract. PTHEAD_RFIX returns
 * PUBLIC __rarray, PTHEAD_REAL32 returns PUBLIC __real32[2:*], and 548 of the
 * 596 measured PTHEAD2 calls arrive through those -- so the seven globals
 * below ARE the interface. Dropping one would break callers that never look
 * at what this function returns.
 *
 * Each variable-header section is [n, n, values...]: two control words then
 * the data, which is why PTHEAD_REAL32 skips [2:*] and why an empty section
 * is length 2 rather than 0. ptdata's legacy_header::build produces that
 * layout; this function only transports it.
 */
PUBLIC FUN PTHEAD2(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error) {

    IF (NOT PRESENT(_shot)) _shot = $SHOT;

    _ni = 0; _nr = 0; _na = 0; _n16 = 0; _n32 = 0; _nf32 = 0; _nf64 = 0;
    _error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_header_size",
                        _pointname, REF(LONG(_shot)),
                        REF(_ni), REF(_nr), REF(_na),
                        REF(_n16), REF(_n32), REF(_nf32), REF(_nf64));

    /* 1 = PointnameNotFound, 3 = ShotNotFound: absent data, not a failure to
     * get it. Everything else is a real error and must not be smoothed into
     * an empty header, which callers would read as zeros. */
    IF (_error == 1 || _error == 3) {
        PUBLIC __iarray = ZERO(50, 0);
        PUBLIC __rarray = ZERO(20, 0.0);
        PUBLIC __ascii  = [0, 0];
        PUBLIC __int16  = [0, 0];
        PUBLIC __int32  = [0, 0];
        PUBLIC __real32 = [0.0, 0.0];
        PUBLIC __real64 = [0D0, 0D0];
        RETURN(PUBLIC __iarray);
    }
    IF (_error != 0) { ABORT(); }

    _iarray = ZERO(_ni, 0);
    _rarray = ZERO(_nr, 0.0);
    _ascii  = ZERO(_na, 0);
    _int16  = ZERO(_n16, 0);
    _int32  = ZERO(_n32, 0);
    _real32 = ZERO(_nf32, 0.0);
    _real64 = ZERO(_nf64, 0D0);

    _error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_header_copy",
                        _pointname, REF(LONG(_shot)),
                        REF(_ni),   REF(_iarray),
                        REF(_nr),   REF(_rarray),
                        REF(_na),   REF(_ascii),
                        REF(_n16),  REF(_int16),
                        REF(_n32),  REF(_int32),
                        REF(_nf32), REF(_real32),
                        REF(_nf64), REF(_real64));
    IF (_error != 0) { ABORT(); }

    PUBLIC __iarray = _iarray;
    PUBLIC __rarray = _rarray;
    PUBLIC __ascii  = _ascii;
    PUBLIC __int16  = _int16;
    PUBLIC __int32  = _int32;
    PUBLIC __real32 = _real32;
    PUBLIC __real64 = _real64;

    RETURN(_iarray);
}
```

Two departures from the legacy, both deliberate:

- **No `IF (eq(_error,33)) { _error = 0; }`.** 33 was a libd3 status; the C ABI
  does not produce it. Carrying the mapping forward would silently swallow a
  future `ErrorCode` that happened to be 33.
- **`ABORT()` on a real error**, where the legacy returned a zeroed array. This
  is the spec's error-semantics decision: "there is no data" and "I could not
  get the data" must not look the same.

**The `_int16` type question.** `ZERO(_n16, 0)` allocates 32-bit longs, but
`ptdata_capi_header_copy` writes `int16_t`. TDI has `WORD()` for 16-bit.
Determine the correct allocator empirically before trusting this — a type
mismatch here overruns half the buffer and corrupts whatever follows:

```bash
cd ../ptdata && pixi run -e mds-validate python -c "
import MDSplus
for expr in ['ZERO(4,0W)', 'WORD(ZERO(4,0))', 'ZERO(4,0)']:
    d = MDSplus.Data.execute(expr)
    print(expr, '->', d.data().dtype)
"
```

Use whichever yields `int16`. If none does, pass `VAL(0)` for the `int16`
section and leave `__int16` as an all-zero long array — but only after
Task 1's survey confirms nothing reads it beyond `PTDATA2`'s
`__int16[6..8]`, and note the divergence here.

- [ ] **Step 2: Test it against a real shot**

```bash
pixi run -e mds-validate python -c "
import MDSplus
MDSplus.Data.execute('_e=0; _i = PTHEAD2(\"IP\", 198873, _e); [_e, _i[31], SIZE(PUBLIC __rarray)]')
"
```
Expected: `[0, <npts>, 20]`, and `PTNPTS(\"IP\", 198873)` equal to `_i[31]`.

- [ ] **Step 3: Commit**

```bash
git add tdi/fdp/PTHEAD2.fun
git commit -m "feat(tdi): PTHEAD2 over the ptdata C ABI"
```

---

### Task 4: `PTDATA2.fun`

**Files:**
- Create: `tdi/fdp/PTDATA2.fun`

- [ ] **Step 1: Write it**

```
/* PTDATA2(IN _pointname, OPTIONAL IN _shot, OPTIONAL IN _ical,
 *         OPTIONAL OUT _error, OPTIONAL IN _double)
 *
 * A thin wrapper over ptdata's C ABI. The signature is positional and trees
 * call it positionally, so it is preserved exactly -- including _double,
 * which is accepted and ignored: the ABI returns doubles unconditionally, so
 * there is nothing to toggle.
 *
 * Everything the legacy version did in TDI -- segmented-DFI dispatch, the PCS
 * timebase special cases, the per-DFI ical arithmetic -- the C++ reader does
 * internally. That is the point of the rewrite: it was untestable here and is
 * unit-tested there.
 */
PUBLIC FUN PTDATA2(IN _pointname, OPTIONAL IN _shot, OPTIONAL IN _ical,
                   OPTIONAL OUT _error, OPTIONAL IN _double) {

    IF (NOT PRESENT(_shot)) _shot = $SHOT;
    IF (NOT PRESENT(_ical)) _ical = 1;

    _npts = 0; _ntimes = 0;
    _error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_size",
                        _pointname, REF(LONG(_shot)), REF(LONG(_ical)),
                        REF(_npts), REF(_ntimes));

    /* Absent data returns [0], matching what PTDATA2 callers already expect.
     * Everything else -- an unreadable shotfile, an unsupported ical -- is a
     * real failure and is raised. Silently substituting a calibration would
     * be the worst available outcome: wrong numbers that look right. */
    IF (_error == 1 || _error == 3) { RETURN([0]); }
    IF (_error != 0) { ABORT(); }
    IF (_npts <= 0) { RETURN([0]); }

    _data  = ZERO(_npts, 0D0);
    _times = ZERO(MAX(_ntimes, 1), 0D0);
    _error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_copy",
                        _pointname, REF(LONG(_shot)), REF(LONG(_ical)),
                        REF(_npts), REF(_ntimes), REF(_data), REF(_times));
    IF (_error != 0) { ABORT(); }

    /* The engine's times are already in MILLISECONDS. The header's
     * rarray[7]/rarray[8] (start, delta) are in SECONDS, which is why the
     * fallback below scales and this branch does not. Getting that backwards
     * is a 1000x error that still looks like a plausible time axis. */
    IF (_ntimes > 0) {
        _time = _times;
    } ELSE {
        /* A DFI with no dedicated handler is served by GenericDfi, which
         * produces no time base. The legacy function had the same fallback:
         * synthesize from the header's start and delta. */
        _ignored = PTHEAD2(_pointname, _shot);
        _time = (RAMP(_npts) * PUBLIC __rarray[8] + PUBLIC __rarray[7]) * 1000D0;
    }

    /* Set by the legacy version for a handful of CAMAC DFIs; see Task 1 for
     * what actually reads them. __int16 is [n, n, values...], so int16(6) of
     * the legacy layout is index 6 here too -- the legacy read __int16[6..8]
     * against the same two-control-word layout. */
    PUBLIC __branch = 0;
    PUBLIC __crate  = 0;
    PUBLIC __slot   = 0;
    IF (SIZE(PUBLIC __int16) >= 9) {
        PUBLIC __branch = PUBLIC __int16[6];
        PUBLIC __crate  = PUBLIC __int16[7];
        PUBLIC __slot   = PUBLIC __int16[8];
    }

    PUBLIC __ptdata_signal =
        MAKE_SIGNAL(_data, *, MAKE_DIM(*, MAKE_WITH_UNITS(_time, "ms")));
    RETURN(PUBLIC __ptdata_signal);
}
```

**Note the `__int16` dependency.** The `__branch`/`__crate`/`__slot` block
reads globals that only `PTHEAD2` sets, and the `_ntimes == 0` branch calls
`PTHEAD2` explicitly. When `_ntimes > 0` and Task 1 found nothing reads
`__branch`, that block can be dropped along with its `PTHEAD2` dependency —
decide from the survey, and leave a comment saying which way it went.

- [ ] **Step 2: Test against a real shot**

```bash
pixi run -e mds-validate python -c "
import MDSplus
sig = MDSplus.Data.execute('PTDATA2(\"IP\", 198873)')
print(sig.data().shape, sig.dim_of().data()[:3], sig.dim_of().units)
"
```
Expected: a nonempty array, a millisecond time axis, units `ms`.

- [ ] **Step 3: Test the absent and the invalid cases separately**

```bash
pixi run -e mds-validate python -c "
import MDSplus
print('absent :', MDSplus.Data.execute('PTDATA2(\"NOSUCHPT\", 198873)').data())
try:
    MDSplus.Data.execute('PTDATA2(\"IP\", 198873, 3)')
    print('BAD: ical=3 did not raise')
except Exception as e:
    print('ical=3:', type(e).__name__)
"
```
Expected: `absent : [0]` and a raised exception for `ical=3`. Both matter: the
first is bug-compatibility callers rely on, the second is the divergence from
production this design chose deliberately.

- [ ] **Step 4: Commit**

```bash
git add tdi/fdp/PTDATA2.fun
git commit -m "feat(tdi): PTDATA2 over the ptdata C ABI"
```

---

### Task 5: `PTHEAD2_ASCII.fun`

**Files:**
- Create: `tdi/fdp/PTHEAD2_ASCII.fun`

- [ ] **Step 1: Write it**

The legacy version calls `PTHEAD2_SIZE`, allocates, calls `ptdata_`, then
decodes int32 words back to text 4 bytes at a time, dropping NULs. Our header
call already produced `__ascii`, so only the decode remains.

```
/* PTHEAD2_ASCII(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error)
 *
 * The ASCII variable-header section, as text.
 *
 * __ascii is [n, n, words...] where each word packs 4 characters
 * little-endian, so the decode starts 8 bytes in -- exactly what the legacy
 * version's `_idx + 8.` does. Padding bytes are dropped: the legacy dropped
 * NUL and 12800 (0x3200, a byte-order artefact of the VAX-era packing), and
 * this keeps both because dropping fewer would append junk to pointnames that
 * PCS DFIs then look up.
 */
PUBLIC FUN PTHEAD2_ASCII(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error) {

    PRIVATE FUN c(IN _i, IN _idx) {
        _j = _idx + 8;
        _ichr = _i[_j / 4] >> ((_j MOD 4) * 8);
        IF ((_ichr == 0) || (_ichr == 12800)) { RETURN("NULL"); }
        RETURN(CHAR(_ichr & 0xff));
    };

    IF (NOT PRESENT(_shot)) _shot = $SHOT;

    _error = 0;
    _ignored = PTHEAD2(_pointname, _shot, _error);
    IF (_error != 0) { RETURN(""); }

    _ascii = PUBLIC __ascii;
    IF (SIZE(_ascii) <= 2) { RETURN(""); }

    _text = "";
    _sec = 4 * _ascii[1];
    FOR (_i = 0; _i < _sec; _i++) {
        _achr = c(_ascii, _i);
        IF (_achr != "NULL") { _text = _text // _achr; }
    }
    RETURN(_text);
}
```

- [ ] **Step 2: Test against a PCS pointname**

The DFIs that use it are 2201/2202/2203, whose ASCII header holds the clock
pointname. Pick one from the survey output, or fall back to any point with a
nonempty ASCII section:

```bash
pixi run -e mds-validate python -c "
import MDSplus
print(repr(MDSplus.Data.execute('PTHEAD2_ASCII(\"<pcs point>\", 198873)').data()))
"
```
Expected: a clean pointname string with no trailing NULs or `\x32`.

- [ ] **Step 3: Commit**

```bash
git add tdi/fdp/PTHEAD2_ASCII.fun
git commit -m "feat(tdi): PTHEAD2_ASCII over the header ABI"
```

---

### Task 6: Build ptdata into the image

**Files:**
- Modify: `Containerfile.mdsip`

- [ ] **Step 1: Add the site TDI library**

One URL on the existing `dnf -y install`, next to the two MDSplus RPMs:

```dockerfile
      ${MDSPLUS_NOARCH}/mdsplus-d3d-${MDSPLUS_VERSION}.el9.noarch.rpm \
```

with a comment above the block:

```dockerfile
# Three packages now. mdsplus-d3d is the DIII-D site TDI library -- 118 .fun
# files including the PTHEAD_* family and the legacy PTDATA() shim, which the
# tree survey found is the majority caller. Upstream packages it, so this needs
# no clone, no credentials, and nothing for the service account to reach.
#
# Its copy has drifted from DIII-D/css-d3d-mdsplus in 15 of the 118. Of the
# functions trees actually call, only USING_SIGNAL's drift is on the
# data-retrieval path; DAMPHASE and LOADDATA differ too but both SPAWN ssh to a
# GA host and cannot work here under either version.
```

- [ ] **Step 2: Add a build stage for libptd3d**

```dockerfile
# --- ptdata build stage -----------------------------------------------------
# PTDATA_WITH_FDPIO=OFF is the load-bearing flag: without it libptd3d links
# libfdpio2 and can open a Pelican URL. With it, the library links only
# ${CMAKE_DL_LIBS} and the sandbox's no-network property holds by construction
# rather than by policy. Task 8 asserts that with readelf.
FROM almalinux:9 AS ptdata-build

ARG PTDATA_VERSION=2.2.0

RUN dnf -y install gcc-c++ cmake make git tar \
 && dnf clean all && rm -rf /var/cache/dnf

# The source, not a conda package: this image is RPM-based and installing conda
# into it to get one .so would drag a package manager into a sandbox whose
# threat model assumes the client has code execution.
ADD https://github.com/GA-FDP/ptdata/archive/refs/tags/release-${PTDATA_VERSION}.tar.gz /tmp/ptdata.tar.gz
RUN mkdir -p /tmp/ptdata && tar -xzf /tmp/ptdata.tar.gz -C /tmp/ptdata --strip-components=1

RUN cmake -S /tmp/ptdata/cpp -B /tmp/ptdata/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local/ptdata \
        -DPTDATA_WITH_FDPIO=OFF \
        -DPTDATA_BUILD_PYTHON=OFF \
 && cmake --build /tmp/ptdata/build -j"$(nproc)" \
 && cmake --install /tmp/ptdata/build

# Fail the build, not the deployment, if the flag stopped working.
RUN set -eux; \
    test -f /usr/local/ptdata/lib/libptd3d.so; \
    ! readelf -d /usr/local/ptdata/lib/libptd3d.so | grep NEEDED | grep -qE 'libfdpio|libXrd'
```

**Verify the option names against the tag before running this.**
`-DPTDATA_BUILD_PYTHON=OFF` and the install prefix layout are asserted here,
not measured; `cmake -S cpp -LAH` lists the real ones. A wrong option name is
ignored by CMake with only a warning, so a typo here silently builds *with*
fdpio and the `readelf` line above is what catches it.

- [ ] **Step 3: Copy the artifacts into the runtime stage**

In the main stage, after the MDSplus install:

```dockerfile
COPY --from=ptdata-build /usr/local/ptdata/lib/ /usr/local/ptdata/lib/
RUN echo /usr/local/ptdata/lib > /etc/ld.so.conf.d/ptdata.conf && ldconfig
```

No compiler, no headers, no source — only the shared objects.

- [ ] **Step 4: Install our TDI functions and set MDS_PATH**

```dockerfile
COPY tdi/fdp/ /usr/local/fdp/tdi/

# MDS_PATH is searched in order, first match wins, and it is a FLAT list --
# naming only a parent resolves PTDATA2 while leaving PTHEAD2 unresolved, a
# partial install that presents as a data problem rather than a packaging one.
# Ours goes first so the site versions are shadowed rather than deleted:
# removing one entry restores production behaviour exactly.
ENV MDS_PATH=/usr/local/fdp/tdi\;/usr/local/mdsplus/tdi/d3d\;/usr/local/mdsplus/tdi/d3d/ptdata\;/usr/local/mdsplus/tdi/d3d/ptdata2\;/usr/local/mdsplus/tdi/d3d/ptdata_historic\;/usr/local/mdsplus/tdi/d3d/global\;/usr/local/mdsplus/tdi/d3d/nimrod\;/usr/local/mdsplus/tdi\;/usr/local/mdsplus/tdi/remote

ENV PTD3D_LIBRARY=/usr/local/ptdata/lib/libptd3d.so
ENV PTDATA_JSON_INDEX_DIR=/ptdata-index
ENV PTDATA_JSON_INDEX_PATTERN=json_indexes_*
# Not optional. PTDATA_WITH_FDPIO=OFF removes remote FILE access but not
# ptserver, which is a raw socket -- so without this an index miss becomes a
# connection attempt from a container with no route anywhere, and reports as a
# network error rather than as absent data.
ENV PTDATA_PTSERVERS=none
```

- [ ] **Step 5: Build the Pelican-path chain**

```dockerfile
# The index records absolute Pelican URLs:
#   "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE"
# LocalIoProvider::resolve() passes its argument through unchanged, so that
# string reaches open(2) verbatim. It does NOT begin with '/', so POSIX treats
# it as relative and collapses the doubled slash -- with cwd at /, it resolves
# as /pelican:/osg-htc.org:443/fdp-d3d/archives/...
#
# So the fix is a directory chain, not code. REVISIT: the real fix is for the
# indexer to record archive-relative paths.
RUN mkdir -p '/pelican:/osg-htc.org:443' \
 && ln -s /fdp-archives '/pelican:/osg-htc.org:443/fdp-d3d' \
 && mkdir -p /fdp-archives/archives/ptdata /ptdata-index
```

- [ ] **Step 6: Make the working directory explicit**

socat's child inherits socat's cwd, so `WORKDIR` alone is not enough. Add
`cd /` to `fdp-mdsip-connection`, above the `exec`:

```sh
# The Pelican-path chain resolves RELATIVE to the working directory, so this
# is load-bearing, not hygiene. socat's child inherits socat's cwd, which is
# why WORKDIR is not sufficient. Without it every ptdata lookup fails as "file
# not found" with nothing pointing at the cause.
cd /
```

and assert it in the same `RUN` block that checks for `-m`:

```dockerfile
    grep -q '^cd /$' /usr/local/bin/fdp-mdsip-connection; \
```

- [ ] **Step 7: Build the image**

Run: `podman build -f Containerfile.mdsip -t fdp-mdsip .`
Expected: builds; the in-build `readelf` and `test -f` assertions pass.

- [ ] **Step 8: Commit**

```bash
git add Containerfile.mdsip
git commit -m "feat(sandbox): ptdata, the site TDI library, and the index config"
```

---

### Task 7: Mount the shotfiles and the index

**Files:**
- Modify: `scripts/mdsip-sandbox.sh`
- Modify: `deploy/fdp-mdsip.container`

Both, in the same commit. The quadlet's own header says every option mirrors
the script; a control in one and not the other is the failure that arrangement
exists to prevent.

- [ ] **Step 1: Add the mounts to the script**

Next to `TREES`/`TREE_MOUNT`:

```bash
# PTData shotfiles and the JSON index, both read-only. Empty disables ptdata
# entirely, which is the right default for the tree-only fixture the tests use
# -- a missing mount should look like "no ptdata configured", not like an
# archive that failed to appear.
PTDATA_ARCHIVE="${MDSIP_PTDATA_ARCHIVE:-}"
PTDATA_INDEX="${MDSIP_PTDATA_INDEX:-}"

PTDATA_FLAGS=()
if [ -n "$PTDATA_ARCHIVE" ]; then
  [ -d "$PTDATA_ARCHIVE" ] || { echo "no ptdata archive at $PTDATA_ARCHIVE"; exit 1; }
  # Nested to match what the index records: the index holds
  # .../fdp-d3d/archives/ptdata/... and the container's /fdp-d3d symlink points
  # at /fdp-archives, so the shotfiles must land under archives/ptdata there.
  # Mounting only .../ptdata keeps the rest of /mnt/beegfs/data out.
  PTDATA_FLAGS+=(-v "$(readlink -f "$PTDATA_ARCHIVE"):/fdp-archives/archives/ptdata:ro")
fi
if [ -n "$PTDATA_INDEX" ]; then
  [ -d "$PTDATA_INDEX" ] || { echo "no ptdata index at $PTDATA_INDEX"; exit 1; }
  PTDATA_FLAGS+=(-v "$(readlink -f "$PTDATA_INDEX"):/ptdata-index:ro")
fi
```

and `"${PTDATA_FLAGS[@]}"` in the `podman run` invocation, in the filesystem
group next to the tree mount.

- [ ] **Step 2: Add them to the quadlet**

```
# PTData shotfiles and the JSON index, read-only. Nested to match the absolute
# Pelican URLs the index records; see Containerfile.mdsip.
Volume=/mnt/beegfs/data/archives/ptdata:/fdp-archives/archives/ptdata:ro
Volume=/mnt/beegfs/data/archives/index/json:/ptdata-index:ro
```

- [ ] **Step 3: Start it and check the mounts landed**

```bash
MDSIP_PTDATA_ARCHIVE=/mnt/beegfs/data/archives/ptdata \
MDSIP_PTDATA_INDEX=/mnt/beegfs/data/archives/index/json \
  bash scripts/mdsip-sandbox.sh start
podman exec fdp-mdsip sh -c 'ls /ptdata-index | head; ls "/pelican:/osg-htc.org:443/fdp-d3d/archives/ptdata" | head'
```
Expected: index snapshot directories, and shotfile directories through the
symlink chain. The second command is the real check — it proves the chain
resolves, not just that the mount exists.

- [ ] **Step 4: Commit**

```bash
git add scripts/mdsip-sandbox.sh deploy/fdp-mdsip.container
git commit -m "feat(sandbox): read-only shotfile and index mounts"
```

---

### Task 8: Tests

**Files:**
- Create: `tests/integration/check_mds_path.py`
- Create: `tests/integration/check_pelican_path.py`
- Create: `tests/integration/test_ptdata_tdi.sh`
- Modify: `tests/mkpath.py` (the fixture tree gains an embedded-record node)
- Modify: `tests/security/verify_sandbox.py`

- [ ] **Step 1: A check that MDS_PATH resolves every function we depend on**

The spec asks for one that "fails loudly when `MDS_PATH` resolves `PTDATA2` but
not its helpers" — the flat-list failure mode.

```python
#!/usr/bin/env python
"""Every TDI function the ptdata path needs must resolve.

MDS_PATH is a flat list, not recursive, so naming a parent directory resolves
some functions and not others. That partial install presents as a data problem
-- %TDI-E-UNKNOWN_VAR from inside a tree expression -- rather than as the
packaging problem it is. This turns it back into a packaging problem.

Run inside the sandbox:  python tests/integration/check_mds_path.py
"""
import sys

import MDSplus

# Ours, then the shims that call them, then the site functions the tree survey
# found stored records calling directly.
REQUIRED = [
    "PTDATA2", "PTHEAD2", "PTHEAD2_ASCII", "PTD3D_LIBRARY",
    "PTDATA", "PTNPTS",
    "PTHEAD_IFIX", "PTHEAD_RFIX", "PTHEAD_REAL32", "PTHEAD_ASCII",
    "DAMPHASE", "USING_SIGNAL", "LOADDATA", "MULTIPHASE",
    "ECEPROF", "TECEPROF", "IP_PROBES", "IP_PROBES_Z", "ECHPWRC", "SLEEP",
]

missing = []
for name in REQUIRED:
    # Resolution without invocation: a function that exists is a routine, and
    # one that does not raises. Calling them would need arguments and would
    # fail for data reasons that have nothing to do with MDS_PATH.
    try:
        MDSplus.Data.execute(f"KIND({name})")
    except Exception:  # noqa: BLE001 -- any failure here means unresolved
        missing.append(name)

if missing:
    sys.exit(
        "MDS_PATH does not resolve: " + ", ".join(missing) +
        "\nMDS_PATH is a FLAT list -- each subdirectory needs its own entry."
    )
print(f"ok: all {len(REQUIRED)} functions resolve")
```

**Verify `KIND(<name>)` is the right resolution probe** before relying on it;
if it evaluates rather than resolves, use `TranslateLogical`-style lookup or
simply `f'{name}'` in a context that forces resolution. The check is worthless
if it passes for an unresolved name.

- [ ] **Step 2: An e2e test through a real connection**

```bash
#!/usr/bin/env bash
# PTDATA2 end to end: through mdsip, in the sandbox, over the real archive.
#
# The unit-level checks in Tasks 3-5 run in-process, where MDS_PATH and the
# working directory are whatever the shell had. This one goes through socat,
# so it is the only check that covers `cd /` and the container's environment.
set -euo pipefail

SHOT="${PTDATA_TEST_SHOT:-198873}"
POINT="${PTDATA_TEST_POINT:-IP}"
PORT="${MDSIP_PORT:-8000}"

python - "$SHOT" "$POINT" "$PORT" <<'EOF'
import sys
import MDSplus

shot, point, port = int(sys.argv[1]), sys.argv[2], sys.argv[3]
conn = MDSplus.Connection(f"127.0.0.1:{port}")

data = conn.get(f'PTDATA2("{point}", {shot})')
arr = data.data()
if arr.size <= 1:
    sys.exit(f"FAIL: PTDATA2 returned {arr!r} -- [0] means absent or unresolved")

times = data.dim_of().data()
if times.size != arr.size:
    sys.exit(f"FAIL: {arr.size} samples but {times.size} times")

npts = conn.get(f'PTNPTS("{point}", {shot})').data()
print(f"ok: {point} shot {shot}: {arr.size} samples, PTNPTS={npts}, "
      f"t=[{times[0]:.3f}, {times[-1]:.3f}] ms")
EOF
```

- [ ] **Step 3: A fixture tree whose node record embeds a PTDATA2 call**

Steps 2's `conn.get('PTDATA2(...)')` evaluates an expression the *client* sent.
The failure this whole project exists to fix is different: a **stored record**
that the server evaluates when a client reads an ordinary node. Those are the
`%TDI-E-UNKNOWN_VAR` failures on the deployed origin. Nothing above covers
that path, so it needs its own fixture.

`tests/mkpath.py` already builds the fixture tree the other e2e tests use;
extend it with one node whose record is an expression, not data:

```python
def add_ptdata_node(tree, point="IP", shot=198873):
    """A node whose stored record calls PTDATA2, like the real trees do.

    putData of a compiled expression stores the expression itself. Reading the
    node then makes the SERVER evaluate it -- which is the path that fails on
    the origin today, and the only path a client-side conn.get() does not
    exercise.
    """
    node = tree.addNode("PTDATA_EMBEDDED", "SIGNAL")
    node.putData(MDSplus.Data.compile(f'PTDATA2("{point}", {shot})'))
    return node
```

Then read it through the connection and require real samples:

```python
arr = conn.get("\\\\PTDATA_EMBEDDED").data()
if arr.size <= 1:
    sys.exit("FAIL: the embedded PTDATA2 record returned [0] -- "
             "this is the origin's current failure, unfixed")
```

Run: `bash tests/integration/test_ptdata_tdi.sh`
Expected: the embedded node returns the same array the direct call does.

- [ ] **Step 4: A check that the Pelican-path chain resolves, and that cwd is `/`**

```python
#!/usr/bin/env python
"""The Pelican-path chain, and the working directory it depends on.

Index entries are absolute Pelican URLs that reach open(2) verbatim. They do
not start with '/', so they resolve RELATIVE to the working directory -- which
makes `cd /` in fdp-mdsip-connection load-bearing rather than hygiene. socat's
child inherits socat's cwd, so WORKDIR does not cover it.

Run inside the container. Note when reading this: the symlink is ABSOLUTE, so
it only resolves in the container; a host-side version of this test needs a
relative symlink or it fails with ENOENT for reasons unrelated to the design.
"""
import os
import sys

CHAIN = "pelican:/osg-htc.org:443/fdp-d3d/archives/ptdata"

if os.getcwd() != "/":
    sys.exit(f"FAIL: cwd is {os.getcwd()!r}, not '/'. Every ptdata lookup will "
             f"fail as 'file not found' with nothing pointing at the cause.")

# Exactly as the engine sees it: a relative path with a doubled slash, which
# POSIX collapses. Resolving this is the whole trick.
probe = "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata"
if not os.path.isdir(probe):
    sys.exit(f"FAIL: {probe!r} does not resolve from {os.getcwd()!r}. "
             f"Expected the chain /{CHAIN}.")
print(f"ok: {probe!r} resolves relative to /")
```

The `cwd` assertion is only meaningful in the mdsip child, so run it *through*
the server rather than with `podman exec`, which starts a fresh process with
the image's `WORKDIR`:

```bash
python -c "
import MDSplus
c = MDSplus.Connection('127.0.0.1:8000')
print(c.get('MdsShr->TdiExecute(\"getcwd\")'))" 2>/dev/null || true
```

If TDI has no `getcwd`, assert it the way the engine will feel it instead: read
a point whose shotfile is only reachable through the chain, which Step 2
already does — and note here that the direct assertion was not available.

- [ ] **Step 5: Extend the sandbox verification with the no-remote-I/O assertions**

In `tests/security/verify_sandbox.py`, add a check that the shipped libptd3d
cannot do remote I/O — the property the whole no-network argument rests on:

```python
def check_ptdata_cannot_reach_the_network(exec_in_container):
    """libptd3d must not link fdpio or XRootD.

    This is the difference between "the sandbox has no route" and "the library
    cannot open a URL at all". Both are true today and only the second survives
    someone adding a network for an unrelated reason.

    NEEDED entries, not ldd output: ldd prints resolved paths and the build
    directory is named after the package, so a substring match on the path
    matches a perfectly clean binary.
    """
    out = exec_in_container(
        "readelf -d /usr/local/ptdata/lib/libptd3d.so | grep NEEDED"
    )
    bad = [ln for ln in out.splitlines() if "libfdpio" in ln or "libXrd" in ln]
    assert not bad, f"libptd3d links remote I/O: {bad}"
```

- [ ] **Step 6: Run them all**

```bash
bash scripts/mdsip-sandbox.sh start
podman exec fdp-mdsip python /usr/local/fdp/tests/check_mds_path.py
podman exec fdp-mdsip python /usr/local/fdp/tests/check_pelican_path.py
bash tests/integration/test_ptdata_tdi.sh
bash tests/security/verify_sandbox.sh
```
Expected: all pass. The two `podman exec` checks need their scripts inside the
image — add a `COPY tests/integration/check_*.py /usr/local/fdp/tests/` to
Task 6, or bind-mount them; pick one and make the Containerfile match rather
than leaving the commands above aspirational.

- [ ] **Step 7: Commit**

```bash
git add tests/integration/check_mds_path.py tests/integration/check_pelican_path.py \
        tests/integration/test_ptdata_tdi.sh tests/mkpath.py \
        tests/security/verify_sandbox.py
git commit -m "test(sandbox): ptdata end to end, embedded records, MDS_PATH, no remote I/O"
```

---

### Task 9: Equivalence against production

**Why this task exists.** Everything above proves our implementation runs. This
proves it is *right*. Replacing site code that has served DIII-D for two
decades is only defensible with a number-for-number comparison.

**Files:**
- Create: `tests/ptdata_equivalence.py`

- [ ] **Step 1: Write the comparison**

```python
#!/usr/bin/env python
"""Compare our PTDATA2 against production's, per ical, point by point.

Production is reached through a normal MDSplus connection to a server running
the site tdi/d3d; ours through the sandbox. Both are asked for the same
pointname, shot and ical, and the arrays must agree.

    python tests/ptdata_equivalence.py --production atlas.gat.com \\
        --sandbox 127.0.0.1:8000 --shot 198873

ical is {1, 2, 4} because that is the measured set across ~1,700 tree-embedded
calls spanning 14 years. 2 is the one that needs CalibrationMode::Volts, which
is why this cannot run before ptdata 2.1.0.
"""
import argparse
import sys

import numpy as np
import MDSplus

ICALS = [1, 2, 4]


def fetch(conn, point, shot, ical):
    sig = conn.get(f'PTDATA2("{point}", {shot}, {ical})')
    return np.asarray(sig.data()), np.asarray(sig.dim_of().data())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--production", required=True)
    ap.add_argument("--sandbox", required=True)
    ap.add_argument("--shot", type=int, required=True)
    ap.add_argument("--points", nargs="+", default=["IP", "BT", "DENSITY"])
    args = ap.parse_args()

    prod = MDSplus.Connection(args.production)
    sand = MDSplus.Connection(args.sandbox)

    failures = []
    for point in args.points:
        for ical in ICALS:
            try:
                pd, pt = fetch(prod, point, args.shot, ical)
                sd, st = fetch(sand, point, args.shot, ical)
            except Exception as exc:  # noqa: BLE001
                failures.append(f"{point} ical={ical}: raised {exc}")
                continue

            if pd.shape != sd.shape:
                failures.append(
                    f"{point} ical={ical}: {pd.shape} vs {sd.shape} samples")
                continue
            # rtol rather than exact: the legacy path accumulates in float32
            # and ours in double, so the last bits legitimately differ. A real
            # calibration disagreement is orders of magnitude, not ulps.
            if not np.allclose(pd, sd, rtol=1e-5, atol=0, equal_nan=True):
                worst = np.nanmax(np.abs(pd - sd) / np.maximum(np.abs(pd), 1e-30))
                failures.append(
                    f"{point} ical={ical}: data differs, worst rel {worst:.3e}")
            if not np.allclose(pt, st, rtol=1e-6, atol=1e-6, equal_nan=True):
                failures.append(f"{point} ical={ical}: time base differs")

    for f in failures:
        print("FAIL:", f)
    if failures:
        sys.exit(f"{len(failures)} disagreement(s)")
    print(f"ok: {len(args.points)} points x {len(ICALS)} icals agree")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it**

Run against a production MDSplus server and the sandbox.
Expected: agreement. **A disagreement here is a finding, not a test bug** —
investigate before adjusting a tolerance, and record what it was either way.
The likeliest real cause is a per-DFI calibration the legacy TDI applied that
the C++ reader does not, which is precisely what this test exists to surface.

- [ ] **Step 3: Also compare the header path**

The same comparison for `PTHEAD_RFIX` and `PTHEAD_REAL32`, since those are 548
calls that never look at `PTDATA2`:

```python
for point in args.points:
    for fn in ("PTHEAD_RFIX", "PTHEAD_REAL32"):
        p = np.asarray(prod.get(f'{fn}("{point}", {args.shot})').data())
        s = np.asarray(sand.get(f'{fn}("{point}", {args.shot})').data())
        if p.shape != s.shape or not np.allclose(p, s, rtol=1e-5, equal_nan=True):
            failures.append(f"{point} {fn}: header arrays differ")
```

- [ ] **Step 4: Commit**

```bash
git add tests/ptdata_equivalence.py
git commit -m "test(ptdata): per-ical equivalence against production"
```

---

### Task 10: Verify the drifted `USING_SIGNAL`

**Why.** The RPM's `using_signal.fun` differs from the site copy in a way that
is substantive rather than cosmetic: the site version opens the tree through
`TreeShr->TreeOpen` and returns `[0]` on failure, while the RPM's uses
`using(..., _shot, _tree)`. 161 stored-record calls depend on it, and it is the
only drifted function on the data-retrieval path.

**Files:**
- Modify: `tests/integration/test_ptdata_tdi.sh` (or a sibling)

- [ ] **Step 1: Find a record that calls it**

```bash
pixi run python tests/survey_tdi_calls.py --grep USING_SIGNAL --print-nodes
```

If the survey has no such flag, add one — it already decompiles every record,
so printing the node path alongside a matched call is a few lines. Expected: at
least one node path in `mhd` or `transport`, where the earlier survey found all
161 calls.

- [ ] **Step 2: Read that node through the sandbox and through production**

```bash
python -c "
import MDSplus, numpy as np
node, shot, tree = '\\\\<node from step 1>', 198873, '<tree>'
out = {}
for label, host in (('prod', 'atlas.gat.com'), ('sandbox', '127.0.0.1:8000')):
    c = MDSplus.Connection(host)
    c.openTree(tree, shot)
    out[label] = np.asarray(c.get(node).data())
    c.closeTree(tree, shot)
p, s = out['prod'], out['sandbox']
print('shapes', p.shape, s.shape)
print('equal:', p.shape == s.shape and np.allclose(p, s, rtol=1e-5, equal_nan=True))
"
```
Expected: `equal: True`.

- [ ] **Step 3: Decide, and record the decision**

- **Equal** → nothing to do; note in the spec that the drift was checked and is
  immaterial.
- **Different** → ship the site copy in `tdi/fdp/`, shadowing the RPM's the
  same way `PTDATA2` shadows its. One file, and the `MDS_PATH` order already
  supports it. Do **not** patch the RPM's copy in place: that makes the image
  contents differ from the package it claims to install.

- [ ] **Step 4: Commit**

```bash
git commit -am "test(sandbox): verify the USING_SIGNAL drift is immaterial"
```

---

### Task 11: Documentation

**Files:**
- Modify: `docs/security.md`, `README.md`
- Modify: `docs/superpowers/specs/2026-08-13-ptdata-in-the-mdsip-sandbox-design.md`

- [ ] **Step 1: Update the security posture note**

The spec's argument is that the added data is already inside the boundary — any
token holder may read everything under `/fdp-d3d/archives` read-only — so a
client with code execution gains nothing new. Write that into `docs/security.md`
alongside the existing mounts, together with the two properties that back it:
both mounts are read-only, and `libptd3d` cannot open a remote file at all.

- [ ] **Step 2: Note the syscall-capture debt**

`tests/security/capture_syscalls.sh` has not been re-run since the socat
entrypoint landed, and ptdata adds file I/O paths that may touch syscalls the
allowlist does not carry. Adding a mount does not obviously add syscalls, but
"obviously" is what the capture exists to replace.

- [ ] **Step 3: Mark the spec implemented**

Change its status line from "design, revised 2026-08-13 … Not yet implemented"
to implemented, with the date and a pointer to this plan.

- [ ] **Step 4: Commit**

```bash
git add docs/ README.md
git commit -m "docs: ptdata in the sandbox -- posture, debts, and status"
```

---

## What this plan deliberately does not do

- **No `ical` 3, 10-19 or 20.** Measured across ~1,700 tree-embedded calls over
  14 years; only `{1, 2, 4}` appear. An interactive caller using one of the
  others gets an error where production answers — a divergence, recorded in the
  spec's "Accepted consequences", and still a strict improvement over a sandbox
  where the function does not resolve at all.
- **No directory-scan fallback.** Resolution is index-only; coverage equals the
  index's coverage. A shot on disk but absent from the index is unavailable.
- **No indexer change.** The Pelican-path chain is a workaround for absolute
  URLs in the index. The real fix — archive-relative paths — needs the index
  regenerated and its consumers updated, and stays a follow-up.
