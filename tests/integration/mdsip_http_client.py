#!/usr/bin/env python3
"""A local mdsip listener that forwards each call to the HTTP relay.

This is the client half of the tunnel, in Python. It exists to test the ext
handler against a real, unmodified `MDSplus.Connection`: point a Connection at
this process and every call travels as an HTTP POST to the origin's
`XrdHttpMdsip` handler, which relays it to a real mdsip server.

It is deliberately the same shape as the eventual `libMdsIpFDP.so`
(docs/client-transport.md) -- accumulate a complete call, POST it, write back
the one answer -- so what passes here is what that transport has to reproduce.

Usage:  mdsip_http_client.py <listen-port> <origin-host:port> <url-prefix>
"""

import http.client
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


class RelaySession(object):
    """One HTTP session against the ext handler, holding one mdsip connection."""

    def __init__(self, origin, prefix):
        self.origin = origin
        self.prefix = prefix
        self.conn = http.client.HTTPConnection(origin, timeout=120)
        self.token = self._post("/connect", b"").decode("ascii").strip()
        if not self.token:
            raise RuntimeError("relay returned an empty session token")

    def _post(self, action, body, headers=None):
        hdrs = {"Content-Length": str(len(body))}
        if headers:
            hdrs.update(headers)
        self.conn.request("POST", self.prefix + action, body=body, headers=hdrs)
        resp = self.conn.getresponse()
        payload = resp.read()
        if resp.status != 200:
            raise RuntimeError("relay %s -> %d %s: %r"
                               % (action, resp.status, resp.reason, payload[:200]))
        return payload

    def call(self, batch):
        return self._post("/msg", batch, {"X-Fdp-Session": self.token})

    def close(self):
        try:
            self._post("/close", b"", {"X-Fdp-Session": self.token})
        except Exception:
            pass
        self.conn.close()


def handle(client, origin, prefix):
    session = RelaySession(origin, prefix)
    try:
        while True:
            # Accumulate a complete call: mdsip sends one message per argument,
            # and only the last (descriptor_idx == nargs - 1) ends it. An HTTP
            # request carries one blob, so it must carry the whole call.
            batch = b""
            while True:
                got = read_message(client)
                if got is None:
                    return
                msg, nargs, idx = got
                batch += msg
                if idx >= nargs - 1:
                    break

            client.sendall(session.call(batch))
    finally:
        session.close()
        client.close()


def main():
    listen_port = int(sys.argv[1])
    origin = sys.argv[2]
    prefix = sys.argv[3] if len(sys.argv) > 3 else "/mdsip"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port))
    srv.listen(8)
    sys.stderr.write("mdsip-http-client on %d -> http://%s%s\n"
                     % (listen_port, origin, prefix))
    sys.stderr.flush()

    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, origin, prefix),
                         daemon=True).start()


if __name__ == "__main__":
    main()
