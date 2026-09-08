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

std::string TempDir() {
    char tmpl[] = "/tmp/pointstore.XXXXXX";
    return ::mkdtemp(tmpl);
}

void MkdirP(const std::string &p) {
    std::string acc;
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') {
            acc = p.substr(0, i);
            ::mkdir(acc.c_str(), 0755);
        }
}

const char *kStamp = "catalog_20260907T232802Z";

// The store layout the origin actually serves, verified on d3d-origin
// 2026-09-07:
//   <root>/catalog/catalog_<stamp>/latest/<bucket:04d>.json   {"<shot>": <ver>}
//   <root>/views/shots/<bucket:04d>/<shot:06d>/v<N>/meta/index.json
//   <root>/views/shots/<bucket:04d>/<shot:06d>/v<N>/ptdata/<shot><EXT>
// Note there is no ext_location and no recorded path anywhere: the shotfile
// path is CONSTRUCTED from the version directory (store spec §4.7b), which is
// why this fixture needs no URL prefix and no rewrite root.
void WriteCatalog(const std::string &root, int shot, int version) {
    const std::string d = root + "/catalog/" + kStamp + "/latest";
    MkdirP(d);
    std::ofstream(d + "/1659.json") << "{\"" << shot << "\": " << version << "}";
}

std::string VersionDir(const std::string &root, int version) {
    return root + "/views/shots/1659/165920/v" + std::to_string(version);
}

void WriteIndex(const std::string &root, int version) {
    const std::string d = VersionDir(root, version) + "/meta";
    MkdirP(d);
    std::ofstream(d + "/index.json")
        << "{\"shot\":165920,\"exts\":[\".MAG\"],"
        << "\"pointname_ext\":{\"IP\":\".MAG\"}}";
}

// Symlinked, not copied: 165920.MAG is 323 MB and the bytes must be real for
// the ShotFile parse to mean anything.
void LinkShotfile(const std::string &root, int version) {
    const std::string d = VersionDir(root, version) + "/ptdata";
    MkdirP(d);
    ::symlink((ShotsDir() + "/165920.MAG").c_str(),
              (d + "/165920.MAG").c_str());
}

std::string BuildStore(int version = 1) {
    const std::string root = TempDir();
    WriteCatalog(root, 165920, version);
    WriteIndex(root, version);
    LinkShotfile(root, version);
    return root;
}

}  // namespace

// Gated on the 323 MB shot cache. Declared with doctest::skip so an absent
// cache is reported as SKIPPED rather than passed: `if (!HaveShot()) return;`
// makes a vacuous case indistinguishable from a real one in a CI log, and CI
// runs `pixi run test` with no PTDATA_TEST_SHOTS_DIR. Measured 2026-09-08:
// without the cache these cases silently dropped 10 of 21 assertions while
// still reporting "9 passed | 0 skipped".
TEST_CASE("resolves through the store and returns the record bytes"
          * doctest::skip(!HaveShot())) {
    fdp::PointStore store(BuildStore(), "catalog_*");
    const auto rec = store.Read(165920, "IP");

    REQUIRE(rec.found);
    CHECK(rec.bytes.size() > 0);
    CHECK(rec.extension == ".MAG");
}

TEST_CASE("the record carries the version and snapshot it came from"
          * doctest::skip(!HaveShot())) {
    // These are what the endpoint returns as X-Ptdata-Version and
    // X-Ptdata-Snapshot, which is how a client detects a run that straddled a
    // snapshot swap. A resolution that cannot name its own provenance is
    // useless for that.
    fdp::PointStore store(BuildStore(3), "catalog_*");
    const auto rec = store.Read(165920, "IP");

    REQUIRE(rec.found);
    CHECK(rec.version == 3);
    CHECK(rec.snapshot == kStamp);
}

TEST_CASE("the extension reported is the one the index chose"
          * doctest::skip(!HaveShot())) {
    // The client sends ?ext as a hint and the store ignores it.
    // X-Ptdata-Extension is how a client learns what actually answered, so it
    // must reflect the index, not the request.
    fdp::PointStore store(BuildStore(), "catalog_*");
    CHECK(store.Read(165920, "IP").extension == ".MAG");
}

