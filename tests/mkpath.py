#!/usr/bin/env python3
"""Build a virtual-file object path from name=expression pairs.

    mkpath.py <tree|-> <shot> name=expr [name=expr ...]

Set FDP_TREES to the archive root to have the version token resolved from the
tree file, exactly as the plugin does. Without it the version is "-", which the
plugin accepts only for tree-less requests.

The payload is exactly what MDSplus's GetMany builds and serialises: an APD
list of {name, exp, args} dictionaries. The plugin never interprets it -- it
hands the bytes to GetManyExecute($) -- so this file is the single reference
implementation of the path grammar on the Python side.

Keep the chunking rule in step with fdp::kMaxSegment. A second implementation
of this grammar that disagrees with the first does not raise an error; it
produces paths that miss cache or fail to parse, which is far harder to spot.
"""

import base64
import sys

import numpy as np
from MDSplus import apd

MAX_SEGMENT = 249   # fdp::kMaxSegment -- NAME_MAX(255) minus room for ".cinfo"
PREFIX = "/tdi"
NO_VERSION = "-"


def tree_version(tree, shot):
    """Mirror of fdp::TreeVersion -- FNV-1a over (inode, size, mtime).

    A second implementation of a rule the plugin also implements, so it has to
    agree exactly or every request 404s. Kept here rather than shelling out so
    the tests can build paths without a running origin.
    """
    import os
    if not tree or tree == NO_VERSION:
        return NO_VERSION
    root = os.environ.get("FDP_TREES")
    if not root:
        return NO_VERSION
    # The test archive is flat; the plugin's templates cover the branch layout.
    for cand in (os.path.join(root, "%s_%d.datafile" % (tree, shot)),
                 os.path.join(root, "codes", tree, shot_bucket(shot),
                              "%s_%d.datafile" % (tree, shot))):
        try:
            st = os.stat(cand)
        except OSError:
            continue
        h = 1469598103934665603
        for val in (st.st_ino, st.st_size, int(st.st_mtime)):
            for b in val.to_bytes(8, "little"):
                h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return "v%016x" % h
    raise SystemExit("mkpath: no tree file for %s shot %d under %s" % (tree, shot, root))


def build_payload(items):
    """items: [(name, expression)] -> serialised APD list bytes."""
    lst = apd.List([apd.Dictionary({"name": n, "exp": e, "args": ()})
                    for n, e in items])
    # serialize() returns an MDSplus Int8Array, not bytes.
    return np.asarray(lst.serialize(), dtype=np.int8).tobytes()


def shot_bucket(shot):
    d = "%08d" % (shot // 100)
    return "/".join(d[i:i + 2] for i in range(0, 8, 2))


def build_path(tree, shot, items, version=None):
    enc = base64.urlsafe_b64encode(build_payload(items)).decode().rstrip("=")
    chunks = [enc[i:i + MAX_SEGMENT] for i in range(0, len(enc), MAX_SEGMENT)]
    if version is None:
        version = tree_version(tree, shot)
    return "%s/%s/%s/%d/%s/%s" % (PREFIX, tree, shot_bucket(shot), shot,
                                  version, "/".join(chunks))


def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    tree, shot = argv[1], int(argv[2])
    items = [tuple(a.split("=", 1)) for a in argv[3:]]
    print(build_path(tree, shot, items))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
