#!/usr/bin/env python
"""Build a tree whose stored record CALLS PTDATA2, like the real DIII-D trees.

This is the failure the whole ptdata-in-the-sandbox effort exists to fix, and
it is not the same thing as a client sending `PTDATA2(...)` over a connection:

    client-sent expression   the CLIENT composes it; the server evaluates a
                             string it was handed
    stored record            the client reads an ordinary node, and the SERVER
                             evaluates an expression baked into the tree

Only the second is what fails on the deployed origin today with
%TDI-E-UNKNOWN_VAR, and only the second exercises the path a real user takes.
putData of a compiled expression stores the expression itself rather than its
value, which is what makes the node behave like the ones in the d3d trees.

    python tests/make_ptdata_tree.py <tree-dir> [pointname] [ptdata-shot]

Then mount <tree-dir> into the sandbox and read \\FDPT::TOP:PTDATA_EMBEDDED.
"""

import os
import sys

import MDSplus

TREE = "fdpt"
SHOT = 99001


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tree_dir = os.path.abspath(sys.argv[1])
    point = sys.argv[2] if len(sys.argv) > 2 else "IP"
    ptshot = int(sys.argv[3]) if len(sys.argv) > 3 else SHOT

    os.makedirs(tree_dir, exist_ok=True)
    os.environ[f"{TREE}_path"] = tree_dir

    t = MDSplus.Tree(TREE, SHOT, "NEW")

    # The node under test: its record is an expression, not data.
    n = t.addNode("PTDATA_EMB", "SIGNAL")
    n.putData(MDSplus.Data.compile(f'PTDATA2("{point}", {ptshot}, 0)'))

    # A control: the same fetch through the legacy PTDATA() shim, which the
    # survey found is the MAJORITY caller in real trees (1,259 calls against
    # PTDATA2's 1,210). It reaches our implementation only by delegating, so a
    # wrapper that works when called directly and fails here would otherwise
    # look fine.
    n2 = t.addNode("PTNPTS_EMB", "NUMERIC")
    n2.putData(MDSplus.Data.compile(f'PTNPTS("{point}", {ptshot})'))

    # A node holding ordinary data, so a test can tell "the server cannot
    # evaluate records" apart from "the tree did not open".
    n3 = t.addNode("PLAIN", "NUMERIC")
    n3.putData(MDSplus.Int32(42))

    t.write()
    t.close()

    # Read the names BACK rather than printing the ones we asked for. MDSplus
    # caps node names at 12 characters and truncates silently: PTDATA_EMBEDDED
    # became PTDATA_EMBED, and every read of the name we thought we wrote came
    # back %TREE-W-NNF, which reads like the record failing to evaluate.
    t = MDSplus.Tree(TREE, SHOT, "READONLY")
    print(f"wrote {TREE} shot {SHOT} in {tree_dir}")
    for node in t.getNodeWild("***"):
        name = str(node.getNodeName())
        if name == "TOP":
            continue
        print(f"  \\{TREE.upper()}::TOP:{name}")


if __name__ == "__main__":
    main()
