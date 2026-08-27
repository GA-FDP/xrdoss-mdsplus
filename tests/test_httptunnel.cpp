#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "HttpTunnel.hh"
#include "MdsipCall.hh"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// setenv/unsetenv around a test, so one case cannot leak into the next.
struct ScopedEnv {
    std::string name;
    bool had;
    std::string old;

    ScopedEnv(const char *n, const char *v) : name(n), had(false) {
        const char *cur = std::getenv(n);
        if (cur) { had = true; old = cur; }
        if (v) ::setenv(n, v, 1); else ::unsetenv(n);
    }
    ~ScopedEnv() {
        if (had) ::setenv(name.c_str(), old.c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

}  // namespace

TEST_CASE("a bare host gets the default prefix and https") {
    ScopedEnv scheme("FDP_TUNNEL_SCHEME", 0);
    CHECK(fdp::BuildUrlBase("origin.example.org") ==
          "https://origin.example.org/mdsip");
}

TEST_CASE("an explicit port is preserved") {
    ScopedEnv scheme("FDP_TUNNEL_SCHEME", 0);
    CHECK(fdp::BuildUrlBase("origin.example.org:8443") ==
          "https://origin.example.org:8443/mdsip");
}

TEST_CASE("an explicit prefix replaces the default") {
    ScopedEnv scheme("FDP_TUNNEL_SCHEME", 0);
    CHECK(fdp::BuildUrlBase("origin.example.org:8443/mdsip") ==
          "https://origin.example.org:8443/mdsip");
    // A federation-style path, where the relay sits under a namespace prefix.
    CHECK(fdp::BuildUrlBase("origin.example.org/fdp-d3d/mdsip") ==
          "https://origin.example.org/fdp-d3d/mdsip");
}

TEST_CASE("a trailing slash does not become a double slash") {
    ScopedEnv scheme("FDP_TUNNEL_SCHEME", 0);
    // "…/mdsip//connect" would 404 against the handler's exact-suffix match.
    CHECK(fdp::BuildUrlBase("origin.example.org/mdsip/") ==
          "https://origin.example.org/mdsip");
}

TEST_CASE("the scheme override is honoured") {
    // Test-only: it exists so a local relay without TLS can be exercised.
    // Production must stay https, which is why that is the default rather
    // than something the target string can quietly downgrade.
    ScopedEnv scheme("FDP_TUNNEL_SCHEME", "http");
    CHECK(fdp::BuildUrlBase("127.0.0.1:10951/mdsip") ==
          "http://127.0.0.1:10951/mdsip");
}

TEST_CASE("an empty target is rejected rather than guessed at") {
    CHECK(fdp::BuildUrlBase("").empty());
    CHECK(fdp::BuildUrlBase("/mdsip").empty());
}

TEST_CASE("BEARER_TOKEN wins over the token file") {
    ScopedEnv tok("BEARER_TOKEN", "  from-env\n");
    CHECK(fdp::FindBearerToken() == "from-env");
}

TEST_CASE("a missing token is empty rather than an error") {
    // No Authorization header is then sent, which is right for an
    // unauthenticated local relay and yields a clean 401 against a real origin.
    ScopedEnv tok("BEARER_TOKEN", 0);
    ScopedEnv home("HOME", "/nonexistent-fdp-test-home");
    CHECK(fdp::FindBearerToken().empty());
}

TEST_CASE("the token file is read and trimmed") {
    char dir[] = "/tmp/fdp-tok-XXXXXX";
    const char *made = ::mkdtemp(dir);
    REQUIRE(made != 0);
    const std::string fdpdir = std::string(dir) + "/.fdp";
    ::mkdir(fdpdir.c_str(), 0700);
    const std::string path = fdpdir + "/token";
    FILE *f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != 0);
    std::fputs("token-from-file\n", f);
    std::fclose(f);

    ScopedEnv tok("BEARER_TOKEN", 0);
    ScopedEnv home("HOME", dir);
    CHECK(fdp::FindBearerToken() == "token-from-file");

    ::unlink(path.c_str());
    ::rmdir(fdpdir.c_str());
    ::rmdir(dir);
}

TEST_CASE("calling before opening fails instead of crashing") {
    fdp::HttpTunnel t;
    CHECK_FALSE(t.IsOpen());
    std::string answer, error;
    CHECK_FALSE(t.Call("anything", answer, error));
    CHECK(!error.empty());
}

TEST_CASE("opening an unparseable target fails cleanly") {
    fdp::HttpTunnel t;
    std::string error;
    CHECK_FALSE(t.Open("", error));
    CHECK(!error.empty());
    CHECK_FALSE(t.IsOpen());
}

// ---------------------------------------------------------------------------
// Reading a call back out of the transport's own byte stream.
//
// The transport buffers a whole mdsip call before POSTing it, so on a redial
// it already holds the bytes it would need to replay. Whether replaying is
// SAFE depends on what the call was, and the only thing that says so is
// descriptor 0's expression text.

namespace {

// One mdsip call: descriptor 0 carrying `expr` as DTYPE_T, plus `extra_args`
// opaque argument messages after it. Mirrors what MDSplus._SendArg writes.
std::string MakeCall(const std::string &expr, int extra_args = 0,
                     unsigned char dtype = fdp::kDtypeCString) {
    const int nargs = extra_args + 1;
    std::string out;

    std::string hdr(fdp::kHeaderBytes, '\0');
    const unsigned int msglen =
        static_cast<unsigned int>(fdp::kHeaderBytes + expr.size());
    std::memcpy(&hdr[fdp::kMsgLenOffset], &msglen, sizeof(msglen));
    const short len = static_cast<short>(expr.size());
    std::memcpy(&hdr[fdp::kLengthOffset], &len, sizeof(len));
    hdr[fdp::kNargsOffset] = static_cast<char>(nargs);
    hdr[fdp::kDescriptorIdxOffset] = 0;
    hdr[fdp::kDtypeOffset] = static_cast<char>(dtype);
    out += hdr;
    out += expr;

    for (int i = 1; i <= extra_args; ++i) {
        std::string a(fdp::kHeaderBytes, '\0');
        const unsigned int alen = static_cast<unsigned int>(fdp::kHeaderBytes);
        std::memcpy(&a[fdp::kMsgLenOffset], &alen, sizeof(alen));
        a[fdp::kNargsOffset] = static_cast<char>(nargs);
        a[fdp::kDescriptorIdxOffset] = static_cast<char>(i);
        out += a;
    }
    return out;
}

}  // namespace

TEST_CASE("a call's expression is read back from descriptor 0") {
    CHECK(fdp::CallExpression(MakeCall("TreeOpen($,$)", 2)) == "TreeOpen($,$)");
    CHECK(fdp::CallExpression(MakeCall("GetManyExecute($)", 1)) ==
          "GetManyExecute($)");
    // A zero-argument call is still one message with one descriptor.
    CHECK(fdp::CallExpression(MakeCall("1+1")) == "1+1");
}

TEST_CASE("a call whose lead descriptor is not a string yields nothing") {
    // Assuming an expression that isn't there would classify random binary as
    // harmless. Empty means "unreadable", and the caller must not redial.
    const std::string binary = MakeCall("\x01\x02\x03", 0, /*dtype=*/8);
    CHECK(fdp::CallExpression(binary).empty());
    CHECK(fdp::EffectOf(fdp::CallExpression(binary)) == fdp::kUnknownEffect);
}

TEST_CASE("truncated and malformed calls yield nothing") {
    CHECK(fdp::CallExpression("").empty());
    CHECK(fdp::CallExpression(std::string(10, '\0')).empty());
    // A header claiming more payload than is present.
    std::string short_payload = MakeCall("TreeOpen($,$)", 0);
    short_payload.resize(fdp::kHeaderBytes + 3);
    CHECK(fdp::CallExpression(short_payload).empty());
}

TEST_CASE("tree-context calls are recognised whatever their case") {
    CHECK(fdp::EffectOf("TreeOpen($,$)") == fdp::kOpensTree);
    CHECK(fdp::EffectOf("treeopen($,$)") == fdp::kOpensTree);
    CHECK(fdp::EffectOf("  TreeOpen($,$)  ") == fdp::kOpensTree);
    CHECK(fdp::EffectOf("TreeSetDefault($)") == fdp::kOpensTree);
    CHECK(fdp::EffectOf("TreeOpenNew($,$)") == fdp::kOpensTree);
    CHECK(fdp::EffectOf("TreeClose($,$)") == fdp::kClosesTree);
}

TEST_CASE("ordinary reads leave nothing behind to restore") {
    CHECK(fdp::EffectOf("GetManyExecute($)") == fdp::kNoEffect);
    CHECK(fdp::EffectOf("\\IPMHD") == fdp::kNoEffect);
    CHECK(fdp::EffectOf("dim_of(ptdata2(\"ip\", 203321), 0)") == fdp::kNoEffect);
    CHECK(fdp::EffectOf("TreePut($,$,$)") == fdp::kNoEffect);
}

TEST_CASE("an assignment makes a session unreconstructible") {
    // A fresh session has no _sig, so a silent redial would answer a later
    // get("_sig") with something else entirely. Refusing to redial keeps the
    // failure visible instead of turning it into a wrong number.
    CHECK(fdp::EffectOf("_sig = ptdata2(\"ip\", 203321)") == fdp::kUnknownEffect);
    CHECK(fdp::EffectOf("_x=1") == fdp::kUnknownEffect);
    CHECK(fdp::EffectOf("") == fdp::kUnknownEffect);
}

TEST_CASE("comparisons are not assignments") {
    // Rejecting these too would be safe but would switch recovery off for
    // perfectly ordinary expressions.
    CHECK(fdp::EffectOf("a == b") == fdp::kNoEffect);
    CHECK(fdp::EffectOf("a != b") == fdp::kNoEffect);
    CHECK(fdp::EffectOf("a <= b") == fdp::kNoEffect);
    CHECK(fdp::EffectOf("a >= b") == fdp::kNoEffect);
}

TEST_CASE("a fresh tunnel is recoverable and has not redialled") {
    fdp::HttpTunnel t;
    CHECK(t.Recoverable());
    CHECK(t.Redials() == 0);
}