TEST_CASE("a pointname absent from the index is an ordinary miss") {
    fdp::PointStore store(BuildStore(), "catalog_*");
    const auto rec = store.Read(165920, "NO_SUCH_POINT");
    CHECK_FALSE(rec.found);
    CHECK_FALSE(rec.defect);      // nothing for the handler to log
}

TEST_CASE("a shot the catalog does not name is an ordinary miss") {
    fdp::PointStore store(BuildStore(), "catalog_*");
    const auto rec = store.Read(999999, "IP");
    CHECK_FALSE(rec.found);
    CHECK_FALSE(rec.defect);      // never minted is not a defect
}

TEST_CASE("a catalog row with no index is a DEFECT the caller must log") {
    // The catalog promised v4 and nothing materialised it. That violates the
    // store's own guarantee, so it must be distinguishable from absent data --
    // otherwise a broken publish reads to the client as "no such pointname".
    const std::string root = TempDir();
    WriteCatalog(root, 165920, 4);

    fdp::PointStore store(root, "catalog_*");
    const auto rec = store.Read(165920, "IP");

    CHECK_FALSE(rec.found);
    CHECK(rec.defect);
    CHECK(rec.detail.find("/v4/meta/index.json") != std::string::npos);
    CHECK(rec.detail.find(kStamp) != std::string::npos);
}

TEST_CASE("an index naming a shotfile that is not there is a miss") {
    // The snapshot is immutable but the tree can still be incomplete. Absent
    // data, not a server error, so the client's provider chain can advance.
    const std::string root = TempDir();
    WriteCatalog(root, 165920, 1);
    WriteIndex(root, 1);          // no shotfile linked
    fdp::PointStore store(root, "catalog_*");

    const auto rec = store.Read(165920, "IP");
    CHECK_FALSE(rec.found);
    CHECK_FALSE(rec.defect);
}

TEST_CASE("the newest catalog snapshot wins") {
    // Deliberately does NOT need the shot cache. Resolution succeeds as soon
    // as the catalog and the index agree; the shotfile only decides whether
    // bytes come back. So version and snapshot are populated even on a miss,
    // and asserting them here keeps snapshot-precedence -- the selection logic
    // most likely to regress -- covered in CI, where the cache is absent.
    const std::string root = TempDir();
    WriteCatalog(root, 165920, 1);
    WriteIndex(root, 1);          // no shotfile: Read misses, resolve does not
    const std::string older = root + "/catalog/catalog_20260101T000000Z/latest";
    MkdirP(older);
    std::ofstream(older + "/1659.json") << "{\"165920\": 99}";

    fdp::PointStore store(root, "catalog_*");
    const auto rec = store.Read(165920, "IP");

    CHECK_FALSE(rec.found);        // no shotfile
    CHECK_FALSE(rec.defect);       // and that is not a defect
    CHECK(rec.version == 1);       // v1 from the NEWEST snapshot, not v99
    CHECK(rec.snapshot == kStamp);
    // The banner accessor must agree with what Read() actually used.
    CHECK(store.CurrentSnapshot() == kStamp);
}

TEST_CASE("version and snapshot are set only once resolution succeeds") {
    // A sharp edge worth pinning: these fields are populated whenever the
    // catalog and index agreed, even on a miss -- but stay defaulted when the
    // catalog never named the shot at all.
    const std::string root = TempDir();
    WriteCatalog(root, 165920, 2);
    WriteIndex(root, 2);
    fdp::PointStore store(root, "catalog_*");

    const auto resolved = store.Read(165920, "IP");   // resolved, no shotfile
    CHECK_FALSE(resolved.found);
    CHECK(resolved.version == 2);
    CHECK(resolved.snapshot == kStamp);

    const auto unminted = store.Read(999999, "IP");   // never resolved
    CHECK_FALSE(unminted.found);
    CHECK(unminted.version == 0);
    CHECK(unminted.snapshot.empty());
}

TEST_CASE("CurrentSnapshot names the snapshot in use, for the banner") {
    fdp::PointStore store(BuildStore(), "catalog_*");
    CHECK(store.CurrentSnapshot() == kStamp);
}
