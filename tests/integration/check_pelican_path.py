#!/usr/bin/env python
"""The Pelican-path chain, and the working directory it depends on.

The ptdata index records ABSOLUTE Pelican URLs, and LocalIoProvider::resolve()
passes its argument through unchanged, so a string like

    pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE

reaches open(2) verbatim. It does not begin with '/', so POSIX treats it as
RELATIVE and collapses the doubled slash -- with the working directory at /,
it resolves through a directory chain the image builds:

    /pelican:/osg-htc.org:443/fdp-d3d  ->  /fdp-archives
    /fdp-archives/archives/ptdata          the read-only mount

That makes `cd /` in fdp-mdsip-connection load-bearing rather than hygiene:
socat's child inherits socat's cwd, so the image's WORKDIR does not cover it.

Run inside the sandbox:  python /usr/local/fdp/tests/check_pelican_path.py

Note when reading this: the symlink is ABSOLUTE, so it only resolves inside
the container. A host-side version of this test needs a relative symlink, or
it fails with ENOENT for reasons that have nothing to do with the design.
"""

import os
import sys

# Exactly as the engine sees it -- a relative path with a doubled slash.
PROBE = "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata"
CHAIN = "/pelican:/osg-htc.org:443/fdp-d3d"

failures = []

cwd = os.getcwd()
if cwd != "/":
    failures.append(
        f"cwd is {cwd!r}, not '/'. Every ptdata lookup will fail as "
        f"'file not found' with nothing pointing at the cause.")
else:
    print("  ok   cwd is /")

if not os.path.islink(CHAIN):
    failures.append(f"{CHAIN!r} is not a symlink -- the image did not build "
                    f"the chain, or the federation prefix changed.")
else:
    print(f"  ok   {CHAIN} -> {os.readlink(CHAIN)}")

# The real test: resolve the string the way the engine will, from where the
# server will be standing. Checking the chain's components separately would
# pass while the thing that matters still failed.
if not os.path.isdir(PROBE):
    failures.append(
        f"{PROBE!r} does not resolve from {cwd!r}. The Pelican host and "
        f"federation prefix are encoded in a DIRECTORY NAME; if either "
        f"changed, every lookup fails as 'file not found'.")
else:
    print(f"  ok   {PROBE!r} resolves relative to /")

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(f"{len(failures)} failure(s)")
print("ok: the Pelican-path chain resolves")
