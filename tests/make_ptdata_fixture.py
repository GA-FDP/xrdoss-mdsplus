#!/usr/bin/env python
"""Write a synthetic PTData shotfile, so the TDI wrappers can be tested offline.

The wrappers in tdi/fdp/ are thin transports over ptdata's C ABI, but "thin"
is a claim, not a proof -- the argument order of a 16-argument BUILD_CALL, the
element type of each ZERO(), and the millisecond/second distinction in the
time base are all silently wrong if guessed. Testing them needs real bytes on
disk, and the sandbox has no network.

This ports the PFI-42 record layout from ptdata's cpp/tests/test_helpers.h.
The port is verified rather than trusted: --verify reads the file back through
ptdata's own PtDataReader, so a fixture that disagrees with the engine's
format fails here instead of surfacing as a confusing TDI error later.

    python tests/make_ptdata_fixture.py /tmp/sysd3 --verify

Then point the wrappers at it:

    SYS_D3=/tmp/sysd3 SYS_D3_DELIM=';' PTDATA_PTSERVERS=none \\
    PTDATA_VAX_FLOATS=0 MDS_PATH="...;tdi/fdp;..." python ...

PTDATA_VAX_FLOATS=0 because these records are IEEE. Real shotfiles are VAX,
which is why the library defaults the other way.
"""

import argparse
import os
import struct
import sys

SHOT = 99001
BLOCK = 512

# (pointname, dfi, samples) -- two points so tests can prove results are keyed
# on the pointname rather than returned for whatever was asked last.
POINTS = [
    ("IP", 999, [10, 20, 30, 40, 50]),
    ("BT", 999, [-7, -14, -21]),
]

RC_OVER_G = -2.0
INHERENT = 1.0
START_TIME = 0.0
DELTA_TIME = 0.001


def _asc(s, n):
    return s.encode("ascii", "replace").ljust(n, b" ")[:n]


def build_preheader(uncompressed_size):
    pre = bytearray(32)
    struct.pack_into("<i", pre, 0, uncompressed_size)
    struct.pack_into("<H", pre, 4, 0)   # err_stat
    struct.pack_into("<H", pre, 30, 0)  # vax_stat
    return bytes(pre)


def build_pfi42_header(shot, pointname, dfi, data_word_count, bits, data_type):
    """192-byte fixed header. Offsets mirror test_helpers.h exactly."""
    h = bytearray(192)
    w32 = lambda off, v: struct.pack_into("<i", h, off, v)
    wf32 = lambda off, v: struct.pack_into("<f", h, off, v)
    wasc = lambda off, s, n: h.__setitem__(slice(off, off + n), _asc(s, n))

    w32(0, 42)                  # pfi
    w32(4, 96)                  # header_word_count (fixed only: 192/2)
    w32(8, shot)
    wasc(12, "D3D ", 4)         # experiment
    wasc(16, "CURR", 4)         # phase
    wasc(20, pointname, 12)     # pointname (12 chars at PFI 42)
    w32(32, 12)                 # hour
    w32(36, 0)                  # minute
    w32(40, 0)                  # second
    w32(44, 1)                  # month
    w32(48, 1)                  # day
    w32(52, 2024)               # year
    w32(56, 0)                  # point_type
    wasc(60, "Test signal", 24) # description
    wasc(84, "V", 4)            # units
    w32(88, 0)                  # revision
    wf32(92, START_TIME)
    wf32(96, DELTA_TIME)
    wf32(100, RC_OVER_G)
    wf32(104, INHERENT)
    w32(108, 0)                 # zero_offset
    w32(112, dfi)
    w32(116, data_word_count)   # what PTNPTS reads, as IARRAY(32)
    w32(120, bits)
    wasc(124, data_type, 4)
    w32(128, 0)                 # compression
    for off in range(132, 172, 4):   # reserved, 0xF0F0F0F0 as libd3 wrote it
        struct.pack_into("<I", h, off, 0xF0F0F0F0)
    w32(172, 0)                 # ascii_var_count
    w32(176, 0)                 # int16_var_count
    w32(180, 0)                 # int32_var_count
    w32(184, 0)                 # real32_var_count
    w32(188, 0)                 # real64_var_count
    return bytes(h)


