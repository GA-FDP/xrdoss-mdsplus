"""Which TDI functions do stored tree records actually call?

The sandbox must provide every TDI function a tree node's stored record
invokes, or that node fails with %TDI-E-UNKNOWN_VAR. The design spec measured
PTDATA2 usage; this answers the broader question, which decides whether the
sandbox needs the DIII-D site TDI library or only our own PTDATA2 / PTHEAD2.

Offline on purpose: reading a record through mdsip makes the *server* evaluate
it, which is precisely what fails today. `getNode(...).getRecord()` returns the
stored expression without evaluating it, and `decompile()` renders it as text.

Usage:

    pixi run -e mds-validate python tests/survey_tdi_calls.py <treeroot> [<treeroot> ...]

Each treeroot holds <tree>/<tree>_<shot>.tree. Classification uses three
authoritative lists rather than a hand-written guess:

  MDSPLUS  builtin opcodes (include/opcbuiltins.h) + kernel .fun files
  SITE     *.fun in a css-d3d-mdsplus checkout
  OURS     PTDATA2/PTHEAD2 and the legacy shims that delegate to them

Override the source paths with $MDSPLUS_SRC and $CSS_D3D_TDI.

Two things this gets right that a naive scan does not:

  * String literals are stripped before scanning. Descriptions and units are
    full of text like "ANGLE (degrees)" and "Pressure (mTorr)", which a bare
    NAME\\s*\\( regex reports as calls to ANGLE and MTORR.
  * Node references are excluded. A record referring to \\TOP.X:Y renders with
    names that are not calls.
"""

import collections
import os
import re
import sys

import MDSplus

MDSPLUS_SRC = os.environ.get(
    "MDSPLUS_SRC", "/fusion/projects/dt/sammuli/fdp_dev/repos/mdsplus-xrdcl")
CSS_D3D_TDI = os.environ.get(
    "CSS_D3D_TDI",
    "/fusion/projects/dt/sammuli/fdp_dev/repos/css-d3d-mdsplus/tdi")

# A call is NAME( with the name not preceded by a node-path character.
CALL_RE = re.compile(r"(?<![\\.:$\w])([A-Za-z_][A-Za-z0-9_]*)\s*\(")

# The functions we will provide ourselves.
OURS = {
    "PTDATA2", "PTHEAD2", "PTDATA", "PTHEAD", "PTHEAD2_IFIX", "PTHEAD2_ASCII",
    "PTHEAD2_REAL32", "PTHEAD2_SIZE", "PTCHAR2", "PTDATA_LIBRARY",
}


def strip_strings(text):
    """Blank out string literals, keeping length and structure.

    TDI descriptions and units are prose, and prose contains "WORD (" a lot.
    Replacing the contents with spaces (rather than deleting) keeps the rest
    of the expression aligned and avoids gluing tokens together.

    Both quote styles matter. Handling only double quotes leaves single-quoted
    prose intact, and that is not hypothetical: \\ECE::TOP.CAL:UNCERT:NOTES is
    'Array of uncertainty values ("error bars") ...', whose inner double-quoted
    span gets blanked and leaves `values (` looking like a call to VALUES.
    """
    out = []
    quote = None
    for ch in text:
        if quote is None and ch in '"\'':
            quote = ch
            out.append(" ")
        elif quote is not None:
            out.append(" ")
            if ch == quote:
                quote = None
        else:
            out.append(ch)
    return "".join(out)


