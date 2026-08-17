#!/usr/bin/env python
"""Every TDI function the ptdata path needs must resolve.

A function that does not resolve fails as %TDI-E-UNKNOWN_VAR from inside a
tree expression -- which reads as a data problem and is a packaging one. This
turns it back into a packaging problem, at build or start time rather than
when someone reads a node.

Run inside the sandbox:  python /usr/local/fdp/tests/check_mds_path.py

The probe is `NAME()`, and the distinction is the EXCEPTION TYPE rather than
whether it raises. Measured 2026-08-16:

    resolved, needs no args   -> returns
    resolved, needs args      -> %TDI-E-MISS_ARG
    not resolved              -> %TDI-E-UNKNOWN_VAR

A bare `NAME`, or `KIND(NAME)`, is useless as a probe: TDI reads it as a node
reference and raises TreeNOT_OPEN for resolved and unresolved names alike, so
a check built on either passes for everything including typos.
"""

import sys

import MDSplus

# Ours.
OURS = ["PTDATA2", "PTHEAD2", "PTHEAD2_ASCII", "PTD3D_LIBRARY"]

# The shims that reach ours, and the site functions the tree survey found
# stored records calling directly.
SITE = [
    "PTDATA", "PTNPTS",
    "PTHEAD_IFIX", "PTHEAD_RFIX", "PTHEAD_REAL32", "PTHEAD_ASCII",
    "PTHEAD_INT16", "PTHEAD_INT32",
    "DAMPHASE", "USING_SIGNAL", "LOADDATA", "MULTIPHASE",
    "ECEPROF", "TECEPROF", "IP_PROBES", "IP_PROBES_Z", "ECHPWRC", "SLEEP",
]

# Kernel functions that are not builtins and that the above depend on.
# TranslateLogical is used by the site's own ptdata2.fun; GetManyExecute is
# what every batch request goes through.
KERNEL = ["TranslateLogical", "GetManyExecute"]


def resolves(name):
    try:
        MDSplus.Data.execute(f"{name}()")
        return True
    except Exception as exc:  # noqa: BLE001 -- the type is the signal
        return type(exc).__name__ != "TdiUNKNOWN_VAR"


def main():
    missing = []
    for group, names in (("ours", OURS), ("site", SITE), ("kernel", KERNEL)):
        for name in names:
            ok = resolves(name)
            print(f"  {'ok  ' if ok else 'FAIL'} {group:<7} {name}")
            if not ok:
                missing.append(name)

    # Guard against the probe silently passing everything: a name that cannot
    # exist must come back unresolved. Without this, a change to MDSplus's
    # error types would turn this whole check into a no-op that reports green.
    if resolves("NO_SUCH_FUNCTION_XYZZY"):
        sys.exit("PROBE BROKEN: a nonexistent name reported as resolved, so "
                 "this check proves nothing. Fix the probe before trusting it.")

    if missing:
        sys.exit(
            "MDS_PATH does not resolve: " + ", ".join(missing) +
            "\nEntries are searched in order and recursively; check that both "
            "/usr/local/fdp/tdi and /usr/local/mdsplus/tdi are on it.")
    print(f"ok: all {len(OURS) + len(SITE) + len(KERNEL)} functions resolve")


if __name__ == "__main__":
    main()
