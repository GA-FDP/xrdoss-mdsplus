import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
import pytest
from MDSplus import Data

EVALD = os.path.join(os.path.dirname(__file__), "..", "evaluator", "fdp_mdsplus_evald.py")


def build_request(items):
    """items: list of (name, exp, [arg_bytes, ...]) -> canonical bytes (spec 4.6)."""
    out = struct.pack(">BH", 1, len(items))
    for name, exp, args in items:
        nb, eb = name.encode(), exp.encode()
        out += struct.pack(">H", len(nb)) + nb
        out += struct.pack(">I", len(eb)) + eb
        out += struct.pack(">B", len(args))
        for a in args:
            out += struct.pack(">I", len(a)) + a
    return out


def serialize_arg(tdi):
    return np.asarray(Data.execute(tdi).serialize(), dtype=np.int8).tobytes()


@pytest.fixture(scope="module")
def server():
    sock_path = os.path.join(tempfile.mkdtemp(), "evald.sock")
    proc = subprocess.Popen([sys.executable, EVALD, "--socket", sock_path])
    for _ in range(100):
        if os.path.exists(sock_path):
            break
        time.sleep(0.1)
    else:
        proc.kill()
        raise RuntimeError("evald did not create its socket")
    yield sock_path
    proc.terminate()
    proc.wait(timeout=10)


def roundtrip(sock_path, items, tree="", shot=0):
    tb = tree.encode()
    payload = struct.pack(">H", len(tb)) + tb + struct.pack(">q", shot) + build_request(items)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock_path)
        s.sendall(struct.pack(">I", len(payload)) + payload)
        header = b""
        while len(header) < 5:
            chunk = s.recv(5 - len(header))
            if not chunk:
                raise RuntimeError("server closed before responding")
            header += chunk
        status = header[0]
        (length,) = struct.unpack(">I", header[1:5])
        body = b""
        while len(body) < length:
            chunk = s.recv(length - len(body))
            if not chunk:
                break
            body += chunk
    return status, body


def decode(body):
    return Data.deserialize(np.frombuffer(body, dtype=np.int8))


def test_evaluates_treeless_expression(server):
    status, body = roundtrip(server, [("r0", "[1.0,2.0,3.0]", [])])
    assert status == 0
    assert np.allclose(np.asarray(decode(body)["r0"]["value"]), [1.0, 2.0, 3.0])


def test_applies_a_single_argument(server):
    status, body = roundtrip(server, [("r0", "$ * 2", [serialize_arg("21")])])
    assert status == 0
    assert int(np.asarray(decode(body)["r0"]["value"])) == 42


def test_applies_multiple_arguments_in_order(server):
    status, body = roundtrip(
        server, [("r0", "$ - $", [serialize_arg("50"), serialize_arg("8")])])
    assert status == 0
    assert int(np.asarray(decode(body)["r0"]["value"])) == 42


def test_applies_an_array_argument(server):
    status, body = roundtrip(server, [("r0", "$ * 10", [serialize_arg("[1.,2.,3.]")])])
    assert status == 0
    assert np.allclose(np.asarray(decode(body)["r0"]["value"]), [10.0, 20.0, 30.0])


def test_batch_returns_every_name(server):
    status, body = roundtrip(server, [("a", "1", []), ("b", "2", []), ("c", "3", [])])
    assert status == 0
    result = decode(body)
    assert [int(np.asarray(result[k]["value"])) for k in ("a", "b", "c")] == [1, 2, 3]


def test_expression_with_awkward_characters(server):
    # Quotes, a comma inside a string, parentheses, spaces.
    status, body = roundtrip(server, [("s", "concat('a,b', ' c')", [])])
    assert status == 0
    assert str(decode(body)["s"]["value"]) == "a,b c"


def test_per_expression_error_is_in_band(server):
    status, body = roundtrip(server, [("bad", "this_is_not_tdi(", [])])
    assert status == 0, "a bad expression must not fail the whole request"
    assert "error" in decode(body)["bad"]


def test_mixed_success_and_failure(server):
    status, body = roundtrip(
        server, [("ok", "42", []), ("bad", "nonexistent_function_xyz()", [])])
    assert status == 0
    result = decode(body)
    assert int(np.asarray(result["ok"]["value"])) == 42
    assert "error" in result["bad"]


def test_tdi_state_does_not_leak_between_requests(server):
    # A long-lived worker must not let one request's TDI variables affect the
    # next, or the same cached path could return different bytes over time.
    status, _ = roundtrip(server, [("set", "_leaky = 41", [])])
    assert status == 0
    status, body = roundtrip(server, [("get", "_leaky + 1", [])])
    assert status == 0
    assert "error" in decode(body)["get"], "TDI state leaked across requests"


def test_public_tdi_state_does_not_leak_either(server):
    status, _ = roundtrip(server, [("set", "public _pleaky = 7", [])])
    assert status == 0
    status, body = roundtrip(server, [("get", "_pleaky", [])])
    assert status == 0
    assert "error" in decode(body)["get"], "public TDI state leaked across requests"


def test_identical_requests_give_identical_bytes(server):
    # The response is cached under a path derived from the request, so equal
    # requests must produce equal bytes or the cache would be serving lies.
    _, first = roundtrip(server, [("r0", "[1.0,2.0,3.0]", [])])
    _, second = roundtrip(server, [("r0", "[1.0,2.0,3.0]", [])])
    assert first == second


def test_missing_tree_is_whole_request_error(server):
    status, body = roundtrip(server, [("r0", "1", [])], tree="no_such_tree", shot=1)
    assert status == 1
    assert body


def test_malformed_request_is_whole_request_error(server):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(server)
        junk = b"\x02\x00\x01"          # bad version byte
        payload = struct.pack(">H", 0) + struct.pack(">q", 0) + junk
        s.sendall(struct.pack(">I", len(payload)) + payload)
        header = b""
        while len(header) < 5:
            header += s.recv(5 - len(header))
    assert header[0] == 1


def test_empty_request_is_rejected(server):
    payload = struct.pack(">H", 0) + struct.pack(">q", 0) + struct.pack(">BH", 1, 0)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(server)
        s.sendall(struct.pack(">I", len(payload)) + payload)
        header = b""
        while len(header) < 5:
            header += s.recv(5 - len(header))
    assert header[0] == 1


def test_trailing_bytes_are_rejected(server):
    body = build_request([("r0", "1", [])]) + b"junk"
    payload = struct.pack(">H", 0) + struct.pack(">q", 0) + body
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(server)
        s.sendall(struct.pack(">I", len(payload)) + payload)
        header = b""
        while len(header) < 5:
            header += s.recv(5 - len(header))
    assert header[0] == 1
