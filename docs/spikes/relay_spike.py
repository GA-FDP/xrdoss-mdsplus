#!/usr/bin/env python3
"""Spike: can mdsip's byte stream be cut into request/response pairs?

That is the only real unknown behind an HTTP relay. An HTTP request/response
cannot carry an open-ended bidirectional stream -- it carries one blob up and
one blob down. So a tunnel only works if the mdsip conversation is strictly:

    client sends a COMPLETE call (nargs messages)  ->  server sends ONE answer

This proxy proves or disproves exactly that, with no XRootD involved. It sits
between a stock MDSplus.Connection and a real mdsip server, accumulates whole
calls, forwards each as a unit, and relays back exactly one answer -- which is
precisely what the ext handler would do with BuffgetData/SendSimpleResp.

If a real Connection works through this, the tunnel model holds and the rest is
HTTP plumbing that Pelican and TPC already do.
"""

import socket
import struct
import sys
import threading

HDR = 48
# msglen, status, length, nargs, descriptor_idx, message_id, dtype,
# client_type, ndims, dims[8]
HDR_FMT = "Iihbbbbbbiiiiiiii"


def read_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_message(sock):
    """One mdsip message -> (bytes, nargs, idx) or None at EOF."""
    hdr = read_exactly(sock, HDR)
    if hdr is None:
        return None
    fields = struct.unpack(HDR_FMT, hdr)
    msglen, nargs, idx = fields[0], fields[3], fields[4]
    body = b""
    if msglen > HDR:
        body = read_exactly(sock, msglen - HDR)
        if body is None:
            return None
    return hdr + body, nargs, idx


def handle(client, upstream_host, upstream_port, stats):
    """One client connection == one relay session holding one mdsip connection.

    The per-session upstream connection is the crux: mdsip is STATEFUL.
    openTree sets the tree context on the connection, so a relay that opened a
    fresh mdsip connection per request would lose it and every get() would fail.
    """
    up = socket.create_connection((upstream_host, upstream_port))
    try:
        while True:
            # Accumulate a complete call.
            batch = b""
            while True:
                got = read_message(client)
                if got is None:
                    return
                msg, nargs, idx = got
                batch += msg
                if idx >= nargs - 1:
                    break
            stats["calls"] += 1

            # Forward the whole call as one unit -- an HTTP request body.
            up.sendall(batch)

            # Exactly one answer comes back -- an HTTP response body.
            ans = read_message(up)
            if ans is None:
                return
            stats["answers"] += 1
            client.sendall(ans[0])
    finally:
        up.close()
        client.close()


def main():
    listen_port = int(sys.argv[1])
    upstream_port = int(sys.argv[2])
    stats = {"calls": 0, "answers": 0}

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port))
    srv.listen(8)
    sys.stderr.write("relay-spike listening on %d -> mdsip %d\n"
                     % (listen_port, upstream_port))
    sys.stderr.flush()

    while True:
        conn, _ = srv.accept()
        t = threading.Thread(target=handle,
                             args=(conn, "127.0.0.1", upstream_port, stats),
                             daemon=True)
        t.start()


if __name__ == "__main__":
    main()
