// tests/test_pointwire.cpp
//
// The wire contract's pure parts. These exist because HttpRelay.cc is
// compiled ONLY into the XRootD plugin module, which no test target links --
// so a 409-to-404 regression was proved to break nothing at all. That is the
// cross-repo contract: the client treats a 404 from this tier as an
// authoritative miss and stops, so answering 404 for a stale pin would make
// "your recorded version is gone" indistinguishable from "this point never
// existed".
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "PointWire.hh"

using namespace fdp;

TEST_CASE("a pin that cannot be honoured is 409, never 404") {
    // THE cross-repo contract. If this ever reads 404, a client replaying a
    // CMF-recorded run is told its data never existed.
    CHECK(StatusForRecord(/*found=*/false, /*pin_failed=*/true) == 409);
}

TEST_CASE("ordinary absence is still 404") {
    CHECK(StatusForRecord(false, false) == 404);
}

TEST_CASE("a served record is 200") {
    CHECK(StatusForRecord(true, false) == 200);
}

TEST_CASE("ParseQuery handles the shapes this endpoint actually receives") {
    CHECK(ParseQuery("/165920/IP").empty());               // no query at all
    CHECK(ParseQuery("/165920/IP?").empty());              // bare ?

    auto ext = ParseQuery("/165920/IP?ext=.MAG");
    CHECK(ext["ext"] == ".MAG");

    auto both = ParseQuery("/165920/IP?ext=.MAG&version=2");
    CHECK(both["ext"] == ".MAG");
    CHECK(both["version"] == "2");

    auto snap = ParseQuery("/165920/IP?snapshot=catalog_20260907T232802Z");
    CHECK(snap["snapshot"] == "catalog_20260907T232802Z");
}

TEST_CASE("ParseQuery survives malformed input rather than misreading it") {
    CHECK(ParseQuery("/x?&&&").empty());                   // separators only
    CHECK(ParseQuery("/x?novalue").empty());               // key with no '='
    CHECK(ParseQuery("/x?=orphan").empty());               // '=' with no key

    auto trailing = ParseQuery("/x?version=2&");           // trailing '&'
    CHECK(trailing["version"] == "2");

    auto empty_val = ParseQuery("/x?version=");            // '=' with no value
    CHECK(empty_val["version"].empty());

    // Last one wins. Not a contract anyone should rely on, but pinned so a
    // change is deliberate.
    auto dup = ParseQuery("/x?version=1&version=2");
    CHECK(dup["version"] == "2");
}

TEST_CASE("ParseQuery decodes a value the way the path parser does") {
    auto q = ParseQuery("/x?snapshot=catalog_2026%2D09%2D07");
    CHECK(q["snapshot"] == "catalog_2026-09-07");
}
