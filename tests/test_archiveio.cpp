#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "ArchiveIo.hh"

#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// A real file on disk, so stat()/open() are exercised rather than mocked --
// the bug this guards against is a rewrite that compiles and does nothing.
struct TempTree {
    std::string dir, path;
    TempTree() {
        char tmpl[] = "/tmp/archiveio.XXXXXX";
        dir = ::mkdtemp(tmpl);
        ::mkdir((dir + "/ptdata").c_str(), 0755);
        path = dir + "/ptdata/165920.MAG";
        std::ofstream(path) << "hello";
    }
    ~TempTree() {
        ::unlink(path.c_str());
        ::rmdir((dir + "/ptdata").c_str());
        ::rmdir(dir.c_str());
    }
};

const char *kPrefix = "pelican://osg-htc.org:443/fdp-d3d/archives";
const char *kUrl = "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/165920.MAG";

}  // namespace

TEST_CASE("a matching prefix is rewritten onto the local root") {
    TempTree t;
    fdp::ArchiveIoProvider io(kPrefix, t.dir);
    CHECK(io.Rewrite(kUrl) == t.dir + "/ptdata/165920.MAG");
    CHECK(io.stat(kUrl));
}

TEST_CASE("open() rewrites too, not only stat()") {
    // ShotLocator stats a path and then hands the same string to ShotFile,
    // which opens it. A rewrite in only one of the two passes the stat and
    // then fails to read, which looks exactly like a corrupt shotfile.
    TempTree t;
    fdp::ArchiveIoProvider io(kPrefix, t.dir);
    auto h = io.open(kUrl, O_RDONLY);
    REQUIRE(h != nullptr);
    char buf[5] = {0};
    CHECK(h->read(buf, 5) == 5);
    CHECK(std::string(buf, 5) == "hello");
}

TEST_CASE("a path outside the prefix passes through untouched") {
    TempTree t;
    fdp::ArchiveIoProvider io("pelican://other.example/x", t.dir);
    CHECK(io.Rewrite(t.path) == t.path);
    CHECK(io.stat(t.path));
}

TEST_CASE("the prefix must end at a path separator") {
    // ".../archives-private" shares a character prefix with ".../archives" but
    // is a different namespace; rewriting it would serve the wrong tree.
    TempTree t;
    fdp::ArchiveIoProvider io(kPrefix, t.dir);
    const std::string other =
        "pelican://osg-htc.org:443/fdp-d3d/archives-private/ptdata/165920.MAG";
    CHECK(io.Rewrite(other) == other);
}

TEST_CASE("the prefix itself, with nothing after it, still rewrites") {
    TempTree t;
    fdp::ArchiveIoProvider io(kPrefix, t.dir);
    CHECK(io.Rewrite(kPrefix) == t.dir);
}

TEST_CASE("a trailing slash on either side does not double up") {
    TempTree t;
    fdp::ArchiveIoProvider io(std::string(kPrefix) + "/", t.dir + "/");
    CHECK(io.Rewrite(kUrl) == t.dir + "/ptdata/165920.MAG");
    CHECK(io.stat(kUrl));
}

TEST_CASE("an empty prefix or root disables rewriting") {
    TempTree t;
    fdp::ArchiveIoProvider off("", "");
    CHECK(off.Rewrite(kUrl) == kUrl);
    CHECK(off.stat(t.path));

    fdp::ArchiveIoProvider half(kPrefix, "");
    CHECK(half.Rewrite(kUrl) == kUrl);
}
