#!/usr/bin/env python
"""Compare the modern ptdata engine against production's legacy PTDATA2.

Replacing TDI code that has served DIII-D for two decades is only defensible
with a number-for-number comparison, per ical, on real shots.

    cd ../toksearch_d3d && pixi run fdp run python \\
        ../xrdoss-mdsplus/tests/ptdata_equivalence.py --shot 198873

What this compares, and why that split:

    reference   production mdsip (atlas.gat.com), evaluating the SITE
                ptdata2.fun -- ~250 lines of TDI calibration arithmetic
    subject     ptdata's PtDataReader, the C++ engine our PTDATA2.fun wraps

It deliberately does NOT go through the sandbox. The wrapper is a thin
transport and is already covered end to end by check_tdi_wrappers.py and the
embedded-record test; the sandbox has only a synthetic fixture, not the real
archive. What is untested until here is whether the ENGINE reproduces the
legacy per-DFI calibration, which is where a real disagreement would live.

ical is {1, 2, 4} because that is the measured set across ~1,700 tree-embedded
calls spanning 14 years. 2 is the one that needed CalibrationMode::Volts.

A disagreement here is a FINDING, not a test bug. Investigate before adjusting
a tolerance: the likeliest real cause is a per-DFI calibration the legacy TDI
applied that the C++ reader does not, which is exactly what this exists to
surface.
"""

import argparse
import sys

import numpy as np

import MDSplus
import ptdata

ICALS = [1, 2, 4]

# ptdata deliberately refuses CalibrationMode::Volts on the PCS DFIs: their
# legacy calibration branch was never validated against real data, so it raises
# rather than returning numbers it cannot vouch for. Production answers, so
# this shows up here as a disagreement -- and it is an ACCEPTED one, measured:
# all 64 ical=2 call sites across spectroscopy and d3d at shots 160062/140054
# are ZEFF01-16, which are DFI 2121. No tree-embedded call reaches a PCS DFI
# with ical=2.
#
# Listed explicitly rather than tolerated silently: if a tree ever does ask for
# it, that is a finding, and this set is where to record having checked.
PCS_DFIS = {2200, 2201, 2202, 2203}
EXPECTED_DIVERGENCE = "ical=2 (Volts) on a PCS DFI"
DEFAULT_POINTS = ["IP", "BT", "DENSITY", "ECHPWRC", "PINJ"]


def reference(conn, point, shot, ical):
    """Production's legacy PTDATA2: data and its time base, in ms."""
    data = np.asarray(conn.get(f'PTDATA2("{point}", {shot}, {ical})').data())
    times = np.asarray(
        conn.get(f'DIM_OF(PTDATA2("{point}", {shot}, {ical}))').data())
    return data, times


def subject(reader, point, shot, ical):
    """The modern engine, at the same calibration."""
    params = ptdata.ExtractionParams()
    params.calibration = ptdata.calibration_mode_for_ical(ical)
    r = reader.fetch(point, shot, params)
    data = np.asarray(r.data if len(r.data) else r.raw_integer, dtype=float)
    return data, np.asarray(r.times, dtype=float)


def dfi_of(reader, point, shot):
    try:
        return reader.read_header(point, shot).fixed_header.dfi
    except Exception:  # noqa: BLE001
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--production", default="atlas.gat.com")
    ap.add_argument("--shot", type=int, required=True)
    ap.add_argument("--points", nargs="+", default=DEFAULT_POINTS)
    ap.add_argument("--rtol", type=float, default=1e-5)
    args = ap.parse_args()

    print(f"ptdata {ptdata.__version__} vs {args.production}, shot {args.shot}")
    conn = MDSplus.Connection(args.production)
    reader = ptdata.PtDataReader()

    failures, compared, skipped, diverged = [], 0, 0, 0
    for point in args.points:
        for ical in ICALS:
            label = f"{point} ical={ical}"
            try:
                pd, pt = reference(conn, point, args.shot, ical)
            except Exception as exc:  # noqa: BLE001
                print(f"  --   {label}: production raised "
                      f"({type(exc).__name__}); skipped")
                skipped += 1
                continue
            # Production returns [0] for absent data. Nothing to compare, and
            # counting it as agreement would let a point that exists nowhere
            # look like a pass.
            if pd.size <= 1:
                print(f"  --   {label}: production returned {pd!r}; skipped")
                skipped += 1
                continue
            try:
                sd, st = subject(reader, point, args.shot, ical)
            except Exception as exc:  # noqa: BLE001
                if ical == 2 and dfi_of(reader, point, args.shot) in PCS_DFIS:
                    print(f"  --   {label}: {EXPECTED_DIVERGENCE}; "
                          f"production answers, we refuse")
                    diverged += 1
                    continue
                failures.append(f"{label}: engine raised {type(exc).__name__}: {exc}")
                print(f"  FAIL {label}: engine raised {type(exc).__name__}")
                continue

            compared += 1
            if pd.shape != sd.shape:
                failures.append(f"{label}: {pd.shape} vs {sd.shape} samples")
                print(f"  FAIL {label}: {pd.shape} vs {sd.shape}")
                continue

            # rtol rather than exact: the legacy path accumulates in float32
            # and ours in double, so the last bits legitimately differ. A real
            # calibration disagreement is orders of magnitude, not ulps.
            if not np.allclose(pd, sd, rtol=args.rtol, atol=0, equal_nan=True):
                denom = np.maximum(np.abs(pd), 1e-30)
                worst = float(np.nanmax(np.abs(pd - sd) / denom))
                failures.append(f"{label}: data differs, worst rel {worst:.3e}")
                print(f"  FAIL {label}: worst rel {worst:.3e}")
                continue

            tmsg = ""
            if pt.shape == st.shape and not np.allclose(
                    pt, st, rtol=1e-6, atol=1e-6, equal_nan=True):
                failures.append(f"{label}: time base differs")
                tmsg = "  (TIME BASE DIFFERS)"
            elif pt.shape != st.shape:
                failures.append(f"{label}: time base {pt.shape} vs {st.shape}")
                tmsg = f"  (time base {pt.shape} vs {st.shape})"
            print(f"  ok   {label}: {pd.size} samples agree{tmsg}")

    print(f"\ncompared {compared}, skipped {skipped}, "
          f"expected-divergence {diverged}, failed {len(failures)}")
    for f in failures:
        print("FAIL:", f)
    if failures:
        sys.exit(f"{len(failures)} disagreement(s)")
    if compared == 0:
        sys.exit("nothing compared -- every case was skipped, so this proves "
                 "nothing. Pick pointnames that exist for this shot.")
    print("engine agrees with production")


if __name__ == "__main__":
    main()
