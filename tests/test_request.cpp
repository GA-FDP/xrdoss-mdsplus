#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "Request.hh"

using fdp::Request;
using fdp::RequestItem;

static Request OneExpr(const std::string &name, const std::string &exp) {
    Request r;
    r.items.push_back(RequestItem{name, exp, {}});
    return r;
}

TEST_CASE("round-trips a single argument-free expression") {
    const Request in = OneExpr("r0", "\\ipmhd");
    Request out;
    REQUIRE(Request::Parse(in.Serialize(), out));
    REQUIRE(out.items.size() == 1);
    CHECK(out.items[0].name == "r0");
    CHECK(out.items[0].exp == "\\ipmhd");
    CHECK(out.items[0].args.empty());
}

TEST_CASE("round-trips binary arguments containing NULs") {
    Request in;
    const std::string arg1("\x00\x01\x02\xff", 4);
    const std::string arg2(300, '\x00');
    in.items.push_back(RequestItem{"r0", "$ + $", {arg1, arg2}});
    Request out;
    REQUIRE(Request::Parse(in.Serialize(), out));
    REQUIRE(out.items[0].args.size() == 2);
    CHECK(out.items[0].args[0] == arg1);
    CHECK(out.items[0].args[1] == arg2);
}

TEST_CASE("round-trips a multi-item batch and preserves order") {
    Request in;
    in.items.push_back(RequestItem{"ip",    "\\ipmhd", {}});
    in.items.push_back(RequestItem{"q95",   "\\q95",   {}});
    in.items.push_back(RequestItem{"betan", "\\betan", {}});
    Request out;
    REQUIRE(Request::Parse(in.Serialize(), out));
    REQUIRE(out.items.size() == 3);
    CHECK(out.items[0].name == "ip");
    CHECK(out.items[1].name == "q95");
    CHECK(out.items[2].name == "betan");
}

TEST_CASE("is deterministic — equal requests give equal bytes") {
    CHECK(OneExpr("r0", "\\ipmhd").Serialize() == OneExpr("r0", "\\ipmhd").Serialize());
}

TEST_CASE("distinguishes things that change the result") {
    const std::string base = OneExpr("r0", "\\ipmhd").Serialize();
    CHECK(OneExpr("r1", "\\ipmhd").Serialize() != base);   // name is in the response dict
    CHECK(OneExpr("r0", "\\q95").Serialize()   != base);   // different expression

    Request reordered;                                      // order is observable
    reordered.items.push_back(RequestItem{"b", "2", {}});
    reordered.items.push_back(RequestItem{"a", "1", {}});
    Request forward;
    forward.items.push_back(RequestItem{"a", "1", {}});
    forward.items.push_back(RequestItem{"b", "2", {}});
    CHECK(reordered.Serialize() != forward.Serialize());

    Request with_arg;                                       // args are in the key
    with_arg.items.push_back(RequestItem{"r0", "\\ipmhd", {std::string("x")}});
    CHECK(with_arg.Serialize() != base);
}

TEST_CASE("an empty name is legal and round-trips") {
    const Request in = OneExpr("", "\\ipmhd");
    Request out;
    REQUIRE(Request::Parse(in.Serialize(), out));
    CHECK(out.items[0].name.empty());
}

TEST_CASE("handles a large batch") {
    Request in;
    for (int i = 0; i < 500; ++i)
        in.items.push_back(RequestItem{"n" + std::to_string(i), "\\sig" + std::to_string(i), {}});
    Request out;
    REQUIRE(Request::Parse(in.Serialize(), out));
    REQUIRE(out.items.size() == 500);
    CHECK(out.items[499].exp == "\\sig499");
}

TEST_CASE("rejects malformed input") {
    Request out;
    CHECK_FALSE(Request::Parse("", out));                              // empty
    CHECK_FALSE(Request::Parse(std::string("\x02\x00\x01", 3), out));  // bad version
    CHECK_FALSE(Request::Parse(std::string("\x01\x00", 2), out));      // truncated count

    std::string truncated = OneExpr("r0", "\\ipmhd").Serialize();
    truncated.resize(truncated.size() - 1);
    CHECK_FALSE(Request::Parse(truncated, out));                       // truncated payload

    const std::string trailing = OneExpr("r0", "\\ipmhd").Serialize() + "junk";
    CHECK_FALSE(Request::Parse(trailing, out));                        // trailing garbage
}

TEST_CASE("rejects an empty request") {
    const Request empty;
    Request out;
    CHECK_FALSE(Request::Parse(empty.Serialize(), out));
}

TEST_CASE("rejects an empty expression") {
    const Request in = OneExpr("r0", "");
    Request out;
    CHECK_FALSE(Request::Parse(in.Serialize(), out));
}

TEST_CASE("leaves the output empty when parsing fails") {
    Request out;
    REQUIRE(Request::Parse(OneExpr("r0", "\\ipmhd").Serialize(), out));
    REQUIRE(out.items.size() == 1);
    CHECK_FALSE(Request::Parse("garbage", out));
    CHECK(out.items.empty());
}
