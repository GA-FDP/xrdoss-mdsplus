#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "TdiPath.hh"

static const char *kPrefix = "/tdi";

// The payload is opaque to us -- it is whatever GetMany.serialize() produced.
// These tests only care that arbitrary bytes survive the path round trip.
static std::string Payload(const std::string &s) { return s; }

TEST_CASE("round-trips a single expression through a path") {
    const std::string path = fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload("\x01\x00\x01 ipmhd"));
    fdp::TdiTarget t;
    REQUIRE(fdp::ParseTdiPath(path, kPrefix, t));
    CHECK(t.tree == "efit01");
    CHECK(t.shot == 190000);
    CHECK(t.payload == Payload("\x01\x00\x01 ipmhd"));
}

TEST_CASE("round-trips a payload containing NULs and high bytes") {
    const std::string binary("\x00\x01\xff\x00\xfe", 5);
    const std::string path = fdp::BuildTdiPath(kPrefix, "efit01", 190000, binary);
    fdp::TdiTarget t;
    REQUIRE(fdp::ParseTdiPath(path, kPrefix, t));
    CHECK(t.payload == binary);
    CHECK(t.payload.size() == 5);
}

TEST_CASE("splits long requests across chunks, none exceeding the name limit") {
    const std::string path =
        fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload(std::string(400, 'x')));
    size_t start = 1;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        const size_t len = (end == std::string::npos ? path.size() : end) - start;
        CHECK(len <= fdp::kMaxSegment);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    fdp::TdiTarget t;
    REQUIRE(fdp::ParseTdiPath(path, kPrefix, t));
    CHECK(t.payload == std::string(400, 'x'));
}

TEST_CASE("rejects a request needing more than kMaxChunks segments") {
    const size_t too_big = fdp::kMaxChunks * fdp::kMaxSegment;  // comfortably over
    const std::string path =
        fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload(std::string(too_big, 'x')));
    fdp::TdiTarget t;
    CHECK_FALSE(fdp::ParseTdiPath(path, kPrefix, t));
}

TEST_CASE("is deterministic — equal requests give equal paths") {
    CHECK(fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload("\x01\x00\x01 ipmhd")) ==
          fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload("\x01\x00\x01 ipmhd")));
}

TEST_CASE("paths contain no character the director would mangle") {
    // Verified in tests/fed/FINDINGS.md: '/' inside a segment collapses under
    // the director's path.Clean, and %2F cannot protect it.
    const std::string path =
        fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload(std::string("\x00\xff/+=", 5)));
    size_t start = 1;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        const std::string seg =
            path.substr(start, (end == std::string::npos ? path.size() : end) - start);
        CHECK(seg.find('%') == std::string::npos);
        CHECK(seg.find('+') == std::string::npos);
        CHECK(seg.find('=') == std::string::npos);
        CHECK(seg != ".");
        CHECK(seg != "..");
        CHECK_FALSE(seg.empty());
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

TEST_CASE("the no-tree sentinel round-trips to an empty tree name") {
    const std::string path = fdp::BuildTdiPath(kPrefix, "", 0, Payload("[1.0,2.0,3.0]"));
    CHECK(path.find(std::string("/") + fdp::kNoTree + "/") != std::string::npos);

    fdp::TdiTarget t;
    REQUIRE(fdp::ParseTdiPath(path, kPrefix, t));
    CHECK(t.tree.empty());
    CHECK(t.shot == 0);
    CHECK(t.payload == "[1.0,2.0,3.0]");
}

TEST_CASE("a real tree name is not confused with the sentinel") {
    fdp::TdiTarget t;
    REQUIRE(fdp::ParseTdiPath(
        fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload("\x01\x00\x01 ipmhd")), kPrefix, t));
    CHECK(t.tree == "efit01");
}

TEST_CASE("builds the documented bucket layout") {
    CHECK(fdp::ShotBucket(190000) == "00/00/19/00");
    CHECK(fdp::ShotBucket(121844) == "00/00/12/18");
    CHECK(fdp::ShotBucket(0)      == "00/00/00/00");
}

TEST_CASE("rejects a bucket that disagrees with the shot") {
    fdp::TdiTarget t;
    std::string wrong = fdp::BuildTdiPath(kPrefix, "efit01", 190000, Payload("\x01\x00\x01 ipmhd"));
    const size_t p = wrong.find("/00/00/19/00/");
    REQUIRE(p != std::string::npos);
    wrong.replace(p, 13, "/00/00/19/01/");
    CHECK_FALSE(fdp::ParseTdiPath(wrong, kPrefix, t));
}

TEST_CASE("rejects malformed paths") {
    fdp::TdiTarget t;
    CHECK_FALSE(fdp::ParseTdiPath("/tdi/efit01/00/00/19/00/190000", kPrefix, t));   // no payload
    CHECK_FALSE(fdp::ParseTdiPath("/tdi/efit01/00/00/19/00/XYZ", kPrefix, t));      // too few parts
    CHECK_FALSE(fdp::ParseTdiPath("/tdi/efit01/00/00/19/00/abc/XYZ", kPrefix, t));  // bad shot
    CHECK_FALSE(fdp::ParseTdiPath("/tdi/efit01/00/00/19/00/190000/!!!", kPrefix, t)); // bad base64
    // NB: a payload that decodes but is not a valid MDSplus request is NOT
    // rejected here -- mdsip judges that, and keeping a second opinion about
    // its format is exactly the duplication this design removed.
    CHECK(fdp::ParseTdiPath("/tdi/efit01/00/00/19/00/190000/QUJD", kPrefix, t));
    CHECK_FALSE(fdp::ParseTdiPath("/tdi//00/00/19/00/190000/AAAA", kPrefix, t));    // empty tree
}

TEST_CASE("declines paths outside the prefix") {
    fdp::TdiTarget t;
    CHECK_FALSE(fdp::IsTdiPath("/archives/mdsplus/codes/efit01/x.tree", kPrefix));
    CHECK_FALSE(fdp::ParseTdiPath("/archives/mdsplus/codes/efit01/x.tree", kPrefix, t));
    CHECK_FALSE(fdp::IsTdiPath("/tdifoo/bar", kPrefix));   // prefix must end at a separator
    CHECK(fdp::IsTdiPath("/tdi/efit01/00/00/19/00/190000/AAAA", kPrefix));
}
