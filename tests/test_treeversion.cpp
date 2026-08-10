#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "TreeVersion.hh"

#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

// A throwaway directory laid out like the archive, so we can mutate files and
// watch the token move.
struct Archive {
    std::string root;

    Archive() {
        char tmpl[] = "/tmp/treever_XXXXXX";
        root = mkdtemp(tmpl);
        MakeDirs("codes/efit01/00/00/19/00");
        MakeDirs("shots/rf/00/00/19/00");
    }
    ~Archive() { std::system(("rm -rf " + root).c_str()); }

    void MakeDirs(const std::string &rel) {
        std::string acc = root;
        size_t start = 0;
        while (start <= rel.size()) {
            const size_t end = rel.find('/', start);
            acc += "/" + rel.substr(start, (end == std::string::npos ? rel.size() : end) - start);
            ::mkdir(acc.c_str(), 0755);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    void Write(const std::string &rel, const std::string &contents) const {
        std::ofstream f((root + "/" + rel).c_str(), std::ios::binary);
        f << contents;
    }
    std::string Search() const {
        return root + "/codes/%T/%B/%T_%S.datafile;" + root + "/shots/%T/%B/%T_%S.datafile";
    }
};

}  // namespace

TEST_CASE("expands the template placeholders") {
    CHECK(fdp::TreeVersion::Expand("/a/%T/%B/%T_%S.datafile", "efit01", 190000) ==
          "/a/efit01/00/00/19/00/efit01_190000.datafile");
    CHECK(fdp::TreeVersion::Expand("%%T", "efit01", 1) == "%T");
}

TEST_CASE("produces a token for an existing tree file") {
    Archive a;
    a.Write("codes/efit01/00/00/19/00/efit01_190000.datafile", "data");
    fdp::TreeVersion tv(a.Search());
    REQUIRE(tv.Configured());

    std::string v, err;
    REQUIRE(tv.Current("efit01", 190000, v, err));
    CHECK(err.empty());
    CHECK(v.size() == 17);          // 'v' + 16 hex
    CHECK(v[0] == 'v');
}

TEST_CASE("is stable across calls for an unchanged file") {
    Archive a;
    a.Write("codes/efit01/00/00/19/00/efit01_190000.datafile", "data");
    fdp::TreeVersion tv(a.Search());

    std::string v1, v2, err;
    REQUIRE(tv.Current("efit01", 190000, v1, err));
    REQUIRE(tv.Current("efit01", 190000, v2, err));
    CHECK(v1 == v2);
}

TEST_CASE("changes when the file changes -- the whole point") {
    Archive a;
    const std::string rel = "codes/efit01/00/00/19/00/efit01_190000.datafile";
    a.Write(rel, "original");
    fdp::TreeVersion tv(a.Search());

    std::string before, after, err;
    REQUIRE(tv.Current("efit01", 190000, before, err));

    // Rewrite with different content AND a different mtime, as a re-analysis
    // would.
    ::sleep(1);
    a.Write(rel, "re-analysed, different length");
    REQUIRE(tv.Current("efit01", 190000, after, err));

    CHECK(before != after);
}

TEST_CASE("searches templates in order and falls through to later branches") {
    Archive a;
    a.Write("shots/rf/00/00/19/00/rf_190000.datafile", "x");
    fdp::TreeVersion tv(a.Search());

    std::string v, err;
    REQUIRE(tv.Current("rf", 190000, v, err));   // only exists under shots/
    CHECK_FALSE(v.empty());
}

TEST_CASE("distinguishes trees and shots") {
    Archive a;
    a.Write("codes/efit01/00/00/19/00/efit01_190000.datafile", "same");
    a.Write("codes/efit01/00/00/19/00/efit01_190001.datafile", "same");
    fdp::TreeVersion tv(a.Search());

    std::string v0, v1, err;
    REQUIRE(tv.Current("efit01", 190000, v0, err));
    REQUIRE(tv.Current("efit01", 190001, v1, err));
    // Identical contents, but different files: the inode separates them.
    CHECK(v0 != v1);
}

TEST_CASE("reports a missing tree file rather than inventing a token") {
    Archive a;
    fdp::TreeVersion tv(a.Search());
    std::string v, err;
    CHECK_FALSE(tv.Current("efit01", 999999, v, err));
    CHECK_FALSE(err.empty());
    CHECK(v.empty());
}

TEST_CASE("an unconfigured resolver fails closed") {
    fdp::TreeVersion tv("");
    CHECK_FALSE(tv.Configured());

    std::string v, err;
    CHECK_FALSE(tv.Current("efit01", 190000, v, err));
    CHECK_FALSE(err.empty());
}
