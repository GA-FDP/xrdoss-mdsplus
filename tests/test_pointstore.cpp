#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "PointStore.hh"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// The same shot cache ptdata's own tests use.
std::string ShotsDir() {
    const char *d = std::getenv("PTDATA_TEST_SHOTS_DIR");
    return d ? d : "/cscratch/sammuli/ptdata_test_files";
}

bool HaveShot() {
    struct ::stat st;
    return ::stat((ShotsDir() + "/165920.MAG").c_str(), &st) == 0;
}

const char *kPrefix = "pelican://osg-htc.org:443/fdp-d3d/archives";

// Write <root>/1659/165920.json recording an absolute pelican:// URL, exactly
// as the production indexer does -- the point being that the rewrite, not a
// convenient local path, is what makes this resolve.
std::string BuildIndex(const std::string &root) {
    ::mkdir(root.c_str(), 0755);
    const std::string sub = root + "/1659";
    ::mkdir(sub.c_str(), 0755);
    std::ofstream(sub + "/165920.json")
        << "{\"shot\":165920,"
        << "\"ext_location\":{\".MAG\":\"" << kPrefix << "/165920.MAG\"},"
        << "\"pointname_ext\":{\"IP\":\".MAG\"}}";
    return root;
}

std::string TempDir() {
    char tmpl[] = "/tmp/pointstore.XXXXXX";
    return ::mkdtemp(tmpl);
}

}  // namespace

TEST_CASE("resolves through the index and returns the record bytes") {
    if (!HaveShot()) {
        MESSAGE("shot cache absent; skipping");
        return;
    }
    const std::string idx = TempDir() + "/idx";
    BuildIndex(idx);

    fdp::PointStore store(idx, "", kPrefix, ShotsDir());
    const auto rec = store.Read(165920, "IP");

    REQUIRE(rec.found);
    CHECK(rec.bytes.size() > 0);
    CHECK(rec.extension == ".MAG");
}

TEST_CASE("the extension reported is the one the index chose") {
    if (!HaveShot()) return;
    const std::string idx = TempDir() + "/idx";
    BuildIndex(idx);

    // The client sends ?ext as a hint and the index ignores it -- resolve()
    // takes only (pointname, shot). X-Ptdata-Extension is how a client learns
    // what actually answered, so it must reflect the index, not the request.
    fdp::PointStore store(idx, "", kPrefix, ShotsDir());
    CHECK(store.Read(165920, "IP").extension == ".MAG");
}

TEST_CASE("a pointname absent from the index is a miss, not an error") {
    const std::string idx = TempDir() + "/idx";
    BuildIndex(idx);
    fdp::PointStore store(idx, "", kPrefix, ShotsDir());
    CHECK_FALSE(store.Read(165920, "NO_SUCH_POINT").found);
}

TEST_CASE("a shot absent from the index is a miss, not an error") {
    const std::string idx = TempDir() + "/idx";
    BuildIndex(idx);
    fdp::PointStore store(idx, "", kPrefix, ShotsDir());
    CHECK_FALSE(store.Read(999999, "IP").found);
}

TEST_CASE("an index entry pointing at a file that is not there is a miss") {
    // The index is a snapshot; the archive moves under it. That must read as
    // absent data, not as a server error, so the client's chain can advance.
    const std::string idx = TempDir() + "/idx";
    BuildIndex(idx);
    fdp::PointStore store(idx, "", kPrefix, "/nonexistent-root");
    CHECK_FALSE(store.Read(165920, "IP").found);
}

TEST_CASE("an empty pattern uses the index dir as given") {
    if (!HaveShot()) return;
    const std::string idx = TempDir() + "/idx";
    BuildIndex(idx);
    fdp::PointStore store(idx, "", kPrefix, ShotsDir());
    CHECK(store.Read(165920, "IP").found);
}

TEST_CASE("a pattern selects the newest snapshot beneath the parent") {
    if (!HaveShot()) return;
    const std::string parent = TempDir();
    ::mkdir((parent + "/json_indexes_2026-01-01").c_str(), 0755);
    BuildIndex(parent + "/json_indexes_2026-06-23");

    fdp::PointStore store(parent, "json_indexes_*", kPrefix, ShotsDir());
    CHECK(store.Read(165920, "IP").found);
}
