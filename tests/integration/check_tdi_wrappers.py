#!/usr/bin/env python
"""Exercise tdi/fdp's PTDATA2 / PTHEAD2 / PTHEAD2_ASCII against a fixture.

In-process, so this covers the wrappers themselves rather than the sandbox --
tests/integration/test_ptdata_tdi.sh is the one that goes through mdsip and
therefore covers `cd /`, MDS_PATH and the container environment.

Setup, either by env var or by the defaults below:

    tests/make_ptdata_fixture.py <dir>       writes <dir>/99001.PLA
    SYS_D3=<dir> SYS_D3_DELIM=';' PTDATA_PTSERVERS=none PTDATA_VAX_FLOATS=0
    PTD3D_LIBRARY=<path to libptd3d.so>
    MDS_PATH="<repo>/tdi/fdp;<mdsplus>/tdi;<mdsplus>/tdi/mdsshr"

MDS_PATH must name tdi/mdsshr explicitly: it is a FLAT list, and
TranslateLogical -- which PTD3D_LIBRARY uses, as does the site library's
ptdata2.fun -- lives in that subdirectory, not in tdi/ itself.

The fixture's DFI is 999, which has no dedicated handler, so GenericDfi serves
it and produces NO time base. That is deliberate: it is the branch where
PTDATA2 must synthesize times from the header, and the branch where the
seconds-to-milliseconds scaling actually matters.
"""

import os
import sys

import MDSplus

SHOT = int(os.environ.get("PTDATA_FIXTURE_SHOT", "99001"))
IP = [10.0, 20.0, 30.0, 40.0, 50.0]
BT = [-7.0, -14.0, -21.0]

failures = []


def check(label, got, want, tol=None):
    if tol is not None:
        ok = len(got) == len(want) and all(
            abs(a - b) <= tol for a, b in zip(got, want))
    else:
        ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {label}: {got!r}")
    if not ok:
        failures.append(f"{label}: expected {want!r}, got {got!r}")


def main():
    e = MDSplus.Data.execute

    print("PTHEAD2")
    # iarray[31] is 0-based for legacy IARRAY(32) -- data_word_count, which is
    # exactly what PTNPTS returns. If this is wrong every npts in every tree
    # is wrong.
    check("iarray[31] (what PTNPTS returns)",
          int(e(f'PTHEAD2("IP", {SHOT})[31]').data()), len(IP))
    check("SIZE(__iarray)", int(e("SIZE(PUBLIC __iarray)").data()), 50)
    check("SIZE(__rarray)", int(e("SIZE(PUBLIC __rarray)").data()), 20)
    # rarray[3] inherent_number, rarray[4] rc_over_g -- the fixture's values.
    check("rarray[3:5]",
          [round(float(v), 6) for v in e("PUBLIC __rarray[3:4]").data()],
          [1.0, -2.0])

    # Empty sections are length 2, not 0: the two control words are always
    # present. PTHEAD_REAL32 ABORTs on exactly this, as the legacy did.
    for sec in ("__ascii", "__int16", "__int32", "__real32", "__real64"):
        check(f"SIZE({sec}) -- two control words",
              int(e(f"SIZE(PUBLIC {sec})").data()), 2)

    # The C ABI writes int16_t here. ZERO(n,0) would allocate int32 and the
    # copy would land in half the buffer, which looks like plausible garbage.
    check("__int16 element type",
          str(e("PUBLIC __int16").data().dtype), "int16")

    print("PTDATA2")
    sig = e(f'PTDATA2("IP", {SHOT}, 0)')
    check("data (ical=0, raw counts)", [float(v) for v in sig.data()], IP)
    # Synthesized from rarray start/delta, which are SECONDS -- so 0.001 s
    # must arrive as 1 ms. Tolerance because rarray is float32.
    check("times, seconds scaled to ms",
          [round(float(v), 3) for v in sig.dim_of().data()],
          [0.0, 1.0, 2.0, 3.0, 4.0], tol=1e-3)
    check("dim units", str(sig.dim_of().units), "ms")

    # Keyed on pointname: returning the previous point's samples would be the
    # worst failure available -- plausible numbers on the wrong signal.
    check("a second pointname gets its own data",
          [float(v) for v in e(f'PTDATA2("BT", {SHOT}, 0)').data()], BT)

    print("error semantics")
    check("absent pointname returns [0]",
          [float(v) for v in e(f'PTDATA2("NOPE", {SHOT}, 0)').data()], [0.0])
    try:
        e(f'PTDATA2("IP", {SHOT}, 3)')
        failures.append("ical=3 did not raise -- a calibration was substituted")
        print("  FAIL unsupported ical did not raise")
    except Exception as exc:  # noqa: BLE001 -- any raise is the pass condition
        print(f"  ok   unsupported ical raises ({type(exc).__name__})")

    print("PTHEAD2_ASCII")
    check("empty ascii section decodes to ''",
          str(e(f'PTHEAD2_ASCII("IP", {SHOT})').data()), "")
    check("absent pointname decodes to ''",
          str(e(f'PTHEAD2_ASCII("NOPE", {SHOT})').data()), "")

    print()
    if failures:
        for f in failures:
            print("FAIL:", f)
        sys.exit(f"{len(failures)} failure(s)")
    print("all checks passed")


if __name__ == "__main__":
    main()
