#!/usr/bin/env python3
"""Do concurrent clients get their OWN tree, against a real deployment?

This is the acceptance test for the per-connection-process fix. Run it from a
client host against production after deploying:

    BEARER_TOKEN=$(cat ~/.fdp/token) \
    LD_LIBRARY_PATH=$PWD/build \
    python tests/integration/check_tree_contexts.py \
        fdp://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip \
        efit01 198873 198877 198878 198879 198880 198881 198882 198883

Exit status is 0 only if every connection saw exactly the shot it opened.

Why $SHOT and not the data: after openTree, $SHOT reports the shot open in THIS
connection's tree context. A server sharing one context across connections
returns whichever shot was opened last -- to every client, with a SUCCESS
status. Comparing returned DATA also works but needs shots whose data differs;
$SHOT needs nothing but distinct shot numbers.

Distinct shots are essential. If every connection opens the same shot, a server
with one shared context returns exactly what a correct one returns, and the
check cannot fail. That is not hypothetical: it is how the previous version of
this check passed while `mdsip -m` was serving every client the same tree.
"""

import os
import sys
from multiprocessing import get_context


def probe(args):
    url, tree, shot = args
    from MDSplus import Connection
    try:
        conn = Connection(url)
        conn.openTree(tree, shot)
        seen = sorted({int(conn.get("$SHOT").data()) for _ in range(10)})
        return shot, seen
    except Exception as exc:                       # noqa: BLE001 -- report, don't raise
        return shot, "ERROR: %s: %s" % (type(exc).__name__, exc)


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    url, tree = sys.argv[1], sys.argv[2]
    shots = [int(s) for s in sys.argv[3:]]
    if len(set(shots)) < 2:
        print("need at least 2 DISTINCT shots, or this check cannot fail")
        return 2

    if not os.environ.get("BEARER_TOKEN") and not os.path.exists(
            os.path.expanduser("~/.fdp/token")):
        print("warning: no BEARER_TOKEN and no ~/.fdp/token; expect 401",
              file=sys.stderr)

    print("%d concurrent connections -> %s" % (len(shots), url))
    with get_context("spawn").Pool(len(shots)) as pool:
        results = pool.map(probe, [(url, tree, s) for s in shots])

    wrong = 0
    for shot, seen in results:
        if isinstance(seen, str):
            print("  opened %d: %s" % (shot, seen))
            wrong += 1
        elif seen != [shot]:
            others = [s for s in seen if s != shot]
            print("  opened %d: $SHOT reported %s  <-- SAW ANOTHER CONNECTION'S "
                  "TREE %s" % (shot, seen, others))
            wrong += 1
        else:
            print("  opened %d: $SHOT always %d  ok" % (shot, shot))

    print()
    if wrong:
        print("FAIL: %d of %d connections did not get their own tree." %
              (wrong, len(shots)))
        print("The server is sharing one tree context across connections, so "
              "concurrent clients")
        print("silently read each other's shots. Check that mdsip runs one "
              "process per connection")
        print("(no -m) -- see Containerfile.mdsip.")
        return 1
    print("PASS: all %d connections kept their own tree context." % len(shots))
    return 0


if __name__ == "__main__":
    sys.exit(main())
