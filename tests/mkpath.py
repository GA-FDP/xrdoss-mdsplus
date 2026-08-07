#!/usr/bin/env python3
"""Build a virtual-file object path from name=expression pairs.

    mkpath.py <tree|-> <shot> name=expr [name=expr ...]

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


def build_payload(items):
    """items: [(name, expression)] -> serialised APD list bytes."""
    lst = apd.List([apd.Dictionary({"name": n, "exp": e, "args": ()})
                    for n, e in items])
    # serialize() returns an MDSplus Int8Array, not bytes.
    return np.asarray(lst.serialize(), dtype=np.int8).tobytes()


def shot_bucket(shot):
    d = "%08d" % (shot // 100)
    return "/".join(d[i:i + 2] for i in range(0, 8, 2))


def build_path(tree, shot, items):
    enc = base64.urlsafe_b64encode(build_payload(items)).decode().rstrip("=")
    chunks = [enc[i:i + MAX_SEGMENT] for i in range(0, len(enc), MAX_SEGMENT)]
    return "%s/%s/%s/%d/%s" % (PREFIX, tree, shot_bucket(shot), shot, "/".join(chunks))


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
