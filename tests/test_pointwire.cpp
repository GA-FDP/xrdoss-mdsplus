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

// --- the regression that shipped -------------------------------------------
//
// Every test above feeds ParseQuery a string containing '?'. XRootD never
// delivers that shape to an ext handler: XrdHttpExtReq::resource is stripped
// of the query. So the parser was correct, thoroughly tested, and fed the
// wrong string -- and 7.26.0-fdp2.6.0 served the latest version, with a 200,
// for every pinned request.

TEST_CASE("a RAW query parses -- the shape XRootD actually delivers") {
    const auto q = ParseQueryString("version=99&ext=.PLA");
    CHECK(q.at("version") == "99");
    CHECK(q.at("ext") == ".PLA");
    CHECK(ParseQueryString("").empty());
    CHECK(ParseQueryString("snapshot=catalog_x").at("snapshot") == "catalog_x");
}

TEST_CASE("xrd-http-query wins over xrd-http-fullresource") {
    // The two headers must DISAGREE here. An earlier version of this test set
    // both to the same value, so dropping the preferred lookup entirely still
    // passed -- it discriminated nothing.
    std::map<std::string, std::string> h;
    h["xrd-http-query"] = "version=7";
    h["xrd-http-fullresource"] = "/165920/IP?version=999";
    CHECK(QueryFromHeaders(h) == "version=7");
    CHECK(ParseQueryString(QueryFromHeaders(h)).at("version") == "7");
}

TEST_CASE("xrd-http-fullresource is the fallback when the query header is empty") {
    std::map<std::string, std::string> h;
    h["xrd-http-query"] = "";
    h["xrd-http-fullresource"] = "/165920/IP?version=3&snapshot=cat_a";
    const auto q = ParseQueryString(QueryFromHeaders(h));
    CHECK(q.at("version") == "3");
    CHECK(q.at("snapshot") == "cat_a");
}

TEST_CASE("no query anywhere is an unpinned read, not an error") {
    std::map<std::string, std::string> none;
    CHECK(QueryFromHeaders(none).empty());

    std::map<std::string, std::string> pathonly;
    pathonly["xrd-http-fullresource"] = "/165920/IP";   // no '?'
    CHECK(QueryFromHeaders(pathonly).empty());
    CHECK(ParseQueryString(QueryFromHeaders(pathonly)).empty());
}

TEST_CASE("reading the query off a stripped resource yields nothing") {
    // The precise defect: this is what the handler used to do, and it is why
    // a pin could never fail -- it was never seen.
    CHECK(ParseQuery("/165920/IP").empty());
}

