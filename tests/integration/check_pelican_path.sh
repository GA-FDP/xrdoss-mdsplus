#!/bin/sh
# The Pelican-path chain, and the working directory it depends on.
#
# The ptdata index records ABSOLUTE Pelican URLs, and LocalIoProvider::resolve()
# passes its argument through unchanged, so a string like
#
#     pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE
#
# reaches open(2) verbatim. It does not begin with '/', so POSIX treats it as
# RELATIVE and collapses the doubled slash -- with the working directory at /,
# it resolves through a directory chain the image builds:
#
#     /pelican:/osg-htc.org:443/fdp-d3d  ->  /fdp-archives
#     /fdp-archives/archives/ptdata          the read-only mount
#
# That makes `cd /` in fdp-mdsip-connection load-bearing rather than hygiene:
# socat's child inherits socat's cwd, so the image's WORKDIR does not cover it.
#
# POSIX sh rather than Python: the sandbox image has python3 but NOT the
# MDSplus bindings, and adding mdsplus-python to get them would enlarge the
# attack surface of a container whose premise is that a client already has
# code execution inside it. Nothing here needs more than test(1).
#
#   podman run --rm --workdir / --entrypoint /bin/sh fdp-mdsip \
#       /usr/local/fdp/tests/check_pelican_path.sh
#
# Note the --workdir: without it this passes or fails for the wrong reason.
# Note also that the symlink is ABSOLUTE, so it only resolves inside the
# container; a host-side run needs a relative symlink or it fails with ENOENT
# for reasons unrelated to the design.
set -u

CHAIN='/pelican:/osg-htc.org:443/fdp-d3d'
# Exactly as the engine sees it: relative, with the doubled slash.
PROBE='pelican://osg-htc.org:443/fdp-d3d/archives/ptdata'

rc=0

if [ "$(pwd)" != "/" ]; then
    echo "FAIL: cwd is $(pwd), not /. Every ptdata lookup will fail as"
    echo "      'file not found' with nothing pointing at the cause."
    rc=1
else
    echo "  ok   cwd is /"
fi

if [ ! -L "$CHAIN" ]; then
    echo "FAIL: $CHAIN is not a symlink -- the image did not build the chain,"
    echo "      or the Pelican federation prefix changed."
    rc=1
else
    echo "  ok   $CHAIN -> $(readlink "$CHAIN")"
fi

# The real test: resolve the string the way the engine will, from where the
# server will be standing. Checking the chain's components separately would
# pass while the thing that actually matters still failed.
if [ ! -d "$PROBE" ]; then
    echo "FAIL: '$PROBE' does not resolve from $(pwd)."
    echo "      The Pelican host and federation prefix are encoded in a"
    echo "      DIRECTORY NAME; if either changed, every lookup fails as"
    echo "      'file not found'."
    rc=1
else
    echo "  ok   '$PROBE' resolves relative to /"
fi

[ "$rc" -eq 0 ] && echo "ok: the Pelican-path chain resolves"
exit "$rc"