def build_record(pointname, dfi, samples):
    data = b"".join(struct.pack("<h", v) for v in samples)
    pre = build_preheader(192 + 32 + len(data))
    hdr = build_pfi42_header(SHOT, pointname, dfi, len(samples), 16, "IN  ")
    return pre + hdr + data


def build_shot_file(records, num_entries=2048):
    """16-byte file header + directory + block-aligned point records."""
    entry_size, header_size = 16, 16
    dir_bytes = num_entries * entry_size
    dir_blocks = (header_size + dir_bytes + BLOCK - 1) // BLOCK
    next_vbn = dir_blocks + 1

    total_blocks = dir_blocks + sum(
        (len(r) + BLOCK - 1) // BLOCK for _n, r in records)
    f = bytearray(total_blocks * BLOCK)

    struct.pack_into("<i", f, 0, num_entries)
    struct.pack_into("<i", f, 4, len(records))
    struct.pack_into("<i", f, 8, total_blocks)
    struct.pack_into("<i", f, 12, next_vbn + len(records))

    vbn = next_vbn
    for i, (name, rec) in enumerate(records):
        off = header_size + i * entry_size
        f[off:off + 10] = _asc(name, 10)
        # VBN and word count are 24-bit little-endian, not 32.
        f[off + 10:off + 13] = vbn.to_bytes(3, "little")
        f[off + 13:off + 16] = (len(rec) // 2).to_bytes(3, "little")
        start = (vbn - 1) * BLOCK
        f[start:start + len(rec)] = rec
        vbn += (len(rec) + BLOCK - 1) // BLOCK
    return bytes(f)


def verify(root):
    """Read the fixture back through ptdata's own reader.

    A hand-ported binary layout that the engine cannot parse is worse than no
    fixture: every downstream failure looks like a TDI bug.
    """
    os.environ.update({
        "SYS_D3": root, "SYS_D3_DELIM": ";",
        "PTDATA_PTSERVERS": "none", "PTDATA_VAX_FLOATS": "0",
    })
    import ptdata

    reader = ptdata.PtDataReader()
    ok = True
    for name, _dfi, samples in POINTS:
        res = reader.fetch(name, SHOT)
        got = list(res.data) if len(res.data) else list(res.raw_integer)
        if [int(v) for v in got] != samples:
            print(f"  FAIL {name}: expected {samples}, got {got}")
            ok = False
        else:
            print(f"  ok   {name}: {got}")

    hdr = ptdata.PtDataHeader(POINTS[0][0], SHOT)
    npts = int(hdr.iarray[31])          # IARRAY(32), what PTNPTS returns
    if npts != len(POINTS[0][2]):
        print(f"  FAIL iarray[31]={npts}, expected {len(POINTS[0][2])}")
        ok = False
    else:
        print(f"  ok   iarray[31] = {npts} (PTNPTS)")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="directory to use as SYS_D3")
    ap.add_argument("--verify", action="store_true",
                    help="read the fixture back through ptdata")
    args = ap.parse_args()

    os.makedirs(args.root, exist_ok=True)
    records = [(n, build_record(n, d, s)) for n, d, s in POINTS]
    path = os.path.join(args.root, f"{SHOT}.PLA")
    with open(path, "wb") as fh:
        fh.write(build_shot_file(records))
    print(f"wrote {path} ({os.path.getsize(path)} bytes, "
          f"{len(POINTS)} points, shot {SHOT})")

    if args.verify and not verify(args.root):
        sys.exit("fixture does not read back correctly")


if __name__ == "__main__":
    main()
