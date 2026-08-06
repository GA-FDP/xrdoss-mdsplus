#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "Base64Url.hh"

TEST_CASE("encodes the ipmhd test vector") {
    CHECK(fdp::Base64UrlEncode("\\ipmhd") == "XGlwbWhk");
}

TEST_CASE("round-trips arbitrary bytes") {
    const std::string in = "\\top.results.aeqdsk:q95 * 2.0 + dim_of(\\ipmhd)";
    std::string out;
    REQUIRE(fdp::Base64UrlDecode(fdp::Base64UrlEncode(in), out));
    CHECK(out == in);
}

TEST_CASE("emits no padding and no unsafe characters") {
    for (size_t n = 0; n < 40; ++n) {
        const std::string enc = fdp::Base64UrlEncode(std::string(n, '\xfb'));
        CHECK(enc.find('=') == std::string::npos);
        CHECK(enc.find('+') == std::string::npos);
        CHECK(enc.find('/') == std::string::npos);
    }
}

TEST_CASE("handles every remainder length") {
    for (size_t n = 0; n < 8; ++n) {
        const std::string in(n, 'x');
        std::string out;
        REQUIRE(fdp::Base64UrlDecode(fdp::Base64UrlEncode(in), out));
        CHECK(out == in);
    }
}

TEST_CASE("round-trips all 256 byte values") {
    std::string in;
    for (int i = 0; i < 256; ++i) in += static_cast<char>(i);
    std::string out;
    REQUIRE(fdp::Base64UrlDecode(fdp::Base64UrlEncode(in), out));
    CHECK(out == in);
}

TEST_CASE("rejects invalid input") {
    std::string out;
    CHECK_FALSE(fdp::Base64UrlDecode("abc/def", out));   // standard-b64 char
    CHECK_FALSE(fdp::Base64UrlDecode("ab=cd", out));     // padding
    CHECK_FALSE(fdp::Base64UrlDecode("A", out));         // impossible length
    CHECK_FALSE(fdp::Base64UrlDecode("ab cd", out));     // whitespace
    CHECK_FALSE(fdp::Base64UrlDecode("ab+cd", out));     // standard-b64 char
}
