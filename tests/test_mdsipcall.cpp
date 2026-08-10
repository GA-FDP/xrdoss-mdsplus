#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "MdsipCall.hh"

#include <cstring>
#include <string>
#include <vector>

namespace {

// One mdsip message: a 48-byte header carrying its own total length, plus
// payload. nargs/descriptor_idx are what mark the end of a call.
std::string Msg(unsigned char nargs, unsigned char idx, const std::string &body) {
    std::string m(fdp::kHeaderBytes, '\0');
    const unsigned int len =
        static_cast<unsigned int>(fdp::kHeaderBytes + body.size());
    std::memcpy(&m[fdp::kMsgLenOffset], &len, sizeof(len));
    m[fdp::kNargsOffset] = static_cast<char>(nargs);
    m[fdp::kDescriptorIdxOffset] = static_cast<char>(idx);
    return m + body;
}

// Feeds a whole stream one byte at a time -- the case most likely to expose an
// off-by-one, and a real one: send() gets whatever chunking MDSplus chose.
fdp::CallAssembler::Status FeedByByte(fdp::CallAssembler &a,
                                      const std::string &s) {
    fdp::CallAssembler::Status st = fdp::CallAssembler::kNeedMore;
    for (size_t i = 0; i < s.size(); ++i) st = a.Append(s.data() + i, 1);
    return st;
}

}  // namespace

TEST_CASE("a one-argument call completes on its only message") {
    fdp::CallAssembler a;
    const std::string call = Msg(1, 0, "payload");
    CHECK(a.Append(call.data(), call.size()) == fdp::CallAssembler::kComplete);
    CHECK(a.Take() == call);
}

TEST_CASE("a multi-argument call completes only on the last message") {
    fdp::CallAssembler a;
    const std::string m0 = Msg(3, 0, "aa");
    const std::string m1 = Msg(3, 1, "bb");
    const std::string m2 = Msg(3, 2, "cc");

    CHECK(a.Append(m0.data(), m0.size()) == fdp::CallAssembler::kNeedMore);
    CHECK(a.Append(m1.data(), m1.size()) == fdp::CallAssembler::kNeedMore);
    CHECK(a.Append(m2.data(), m2.size()) == fdp::CallAssembler::kComplete);
    CHECK(a.Take() == m0 + m1 + m2);
}

TEST_CASE("chunk boundaries do not matter") {
    const std::string call = Msg(2, 0, "first") + Msg(2, 1, "second");

    SUBCASE("one byte at a time") {
        fdp::CallAssembler a;
        CHECK(FeedByByte(a, call) == fdp::CallAssembler::kComplete);
        CHECK(a.Take() == call);
    }

    SUBCASE("split mid-header") {
        fdp::CallAssembler a;
        CHECK(a.Append(call.data(), 20) == fdp::CallAssembler::kNeedMore);
        CHECK(a.Append(call.data() + 20, call.size() - 20) ==
              fdp::CallAssembler::kComplete);
        CHECK(a.Take() == call);
    }

    SUBCASE("split mid-payload") {
        fdp::CallAssembler a;
        const size_t cut = fdp::kHeaderBytes + 2;
        CHECK(a.Append(call.data(), cut) == fdp::CallAssembler::kNeedMore);
        CHECK(a.Append(call.data() + cut, call.size() - cut) ==
              fdp::CallAssembler::kComplete);
        CHECK(a.Take() == call);
    }

    SUBCASE("all at once") {
        fdp::CallAssembler a;
        CHECK(a.Append(call.data(), call.size()) ==
              fdp::CallAssembler::kComplete);
        CHECK(a.Take() == call);
    }
}

TEST_CASE("a header alone is not a complete message") {
    fdp::CallAssembler a;
    const std::string call = Msg(1, 0, "body");
    CHECK(a.Append(call.data(), fdp::kHeaderBytes) ==
          fdp::CallAssembler::kNeedMore);
    CHECK(a.Append(call.data() + fdp::kHeaderBytes,
                   call.size() - fdp::kHeaderBytes) ==
          fdp::CallAssembler::kComplete);
}

