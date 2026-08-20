"""Assert the wire contract against a running handler. Exit non-zero on failure."""

import os
import sys

# Tier 1 and tier 3 must be out of the way or the client answers locally and
# never calls the server -- the failure mode that made ptdata's own suite pass
# while proving nothing.
os.environ.pop("PTDATA_JSON_INDEX_DIR", None)
os.environ["PTDATA_SKIP_PLUGIN"] = "1"
os.environ["PTDATA_PTSERVERS"] = "none"

import ptdata


def code_of(exc):
    """PtDataError.code is a plain int and ErrorCode is not an IntEnum, so the
    obvious `exc.code == ptdata.ErrorCode.X` is always False -- a check that
    reads correctly and never fires. Coerce before comparing."""
    return ptdata.ErrorCode(exc.code)


ENDPOINT = os.environ["ENDPOINT"]
SHOTS = os.environ["SHOTS_DIR"]

local = ptdata.PtDataReader(sys_d3_paths=[SHOTS])
remote = ptdata.PtDataReader(sys_d3_paths=[], http_endpoint=ENDPOINT)
params = ptdata.ExtractionParams()

failures = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


expected = local.fetch("IP", 165920, params, ".MAG")

got = remote.fetch("IP", 165920, params, ".MAG")
check("matches a local reader on data", list(got.data) == list(expected.data),
      f"n={len(expected.data)}")
check("matches on times", list(got.times) == list(expected.times))
check("matches on units", got.units == expected.units, repr(expected.units))
check("server reports the extension it used", got.actual_extension == ".MAG",
      repr(got.actual_extension))

low = remote.fetch("ip", 165920, params, ".MAG")
check("a lowercase pointname is canonicalised",
      list(low.data) == list(expected.data))

hint = remote.fetch("IP", 165920, params, ".NBI")
check("a wrong ext hint still resolves", list(hint.data) == list(expected.data),
      f"server used {hint.actual_extension!r}")

try:
    remote.fetch("NO_SUCH_POINT_XYZ", 165920, params, ".MAG")
    check("a missing point raises ShotNotFound", False, "no exception")
except ptdata._core.PtDataError as exc:
    check("a missing point raises ShotNotFound",
          code_of(exc) == ptdata.ErrorCode.ShotNotFound, str(code_of(exc)))

dead = ptdata.PtDataReader(sys_d3_paths=[], http_endpoint="http://127.0.0.1:9")
try:
    dead.fetch("IP", 165920, params, ".MAG")
    check("an unreachable endpoint is not a miss", False, "no exception")
except ptdata._core.PtDataError as exc:
    check("an unreachable endpoint is not a miss",
          code_of(exc) != ptdata.ErrorCode.ShotNotFound
          and "not found" not in str(exc).lower(),
          str(code_of(exc)))

print()
if failures:
    print(f"{len(failures)} failed: {', '.join(failures)}")
    sys.exit(1)
print("all contract checks passed against the real handler")
