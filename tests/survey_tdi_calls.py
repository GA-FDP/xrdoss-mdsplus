"""Which TDI functions do stored tree records actually call?

The sandbox must provide every TDI function a tree node's stored record
invokes, or that node fails with %TDI-E-UNKNOWN_VAR. The design spec measured
PTDATA2 usage; this answers the broader question, which decides whether the
sandbox needs the DIII-D site TDI library or only our own PTDATA2 / PTHEAD2.

Offline on purpose: reading a record through mdsip makes the *server* evaluate
it, which is precisely what fails today. `getNode(...).getRecord()` returns the
stored expression without evaluating it, and `decompile()` renders it as text.

Usage:

    python tests/survey_tdi_calls.py <treeroot> [<treeroot> ...]
    python tests/survey_tdi_calls.py <tree>:<shot> [<tree>:<shot> ...] [--globals]

Each positional argument is either a **directory** holding
<tree>/<tree>_<shot>.tree, or a **tree:shot** spec resolved through the
ambient MDSplus tree path -- which is how to survey trees that live on the
Pelican origin rather than on disk:

    cd ../toksearch_d3d && pixi run fdp run python \\
        ../xrdoss-mdsplus/tests/survey_tdi_calls.py d3d:198873 --globals

`fdp run` sets default_tree_path and the XRootD plumbing; the tree:shot form
deliberately does NOT set <tree>_path, so that ambient path is what resolves.
Reading records this way is still offline in the sense that matters: getRecord()
returns the stored expression, and nothing evaluates it.

`--globals` adds a second table counting *reads* of the PUBLIC globals the
legacy ptdata functions set as side effects. That is a different question from
the call counts -- a read of __rarray is not a call, so the call regex cannot
see it -- and it is the one that decides which side effects a replacement
PTHEAD2/PTDATA2 must reproduce.

Classification uses three authoritative lists rather than a hand-written guess:

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

# The PUBLIC globals the legacy ptdata functions set as side effects. A record
# that reads one of these depends on a replacement still setting it; the
# functions' return values are not the whole contract. PTHEAD_RFIX returns
# PUBLIC __rarray and PTHEAD_REAL32 returns PUBLIC __real32[2:*], so most of
# PTHEAD2's interface is here rather than in what it returns.
PTDATA_GLOBALS = [
    "__iarray", "__rarray", "__ascii", "__int16", "__int32",
    "__real32", "__real64", "__branch", "__crate", "__slot",
    "__ptdata_signal", "__ptdata_pointname", "__ptdata_shot",
]

# Word-boundary matched so __int16 does not also match __int168. A leading
# underscore is a word character, so \b before __ fires only after a
# non-word character -- which is what we want: x__int16 is not a reference.
GLOBAL_RE = {
    g: re.compile(r"\b" + re.escape(g) + r"\b") for g in PTDATA_GLOBALS
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


# Nodes the walk could not reach, by reason. Reported rather than swallowed:
# a survey that silently skips part of the tree reads as "covered everything".
SKIPPED = collections.Counter()


def walk_records(tree, shot):
    """Yield (node_path, decompiled_record) for every node carrying a record.

    Indexed rather than iterated, because constructing a TreeNode can raise and
    a raise inside `for n in nodes` ends the whole walk. Devices are the case
    that bites: MDSplus's compound.getDevice does `import imp`, removed in
    Python 3.11, so on a 3.12 interpreter every device node throws
    ModuleNotFoundError and would otherwise truncate the survey at the first
    one -- silently, and at a different point per tree.
    """
    try:
        t = MDSplus.Tree(tree, shot, "READONLY")
        nodes = t.getNodeWild("***")
        total = len(nodes)
    except Exception as exc:  # noqa: BLE001
        print(f"  !! cannot open {tree}/{shot}: {exc}", file=sys.stderr)
        return

    for i in range(total):
        try:
            n = nodes[i]
        except ModuleNotFoundError:
            SKIPPED["device node (MDSplus needs the removed `imp` module)"] += 1
            continue
        except Exception as exc:  # noqa: BLE001
            SKIPPED[f"node construction: {type(exc).__name__}"] += 1
            continue
        try:
            rec = n.getRecord()
        except Exception:  # noqa: BLE001 - most nodes simply have no record
            continue
        if rec is None:
            continue
        try:
            path = str(n.getFullPath())
        except Exception:  # noqa: BLE001
            path = "?"
        try:
            yield path, rec.decompile()
        except Exception as exc:  # noqa: BLE001
            SKIPPED[f"decompile: {type(exc).__name__}"] += 1
            continue


def targets(args):
    """Resolve positional arguments to (tree, shot, local_dir_or_None).

    A directory is walked for <tree>/<tree>_<shot>.tree and the per-tree path
    variable is set. A `tree:shot` spec sets nothing, so whatever the ambient
    environment says -- default_tree_path under `fdp run`, for instance --
    is what resolves it.
    """
    for arg in args:
        if os.path.isdir(arg):
            for tree_name in sorted(os.listdir(arg)):
                tdir = os.path.join(arg, tree_name)
                if not os.path.isdir(tdir):
                    continue
                for fname in sorted(os.listdir(tdir)):
                    if not fname.endswith(".tree"):
                        continue
                    shot = int(fname.rsplit("_", 1)[1].split(".")[0])
                    yield tree_name, shot, tdir
        elif ":" in arg:
            tree_name, _c, shot = arg.partition(":")
            yield tree_name, int(shot), None
        else:
            sys.exit(f"not a directory and not tree:shot -- {arg!r}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_globals = "--globals" in sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    mds = mdsplus_names()
    site = site_names()
    print(f"MDSplus names: {len(mds)} (from {MDSPLUS_SRC})")
    print(f"site names:    {len(site)} (from {CSS_D3D_TDI})\n")

    counts = collections.Counter()
    where = collections.defaultdict(set)
    gcounts = collections.Counter()
    gnodes = collections.defaultdict(set)

    for tree_name, shot, tdir in targets(args):
        if tdir is not None:
            os.environ[f"{tree_name}_path"] = tdir
        n = 0
        for path, text in walk_records(tree_name, shot):
            n += 1
            clean = strip_strings(text)
            for name in CALL_RE.findall(clean):
                up = name.upper()
                if up in mds:
                    continue
                counts[up] += 1
                where[up].add(f"{tree_name}/{shot}")
            if want_globals:
                # Against the string-stripped text for the same reason the
                # call scan is: a description mentioning __rarray is prose.
                for g, rx in GLOBAL_RE.items():
                    hits = len(rx.findall(clean))
                    if hits:
                        gcounts[g] += hits
                        gnodes[g].add(f"{tree_name}/{shot}{path}")
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

    if want_globals:
        print(f"\nPUBLIC GLOBALS read by stored records "
              f"({len(gcounts)} distinct, {sum(gcounts.values())} reads)")
        if not gcounts:
            print("  none -- no record reads any ptdata public global, so a "
                  "replacement need only get its RETURN values right.")
        for g in PTDATA_GLOBALS:
            v = gcounts.get(g, 0)
            nodes = sorted(gnodes[g])
            shown = ", ".join(nodes[:3]) + (" ..." if len(nodes) > 3 else "")
            print(f"  {g:<20} {v:>6}   {shown}")

    if SKIPPED:
        print(f"\nSKIPPED ({sum(SKIPPED.values())} nodes not scanned)")
        for reason, n in SKIPPED.most_common():
            print(f"  {n:>6}  {reason}")
        print("  These nodes' records were NOT searched. Treat the counts "
              "below as a lower bound.")

    print("\n" + "=" * 72)
    if not site_used:
        print("VERDICT: no site functions called. css-d3d can be dropped.")
    else:
        print(f"VERDICT: {len(site_used)} site functions are called; the "
              f"sandbox needs the DIII-D site tdi/ (mdsplus-d3d RPM).")


if __name__ == "__main__":
    main()