def mdsplus_names():
    """Builtin opcodes plus every .fun shipped by the MDSplus kernel."""
    names = set()
    opc = os.path.join(MDSPLUS_SRC, "include", "opcbuiltins.h")
    if os.path.exists(opc):
        # Each OPC line carries two names: the internal one and the
        # TDI-visible one, e.g. "OPC ( BuildSignal, BUILD_SIGNAL, ...".
        # Taking only the first misses every BUILD_* / MAKE_* spelling that
        # actually appears in records.
        with open(opc, errors="replace") as fh:
            for m in re.finditer(
                    r"OPC\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
                    r"\s*([A-Za-z_][A-Za-z0-9_]*)", fh.read()):
                names.add(m.group(1).upper())
                names.add(m.group(2).upper())
    # NB: skip tdi/d3d -- this MDSplus fork vendors the DIII-D site library
    # there, and counting it as "MDSplus" would classify every site function
    # as provided and silently answer the question this survey exists to ask.
    tdi = os.path.join(MDSPLUS_SRC, "tdi")
    for root, _d, files in os.walk(tdi):
        if os.path.sep + "d3d" in root + os.path.sep:
            continue
        for f in files:
            if f.endswith(".fun"):
                names.add(os.path.splitext(f)[0].upper())
    return names


def site_names():
    names = set()
    for root, _d, files in os.walk(CSS_D3D_TDI):
        for f in files:
            if f.endswith(".fun"):
                names.add(os.path.splitext(f)[0].upper())
    return names


def walk_records(tree, shot):
    try:
        t = MDSplus.Tree(tree, shot, "READONLY")
        nodes = t.getNodeWild("***")
    except Exception as exc:  # noqa: BLE001
        print(f"  !! cannot open {tree}/{shot}: {exc}", file=sys.stderr)
        return
    for n in nodes:
        try:
            rec = n.getRecord()
        except Exception:  # noqa: BLE001 - most nodes have no record
            continue
        if rec is None:
            continue
        try:
            yield rec.decompile()
        except Exception:  # noqa: BLE001
            continue


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    mds = mdsplus_names()
    site = site_names()
    print(f"MDSplus names: {len(mds)} (from {MDSPLUS_SRC})")
    print(f"site names:    {len(site)} (from {CSS_D3D_TDI})\n")

    counts = collections.Counter()
    where = collections.defaultdict(set)

    for root in sys.argv[1:]:
        for tree_name in sorted(os.listdir(root)):
            tdir = os.path.join(root, tree_name)
            if not os.path.isdir(tdir):
                continue
            for fname in sorted(os.listdir(tdir)):
                if not fname.endswith(".tree"):
                    continue
                shot = int(fname.rsplit("_", 1)[1].split(".")[0])
                os.environ[f"{tree_name}_path"] = tdir
                n = 0
                for text in walk_records(tree_name, shot):
                    n += 1
                    for name in CALL_RE.findall(strip_strings(text)):
                        up = name.upper()
                        if up in mds:
                            continue
                        counts[up] += 1
                        where[up].add(f"{tree_name}/{shot}")
                print(f"  {tree_name}/{shot}: {n} records")

    ours = {k: v for k, v in counts.items() if k in OURS}
    site_used = {k: v for k, v in counts.items()
                 if k in site and k not in OURS}
    unknown = {k: v for k, v in counts.items()
               if k not in site and k not in OURS}

    def dump(title, d):
        print(f"\n{title} ({len(d)} distinct, {sum(d.values())} calls)")
        for k, v in sorted(d.items(), key=lambda kv: -kv[1]):
            locs = sorted(where[k])
            shown = ", ".join(locs[:4]) + (" ..." if len(locs) > 4 else "")
            print(f"  {k:<22} {v:>6}   {shown}")

    print("\n" + "=" * 72)
    dump("OURS -- provided by the sandbox", ours)
    dump("SITE -- needs css-d3d-mdsplus tdi/", site_used)
    dump("UNCLASSIFIED -- neither; investigate", unknown)

    print("\n" + "=" * 72)
    if not site_used:
        print("VERDICT: no site functions called. css-d3d can be dropped.")
    else:
        print(f"VERDICT: {len(site_used)} site functions are called; the "
              f"sandbox needs css-d3d-mdsplus tdi/.")


if __name__ == "__main__":
    main()
