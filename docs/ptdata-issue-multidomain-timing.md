# Multi-domain time base repeats the boundary sample, and goes backwards

**Repo:** GA-FDP/ptdata · **Found:** 2026-08-17, by `xrdoss-mdsplus`'s
`tests/ptdata_equivalence.py` comparing the engine against production's legacy
`PTDATA2` on `atlas.gat.com`.

## Summary

For a point whose timing is **multi-domain** (several sample rates in one
record), `PtDataReader.fetch()` repeats the timestamp at each domain boundary
instead of advancing. The returned `times` array is therefore

- **non-monotonic** — it contains steps of `0` and, at one boundary, `-14 ms`
- **wrong by up to 20 ms** for every sample after the first transition
- **still the right length**, so nothing downstream notices

The `data` array is correct. Only `times` is affected, which is the worse
failure mode: the numbers look plausible and are attached to the wrong times.

## Reproduce

```python
import numpy as np, ptdata
r = ptdata.PtDataReader()
p = ptdata.ExtractionParams()
p.calibration = ptdata.calibration_mode_for_ical(1)
t = np.asarray(r.fetch("ZEFF01", 140054, p).times)

np.all(np.diff(t) > 0)          # False -- expected True
np.unique(np.round(np.diff(t), 6))
# ours:       [-14.   0.   0.2  2.  20. ]
# production: [  0.2  2.   6.   20. ]
```

Against production (`MDSplus.Connection("atlas.gat.com")`,
`DIM_OF(PTDATA2("ZEFF01", 140054, 1))`):

```
prod: ... -920. -918. -916. -896. -876. -856. ...   (2, 2, 20, 20, 20)
ours: ... -920. -918. -916. -916. -896. -876. ...   (2, 2,  0, 20, 20)
                          ^^^^^^ repeated, then one domain-step behind forever
```

Note that ours never produces the **6 ms** domain production has, and gains a
spurious `-14 ms` step — consistent with the boundary being consumed as a
sample rather than as a rate change.

## Scope

Measured on shot 140054 (DFI 2121, 4096 samples, 4 domain boundaries):

| Point | DFI | Samples | Non-positive steps | Max time error | Monotonic |
|---|---|---|---|---|---|
| `ZEFF01` | 2121 | 4096 | 4 | 20 ms | **no** |
| `ZEFF08` | 2121 | 4096 | 4 | 20 ms | **no** |
| `IP` @198873 | 2121 | 480256 | 0 | 0 | yes |
| `BT` @198873 | 2121 | 480256 | 0 | 0 | yes |

So it is **not** DFI-wide: the same DFI is correct when the record has a single
timing domain. It needs a record that actually uses several, which is why it
survived the existing test suite and the 198873 comparison.

`ZEFF01`–`ZEFF16` exist at 140054 but not at 160062, so the older campaigns are
where this shows up — precisely the archived data the FDP origin exists to
serve. All 16 `ZEFF` points were checked; the four sampled all fail identically.

## Why it matters here

`xrdoss-mdsplus` replaces the site `ptdata2.fun` with a thin TDI wrapper over
this engine. The legacy TDI fetched the 64-bit time array itself via
`ptdata64_` call type 22 and used it directly; the modern wrapper trusts
`ExtractedData.times`. So this bug reaches any tree node whose stored record
calls `PTDATA2` on a multi-domain point, and it reaches it silently.

Data values are unaffected, so a consumer that ignores the time axis is fine.
One that interpolates, resamples, or plots against time is not — and a
non-monotonic axis breaks `np.interp` and most plotting without an error.

## Suspected location

`cpp/src/dfi/domain_timing_dfi.cpp` — the loop that walks timing domains and
emits per-sample times. The signature (one repeated timestamp per boundary,
then a permanent one-step lag) reads like an off-by-one where the boundary
sample is emitted with the previous domain's time before the rate switches,
rather than the domain's first sample being emitted at the new rate.

Not fixed here: this is engine behaviour with its own test suite, and the fix
should come with a regression test built from a multi-domain record.
