#!/usr/bin/env python3
"""Evaluate TDI expressions and return MDSplus serialized descriptors.

Speaks a length-prefixed binary protocol over a unix socket so that MDSplus is
never linked into the XRootD server process: a crash, leak or hang in MDSplus
takes down a disposable worker rather than the origin.

The request body carries the same canonical structure the plugin encoded into
the object path (design spec section 4.6), so both sides share one format
rather than a translation step that could drift.

Wire protocol, all integers big-endian:

  request  : u32 total_len
             u16 tree_len | tree | i64 shot | canonical request bytes
  response : u8 status | u32 len | payload

  status 0 -> payload is an MDSplus serialized descriptor
  status 1 -> payload is a UTF-8 error message (whole-request failure)

Per-expression failures are NOT status 1; they appear as an "error" key inside
the returned dictionary, matching GetMany.execute() semantics.
"""

import argparse
import os
import socketserver
import struct
import sys
import threading

import numpy as np
import MDSplus
from MDSplus import Data, apd

STATUS_OK = 0
STATUS_ERROR = 1
REQUEST_VERSION = 1

# MDSplus keeps per-process TDI state, so concurrent connections must not
# interleave inside it.
_EVAL_LOCK = threading.Lock()


def serialized_bytes(data):
    """Data.serialize() returns an MDSplus Int8Array, not bytes."""
    return np.asarray(data.serialize(), dtype=np.int8).tobytes()


def parse_request(buf):
    """Decode the canonical request structure. Raises ValueError if malformed."""
    pos = 0

    def need(n):
        if pos + n > len(buf):
            raise ValueError("truncated request")

    need(3)
    if buf[0] != REQUEST_VERSION:
        raise ValueError("unsupported request version %d" % buf[0])
    (count,) = struct.unpack_from(">H", buf, 1)
    pos = 3
    if count == 0:
        raise ValueError("request contained no expressions")

    items = []
    for _ in range(count):
        need(2)
        (name_len,) = struct.unpack_from(">H", buf, pos)
        pos += 2
        need(name_len)
        name = buf[pos:pos + name_len].decode("utf-8")
        pos += name_len

        need(4)
        (exp_len,) = struct.unpack_from(">I", buf, pos)
        pos += 4
        need(exp_len)
        exp = buf[pos:pos + exp_len].decode("utf-8")
        pos += exp_len
        if not exp:
            raise ValueError("empty expression")

        need(1)
        nargs = buf[pos]
        pos += 1

        args = []
        for _ in range(nargs):
            need(4)
            (arg_len,) = struct.unpack_from(">I", buf, pos)
            pos += 4
            need(arg_len)
            args.append(buf[pos:pos + arg_len])
            pos += arg_len

        items.append((name, exp, args))

    if pos != len(buf):
        raise ValueError("trailing bytes in request")
    return items


def evaluate(tree_name, shot, items):
    """Return raw serialized bytes, or raise on a whole-request failure."""
    # TDI variables persist across execute() calls within a process, so a
    # long-lived worker would leak state between requests: "_x = 41" in one
    # request makes "_x + 1" return 42 in the next. That is both a
    # cross-request information leak and a cache-correctness bug, since the
    # same path would then yield different bytes depending on history.
    Data.execute("reset_private()")
    Data.execute("reset_public()")

    tree = None
    if tree_name:
        # A failure here is a whole-request error: the caller named a shot we
        # cannot open, so nothing in the batch can be evaluated.
        tree = MDSplus.Tree(tree_name, shot, "READONLY")
    try:
        result = {}
        for name, exp, raw_args in items:
            try:
                args = tuple(
                    Data.deserialize(np.frombuffer(a, dtype=np.int8)) for a in raw_args)
                if tree is not None:
                    value = tree.tdiExecute(exp, args) if args else tree.tdiExecute(exp)
                else:
                    value = Data.execute(exp, args) if args else Data.execute(exp)
                result[name] = apd.Dictionary({"value": Data(value)})
            except Exception as exc:                      # per-expression, in band
                result[name] = apd.Dictionary({"error": str(exc)})
        return serialized_bytes(apd.Dictionary(result))
    finally:
        if tree is not None:
            try:
                tree.close()
            except Exception:
                pass


def read_exactly(conn, count):
    buf = b""
    while len(buf) < count:
        chunk = conn.recv(count - len(buf))
        if not chunk:
            raise EOFError("connection closed mid-message")
        buf += chunk
    return buf


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        try:
            (length,) = struct.unpack(">I", read_exactly(conn, 4))
            body = read_exactly(conn, length)
            if len(body) < 10:
                raise ValueError("request header too short")
            (tree_len,) = struct.unpack_from(">H", body, 0)
            if 2 + tree_len + 8 > len(body):
                raise ValueError("truncated request header")
            tree = body[2:2 + tree_len].decode("utf-8")
            (shot,) = struct.unpack_from(">q", body, 2 + tree_len)
            items = parse_request(body[2 + tree_len + 8:])
        except Exception as exc:
            self.respond(STATUS_ERROR, str(exc).encode("utf-8"))
            return
        try:
            with _EVAL_LOCK:
                payload = evaluate(tree, shot, items)
            self.respond(STATUS_OK, payload)
        except Exception as exc:
            self.respond(STATUS_ERROR, str(exc).encode("utf-8"))

    def respond(self, status, payload):
        try:
            self.request.sendall(
                bytes([status]) + struct.pack(">I", len(payload)) + payload)
        except Exception:
            pass


class Server(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="unix socket path to listen on")
    args = parser.parse_args(argv)

    directory = os.path.dirname(os.path.abspath(args.socket))
    if directory:
        os.makedirs(directory, exist_ok=True)
    if os.path.exists(args.socket):
        os.unlink(args.socket)

    server = Server(args.socket, Handler)
    os.chmod(args.socket, 0o660)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if os.path.exists(args.socket):
            os.unlink(args.socket)
    return 0


if __name__ == "__main__":
    sys.exit(main())
