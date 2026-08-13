"""Package test for mdsip-fdp: is the transport where MDSplus will look for it?

Two layers, because they fail differently:

  1. Resolution (always runs, no network). MDSplus finds a transport by
     dlopen-ing "lib" + "MdsIp" + SCHEME and resolving the symbol Io. If that
     fails it silently falls back to the ssh-tunnel transport -- no error, just
     the wrong protocol -- so "did it resolve?" cannot be answered by seeing
     whether a connection works. It is answered here by asking LoadIo directly.

  2. A live fetch (skipped without a token). Proves the shipped binary actually
     talks to the production origin.

The whole process re-execs itself with LD_LIBRARY_PATH scrubbed first. The point
of the package is that no such variable is needed: libMdsShr.so has
RPATH $ORIGIN/., so a transport installed alongside it is found on its own. A
test that inherited a search path would pass without demonstrating that.
"""

import ctypes
import os
import sys

SCRUBBED = "_MDSIP_FDP_TEST_SCRUBBED"
DEFAULT_URL = "fdp://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip"
TREE, SHOT, SIGNAL = "efit01", 198873, r"\ipmhd"


def reexec_without_library_path():
    """Re-run this script with the loader's search-path variables removed.

    glibc reads LD_LIBRARY_PATH once at process start, so clearing it in
    os.environ would not affect any later dlopen. It has to be a new process.
    """
    env = dict(os.environ)
    for var in ("LD_LIBRARY_PATH", "MDSPLUS_LIBRARY_PATH", "MDSPLUS_DIR"):
        env.pop(var, None)
    env[SCRUBBED] = "1"
    os.execve(sys.executable, [sys.executable, os.path.abspath(__file__)], env)


def check_resolution():
    """LoadIo('fdp') must return this package's transport, not the fallback."""
    from MDSplus.version import load_library

    mdsipshr = load_library("MdsIpShr")
    mdsipshr.LoadIo.restype = ctypes.c_void_p
    mdsipshr.LoadIo.argtypes = [ctypes.c_char_p]

    # LoadIo returns &tunnel_routines for anything it cannot find, so the
    # fallback's address is what "not found" looks like.
    fallback = mdsipshr.LoadIo(b"no-such-protocol")
    tcp = mdsipshr.LoadIo(b"tcp")
    fdp = mdsipshr.LoadIo(b"fdp")

    print("  LoadIo(unknown) -> 0x%x  (the fallback)" % (fallback or 0))
    print("  LoadIo(tcp)     -> 0x%x" % (tcp or 0))
    print("  LoadIo(fdp)     -> 0x%x" % (fdp or 0))

    # Positive control first. Without it, a broken LoadIo -- returning NULL for
    # everything, say -- would make the real assertion below pass by accident.
    assert fallback, "LoadIo returned NULL for an unknown protocol; test is not measuring anything"
    assert tcp and tcp != fallback, (
        "LoadIo('tcp') fell back, so MDSplus cannot load ANY transport here. "
        "That is an MDSplus installation problem, not an mdsip-fdp one."
    )

    assert fdp != fallback, (
        "LoadIo('fdp') returned the fallback: libMdsIpFDP.so was not found or "
        "did not load. Check that it is installed next to libMdsShr.so and is "
        "named exactly libMdsIpFDP.so."
    )
    assert fdp != tcp, "LoadIo('fdp') returned the TCP transport"
    print("  resolved, with no LD_LIBRARY_PATH set")


def check_live():
    """Fetch a real signal through the production origin."""
    url = os.environ.get("FDP_TEST_URL") or DEFAULT_URL
    from MDSplus import Connection

    print("  %s" % url)
    conn = Connection(url)
    conn.openTree(TREE, SHOT)
    data = conn.get(SIGNAL).data()
    assert data.size > 0, "%s came back empty" % SIGNAL
    print("  %s %s/%d -> %s" % (SIGNAL, TREE, SHOT, data.shape))


def main():
    if not os.environ.get(SCRUBBED):
        reexec_without_library_path()  # does not return

    print("resolution:")
    check_resolution()

    print("live fetch:")
    if not os.environ.get("BEARER_TOKEN"):
        # Forks have no access to repo secrets, and an offline build has no
        # origin. Skipping is right; skipping silently is not.
        print("  SKIPPED: BEARER_TOKEN is unset")
        return
    check_live()


if __name__ == "__main__":
    main()
