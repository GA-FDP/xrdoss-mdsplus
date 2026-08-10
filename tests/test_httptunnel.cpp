#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "HttpTunnel.hh"

#include <cstdio>
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