TEST_CASE("an empty-payload message is complete at exactly 48 bytes") {
    fdp::CallAssembler a;
    const std::string call = Msg(1, 0, "");
    REQUIRE(call.size() == fdp::kHeaderBytes);
    CHECK(a.Append(call.data(), call.size()) == fdp::CallAssembler::kComplete);
    CHECK(a.Take() == call);
}

TEST_CASE("a zero-argument call does not wait forever") {
    // nargs is unsigned char, so a naive `idx >= nargs - 1` wraps to 255 here
    // and the call never completes -- the transport would hang rather than
    // fail, which is the worst way to be wrong.
    fdp::CallAssembler a;
    const std::string call = Msg(0, 0, "");
    CHECK(a.Append(call.data(), call.size()) == fdp::CallAssembler::kComplete);
}

TEST_CASE("successive calls are framed independently") {
    fdp::CallAssembler a;
    const std::string first = Msg(1, 0, "one");
    const std::string second = Msg(2, 0, "two") + Msg(2, 1, "three");

    REQUIRE(a.Append(first.data(), first.size()) ==
            fdp::CallAssembler::kComplete);
    CHECK(a.Take() == first);

    REQUIRE(a.Append(second.data(), second.size()) ==
            fdp::CallAssembler::kComplete);
    CHECK(a.Take() == second);
}

TEST_CASE("bytes arriving after a complete call are kept for the next one") {
    // Not something MDSplus does today, but dropping them would truncate
    // silently rather than fail visibly.
    fdp::CallAssembler a;
    const std::string first = Msg(1, 0, "one");
    const std::string second = Msg(1, 0, "two");

    REQUIRE(a.Append((first + second).data(), first.size() + second.size()) ==
            fdp::CallAssembler::kComplete);
    CHECK(a.Take() == first);

    // The second call was already buffered, so it is complete with no new input.
    CHECK(a.Append(0, 0) == fdp::CallAssembler::kComplete);
    CHECK(a.Take() == second);
}

TEST_CASE("a message shorter than its own header is rejected") {
    fdp::CallAssembler a;
    std::string bad(fdp::kHeaderBytes, '\0');
    const unsigned int tiny = 10;
    std::memcpy(&bad[fdp::kMsgLenOffset], &tiny, sizeof(tiny));

    CHECK(a.Append(bad.data(), bad.size()) == fdp::CallAssembler::kMalformed);
    // Malformed is sticky: the stream cannot be resynchronised, and pretending
    // otherwise would frame the next call from the wrong offset.
    CHECK(a.Append(bad.data(), bad.size()) == fdp::CallAssembler::kMalformed);
}

TEST_CASE("a large call is assembled correctly from small chunks") {
    // 4 MB, the size of a real \psirz answer, fed in 8 KB pieces.
    const std::string body(4 * 1024 * 1024, 'x');
    const std::string call = Msg(2, 0, "hdr") + Msg(2, 1, body);

    fdp::CallAssembler a;
    fdp::CallAssembler::Status st = fdp::CallAssembler::kNeedMore;
    for (size_t off = 0; off < call.size(); off += 8192) {
        const size_t n = std::min<size_t>(8192, call.size() - off);
        st = a.Append(call.data() + off, n);
    }
    CHECK(st == fdp::CallAssembler::kComplete);
    CHECK(a.Take() == call);
}

TEST_CASE("Reset abandons a partial call") {
    fdp::CallAssembler a;
    const std::string partial = Msg(2, 0, "only the first");
    REQUIRE(a.Append(partial.data(), partial.size()) ==
            fdp::CallAssembler::kNeedMore);
    CHECK(a.Buffered() > 0);

    a.Reset();
    CHECK(a.Buffered() == 0);

    const std::string call = Msg(1, 0, "fresh");
    CHECK(a.Append(call.data(), call.size()) == fdp::CallAssembler::kComplete);
    CHECK(a.Take() == call);
}
