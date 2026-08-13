"""A minimal inetd for the tests: one mdsip process per connection.

The sandbox image uses socat for this (see Containerfile.mdsip). socat is not a
dependency of the dev environment, so the tests get this instead -- same
contract, ~30 lines, no package to install.

Why not just `mdsip -m`: that flag isolates only the TDI context. The open tree
is process-global, so under -m concurrent clients silently read each other's
shots. A test server that behaves differently from the deployed one is worse
than no test server, because it makes the difference invisible.

    python tests/mdsip_inetd.py <port> <hostfile>
"""

import os
import signal
import socket
import sys

MDSPLUS_BIN = os.environ.get("MDSIP_BIN", "mdsip")


def main():
    port, hostfile = int(sys.argv[1]), sys.argv[2]

    # Reap children automatically; nothing here waits on them.
    signal.signal(signal.SIGCHLD, signal.SIG_IGN)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(128)
    print("mdsip_inetd listening on %d" % port, flush=True)

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
