"""A minimal inetd for the tests: one mdsip process per connection.

The sandbox image uses socat for this (see Containerfile.mdsip). socat is not a
dependency of the dev environment, so the tests get this instead -- same
contract, ~30 lines, no package to install.

Why not just `mdsip -m`: that flag isolates only the TDI context. The open tree
is process-global, so under -m concurrent clients silently read each other's
shots. A test server that behaves differently from the deployed one is worse
than no test server, because it makes the difference invisible.

    python tests/mdsip_inetd.py <port> <hostfile>
    python tests/mdsip_inetd.py </path/to.sock> <hostfile>

The second form listens on a Unix socket, as the sandbox does in production
(FDP_MDSIP_SOCKET in scripts/fdp-mdsip-server). mdsip serves an AF_UNIX peer on
fd 0 just as it serves a TCP one; its -p port is then only nominal.
"""

import os
import signal
import socket
import sys

MDSPLUS_BIN = os.environ.get("MDSIP_BIN", "mdsip")


def main():
    target, hostfile = sys.argv[1], sys.argv[2]

    # Reap children automatically; nothing here waits on them.
    signal.signal(signal.SIGCHLD, signal.SIG_IGN)

    if target.startswith("/"):
        # A stale socket file from an earlier run would make bind fail with
        # EADDRINUSE, as socat's unlink-early avoids in the image.
        try:
            os.unlink(target)
        except FileNotFoundError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(target)
        # Same reachability as the image's mode=666.
        os.chmod(target, 0o666)
        port = 8000                     # nominal: mdsip never binds it here
        where = target
    else:
        if not target.isdigit():
            sys.exit("first argument must be a TCP port or an absolute socket path")
        port = int(target)
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", port))
        where = str(port)
    srv.listen(128)
    print("mdsip_inetd listening on %s" % where, flush=True)

    while True:
        conn, _ = srv.accept()
        if os.fork() == 0:
            srv.close()
            # The socket goes on fd 0 ONLY. mdsip serves a single connection
            # from stdin (get_single_server_socket returns 0 on Linux), and its
            # own diagnostics must not land on fd 1/2 -- those would be written
            # into the protocol stream and the client would see a failed
            # connect rather than a log line.
            os.dup2(conn.fileno(), 0)
            if conn.fileno() > 2:
                conn.close()
            # No -m: one process per connection is the whole point.
            os.execvp(MDSPLUS_BIN,
                      [MDSPLUS_BIN, "-p", str(port), "-h", hostfile, "-c", "0"])
            os._exit(1)                                    # unreachable
        conn.close()


if __name__ == "__main__":
    main()
