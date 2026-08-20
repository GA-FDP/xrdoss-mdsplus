# HTTP point endpoint (server) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serve `GET /<pointprefix>/<shot>/<pointname>` from the origin, returning one PTData record's raw bytes, so a remote client needs neither a shot index nor XRootD.

**Architecture:** A second route inside the existing `XrdHttpMdsip` ext handler — XRootD allows four HTTP ext handlers and all four are taken, so this cannot be a new plugin. Resolution is index-first through `libptd3d` built `PTDATA_WITH_FDPIO=OFF`, with an `IoProvider` that rewrites the index's `pelican://` URLs to a local archive root.

**Tech Stack:** C++17, XRootD `XrdHttpExtHandler`, `libptd3d` (ptdata ≥ 2.2.0), doctest, CMake, pixi.

Design: `docs/superpowers/specs/2026-08-19-http-point-endpoint-server-design.md`.
Client half: GA-FDP/ptdata#46. Contract: `ptdata:docs/superpowers/specs/2026-08-19-http-point-provider-design.md`.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/ArchiveIo.hh/.cc` | `ArchiveIoProvider` — rewrites a URL prefix to a local root, delegates to `LocalIoProvider` |
| `src/PointStore.hh/.cc` | Owns the index plugin + locator; `Read(shot, pointname) -> {bytes, extension}` |
| `src/HttpRelay.cc` | Route `GET` under `pointprefix` to a new `DoPoint`; loader parses the point parameters |
| `tests/test_archiveio.cpp` | Prefix rewriting, including the cases that silently do nothing if wrong |
| `tests/test_pointstore.cpp` | Resolution and read against a hand-built index over the ptdata test shotfiles |
| `CMakeLists.txt` | New sources, link `libptd3d`, register both tests |
| `docs/deployment-notes.md` | The config line, the two mounts, what changes on the origin |

`ArchiveIoProvider` and `PointStore` are separate because the first is a pure
string-and-syscall concern testable with `tmp_path`, and the second needs real
shotfiles. Keeping them apart means the interesting failure — a rewrite that
compiles and does nothing — is caught by a test that needs no data.

---

## Task 1: ArchiveIoProvider

**Files:**
- Create: `src/ArchiveIo.hh`, `src/ArchiveIo.cc`
- Test: `tests/test_archiveio.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_archiveio.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "ArchiveIo.hh"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
// A real file, so stat()/open() are exercised rather than mocked.
struct TempFile {
    std::string dir, path;
    TempFile() {
        char tmpl[] = "/tmp/archiveio.XXXXXX";
        dir = ::mkdtemp(tmpl);
        ::mkdir((dir + "/ptdata").c_str(), 0755);
        path = dir + "/ptdata/165920.MAG";
        std::ofstream(path) << "hello";
    }
    ~TempFile() { ::unlink(path.c_str()); ::rmdir((dir + "/ptdata").c_str()); ::rmdir(dir.c_str()); }
};
}  // namespace

TEST_CASE("a matching prefix is rewritten to the local root") {
    TempFile f;
    fdp::ArchiveIoProvider io("pelican://osg-htc.org:443/fdp-d3d/archives", f.dir);
    CHECK(io.stat("pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/165920.MAG"));
}

TEST_CASE("open() rewrites too, not just stat()") {
    TempFile f;
    fdp::ArchiveIoProvider io("pelican://osg-htc.org:443/fdp-d3d/archives", f.dir);
    auto h = io.open("pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/165920.MAG", O_RDONLY);
    REQUIRE(h != nullptr);
    char buf[5] = {0};
    CHECK(h->read(buf, 5) == 5);
    CHECK(std::string(buf, 5) == "hello");
}

TEST_CASE("a path that does not match the prefix is passed through untouched") {
    TempFile f;
    fdp::ArchiveIoProvider io("pelican://other.example/x", f.dir);
    // Absolute local path: must still work, so a mixed configuration degrades
    // to plain local behaviour rather than mangling the path.
    CHECK(io.stat(f.path));
}

TEST_CASE("the prefix must end at a path separator") {
    TempFile f;
    fdp::ArchiveIoProvider io("pelican://osg-htc.org:443/fdp-d3d/archives", f.dir);
    // ".../archives-private/..." shares a character prefix but is a different
    // namespace; rewriting it would silently serve the wrong tree.
    CHECK_FALSE(io.stat("pelican://osg-htc.org:443/fdp-d3d/archives-private/ptdata/165920.MAG"));
}

TEST_CASE("a trailing slash on either side does not double up") {
    TempFile f;
    fdp::ArchiveIoProvider io("pelican://osg-htc.org:443/fdp-d3d/archives/", f.dir + "/");
    CHECK(io.stat("pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/165920.MAG"));
}

TEST_CASE("an empty prefix disables rewriting entirely") {
    TempFile f;
    fdp::ArchiveIoProvider io("", "");
    CHECK(io.stat(f.path));
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
pixi run build
```
Expected: FAIL — no such file `ArchiveIo.hh`.

- [ ] **Step 3: Write the header**

Create `src/ArchiveIo.hh`:

```cpp
#pragma once

#include <ptdata/io_provider.h>

#include <memory>
#include <string>
#include <vector>

namespace fdp {

// An IoProvider that maps the absolute pelican:// URLs recorded in the ptdata
// JSON index onto the origin's local archive, then delegates to
// LocalIoProvider.
//
// The mdsip sandbox solves the same problem with a symlink chain, relying on
// the URL being a relative path once POSIX collapses its double slash. That
// needs the process's working directory to be "/" -- fine when you launch the
// process, but this handler is loaded into Pelican's xrootd and its cwd is not
// ours to set. Rewriting explicitly removes that dependency, and removes the
// federation hostname from a directory name into configuration.
//
// NOTE: the rewrite belongs in open() and stat(). IoProvider::resolve() looks
// like the hook and is not -- ShotLocator calls stat() and hands the path to
// ShotFile directly, and nothing on that path calls resolve() at all.
class ArchiveIoProvider : public ptdata::IoProvider {
public:
    // url_prefix: e.g. "pelican://osg-htc.org:443/fdp-d3d/archives"
    // local_root: e.g. "/fdp-archives"
    // Either empty disables rewriting; paths then pass through unchanged.
    ArchiveIoProvider(std::string url_prefix, std::string local_root);

    std::unique_ptr<ptdata::IoHandle> open(const std::string &path, int flags) override;
    bool stat(const std::string &path) override;
    std::string resolve(const std::string &path) override;
    std::vector<std::string> list_dirs(const std::string &path) override;
    std::vector<std::string> list_files(const std::string &path) override;

    // Exposed for tests: the path actually handed to the filesystem.
    std::string Rewrite(const std::string &path) const;

private:
    std::string url_prefix_;   // no trailing slash
    std::string local_root_;   // no trailing slash
    ptdata::LocalIoProvider local_;
};

}  // namespace fdp
```

- [ ] **Step 4: Write the implementation**

Create `src/ArchiveIo.cc`:

```cpp
#include "ArchiveIo.hh"

namespace fdp {
namespace {

std::string TrimTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

ArchiveIoProvider::ArchiveIoProvider(std::string url_prefix, std::string local_root)
    : url_prefix_(TrimTrailingSlash(std::move(url_prefix))),
      local_root_(TrimTrailingSlash(std::move(local_root))) {}

std::string ArchiveIoProvider::Rewrite(const std::string &path) const {
    if (url_prefix_.empty() || local_root_.empty()) return path;
    if (path.compare(0, url_prefix_.size(), url_prefix_) != 0) return path;

    // The prefix must end at a separator, or ".../archives" also claims
    // ".../archives-private" and silently serves the wrong tree. Same trap the
    // relay's MatchesPath documents for HTTP paths.
    const char after = path.size() > url_prefix_.size() ? path[url_prefix_.size()] : '\0';
    if (after != '/' && after != '\0') return path;

    return local_root_ + path.substr(url_prefix_.size());
}

std::unique_ptr<ptdata::IoHandle> ArchiveIoProvider::open(const std::string &path, int flags) {
    return local_.open(Rewrite(path), flags);
}

bool ArchiveIoProvider::stat(const std::string &path) {
    return local_.stat(Rewrite(path));
}

std::string ArchiveIoProvider::resolve(const std::string &path) {
    return local_.resolve(Rewrite(path));
}

std::vector<std::string> ArchiveIoProvider::list_dirs(const std::string &path) {
    return local_.list_dirs(Rewrite(path));
}

std::vector<std::string> ArchiveIoProvider::list_files(const std::string &path) {
    return local_.list_files(Rewrite(path));
}

}  // namespace fdp
```

- [ ] **Step 5: Register it**

In `CMakeLists.txt`, add `src/ArchiveIo.cc` to a new `pointcore` object library
(it must not go in `core`, which links MDSplus — the point path needs none):

```cmake
    add_library(pointcore OBJECT src/ArchiveIo.cc src/PointStore.cc)
    target_include_directories(pointcore PRIVATE src ${PTDATA_INCLUDE_DIR})
```

and register the test in the `BUILD_TESTS` block:

```cmake
    add_executable(test_archiveio tests/test_archiveio.cpp $<TARGET_OBJECTS:pointcore>)
    target_include_directories(test_archiveio PRIVATE src ${PTDATA_INCLUDE_DIR} ${DOCTEST_INCLUDE_DIR})
    target_link_directories(test_archiveio PRIVATE ${PTDATA_LIB_DIR})
    target_link_libraries(test_archiveio ptd3d pthread)
    add_test(NAME archiveio COMMAND test_archiveio)
```

`PTDATA_INCLUDE_DIR` / `PTDATA_LIB_DIR` are introduced in Task 5; until then,
point them at a local ptdata build with `-DPTDATA_ROOT=`. If that ordering
proves awkward, do Task 5 first — the tasks are independent.

- [ ] **Step 6: Run and commit**

```bash
pixi run build && pixi run test
```
Expected: all previous tests plus 6 new ones pass.

```bash
git add src/ArchiveIo.hh src/ArchiveIo.cc tests/test_archiveio.cpp CMakeLists.txt
git commit -m "feat: rewrite index URLs onto the local archive

The ptdata JSON index records absolute pelican:// URLs. The mdsip sandbox
opens them via a symlink chain that depends on the process cwd being '/';
this handler is loaded into Pelican's xrootd, whose cwd is not ours to set,
so it rewrites the prefix explicitly instead.

The rewrite is in open() and stat(). IoProvider::resolve() looks like the hook
and is not: ShotLocator calls stat() and hands the path to ShotFile directly,
so overriding only resolve() would compile and do nothing."
```

---

## Task 2: PointStore

**Files:**
- Create: `src/PointStore.hh`, `src/PointStore.cc`
- Test: `tests/test_pointstore.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_pointstore.cpp`. It builds a one-entry index over a real
shotfile, so the whole resolution path runs:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "PointStore.hh"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace {

// $PTDATA_TEST_SHOTS_DIR, the same cache ptdata's own tests use.
std::string ShotsDir() {
    const char *d = std::getenv("PTDATA_TEST_SHOTS_DIR");
    return d ? d : "/cscratch/sammuli/ptdata_test_files";
}

// Write <root>/1659/165920.json pointing IP at the real .MAG, using an
// absolute pelican:// URL exactly as the production indexer records it.
std::string BuildIndex(const std::string &root, const std::string &url_prefix) {
    ::mkdir(root.c_str(), 0755);
    const std::string sub = root + "/1659";
    ::mkdir(sub.c_str(), 0755);
    std::ofstream(sub + "/165920.json")
        << "{\"shot\":165920,"
        << "\"ext_location\":{\".MAG\":\"" << url_prefix << "/165920.MAG\"},"
        << "\"pointname_ext\":{\"IP\":\".MAG\"}}";
    return root;
}

}  // namespace

TEST_CASE("resolves through the index and returns the record bytes") {
    if (ShotsDir().empty()) return;
    char tmpl[] = "/tmp/pointstore.XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    const std::string url_prefix = "pelican://osg-htc.org:443/fdp-d3d/archives";
    BuildIndex(root + "/idx", url_prefix);

    fdp::PointStore store(root + "/idx", "", url_prefix, ShotsDir());
    const auto rec = store.Read(165920, "IP");

    REQUIRE(rec.found);
    CHECK(rec.bytes.size() > 0);
    CHECK(rec.extension == ".MAG");
}

TEST_CASE("a pointname absent from the index is a miss, not an error") {
    char tmpl[] = "/tmp/pointstore.XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    BuildIndex(root + "/idx", "pelican://x/y");

    fdp::PointStore store(root + "/idx", "", "pelican://x/y", ShotsDir());
    CHECK_FALSE(store.Read(165920, "NO_SUCH_POINT").found);
}

TEST_CASE("a shot absent from the index is a miss, not an error") {
    char tmpl[] = "/tmp/pointstore.XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    BuildIndex(root + "/idx", "pelican://x/y");

    fdp::PointStore store(root + "/idx", "", "pelican://x/y", ShotsDir());
    CHECK_FALSE(store.Read(999999, "IP").found);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
pixi run build
```
Expected: FAIL — no `PointStore.hh`.

- [ ] **Step 3: Write the header**

Create `src/PointStore.hh`:

```cpp
#pragma once

#include "ArchiveIo.hh"

#include <ptdata/json_index_plugin.h>
#include <ptdata/shot_locator.h>

#include <memory>
#include <string>
#include <vector>

namespace fdp {

// Resolves one (shot, pointname) to its raw record bytes, index-first.
//
// Deliberately holds no ptserver providers: an index miss must read as absent
// data, never as a socket attempt from a process that should not be dialling
// anywhere. A directory scan is not configured either -- sys_d3_paths is empty
// -- because the archive is on a parallel filesystem where a scan is far more
// expensive than the index lookup it would replace.
class PointStore {
public:
    struct Record {
        bool found = false;
        std::vector<uint8_t> bytes;
        std::string extension;   // e.g. ".MAG", from the path the index chose
    };

    // index_pattern may be empty (use index_dir directly) or a glob such as
    // "json_indexes_*" selecting the newest snapshot beneath index_dir.
    PointStore(std::string index_dir, std::string index_pattern,
               std::string url_prefix, std::string local_root);

    // Never throws for "not here": a miss returns {found=false}. Genuine
    // failures -- unreadable file, malformed header -- throw ptdata::PtDataError.
    Record Read(int shot, const std::string &pointname);

private:
    ArchiveIoProvider io_;
    std::unique_ptr<ptdata::JsonIndexPlugin> index_;
    std::unique_ptr<ptdata::ShotLocator> locator_;
};

}  // namespace fdp
```

- [ ] **Step 4: Write the implementation**

Create `src/PointStore.cc`. The extension comes from the resolved path's
suffix — `Located::source_description` is `"index: <path>"`:

```cpp
#include "PointStore.hh"

#include <ptdata/error.h>
#include <ptdata/index_dir_resolver.h>

namespace fdp {
namespace {

// ".MAG" from ".../165920.MAG". Empty when there is no suffix to take.
std::string ExtensionOf(const std::string &path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    return path.substr(dot);
}

}  // namespace

PointStore::PointStore(std::string index_dir, std::string index_pattern,
                       std::string url_prefix, std::string local_root)
    : io_(std::move(url_prefix), std::move(local_root)) {
    std::string dir = index_dir;
    if (!index_pattern.empty()) {
        dir = ptdata::resolve_index_dir(index_dir, index_pattern);
    }

    ptdata::JsonIndexPlugin::Config icfg;
    icfg.index_dir = dir;
    icfg.io_provider = &io_;
    index_ = std::make_unique<ptdata::JsonIndexPlugin>(icfg);

    ptdata::ShotLocator::Config lcfg;
    lcfg.io_provider = &io_;
    lcfg.index_plugin = index_.get();
    // sys_d3_paths and point_providers deliberately empty; see the header.
    locator_ = std::make_unique<ptdata::ShotLocator>(lcfg);
}

PointStore::Record PointStore::Read(int shot, const std::string &pointname) {
    Record out;
    ptdata::ShotLocator::Located located;
    try {
        located = locator_->locate_point(shot, "", pointname);
    } catch (const ptdata::PtDataError &e) {
        if (e.code() == ptdata::ErrorCode::ShotNotFound ||
            e.code() == ptdata::ErrorCode::PointnameNotFound) {
            return out;   // found = false
        }
        throw;
    }

    auto *sf = std::get_if<ptdata::ShotFile>(&located.data);
    if (!sf) return out;   // no point providers configured, so this cannot happen

    auto entry = sf->find(pointname);
    if (!entry) return out;

    out.bytes = sf->read_point(*entry);
    out.extension = ExtensionOf(located.source_description);
    out.found = true;
    return out;
}

}  // namespace fdp
```

**Check `resolve_index_dir`'s real name and header before using it** — ptdata
has an index-dir resolver used for the `json_indexes_*` pattern
(`cpp/src/include/ptdata/`); if the symbol differs, use the real one and say so.
If it is not public, resolve the newest matching directory here instead and note
it as a small ptdata gap.

- [ ] **Step 5: Register and run**

Add `src/PointStore.cc` to `pointcore` (already listed in Task 1's snippet) and
the test:

```cmake
    add_executable(test_pointstore tests/test_pointstore.cpp $<TARGET_OBJECTS:pointcore>)
    target_include_directories(test_pointstore PRIVATE src ${PTDATA_INCLUDE_DIR} ${DOCTEST_INCLUDE_DIR})
    target_link_directories(test_pointstore PRIVATE ${PTDATA_LIB_DIR})
    target_link_libraries(test_pointstore ptd3d pthread)
    add_test(NAME pointstore COMMAND test_pointstore)
```

```bash
pixi run build && pixi run test
```
Expected: pass. If the shot cache is absent the first test returns early —
say so in the report rather than reporting a pass.

- [ ] **Step 6: Commit**

```bash
git add src/PointStore.hh src/PointStore.cc tests/test_pointstore.cpp CMakeLists.txt
git commit -m "feat: resolve a point to its raw record, index-first

No ptserver providers and no sys_d3 scan: an index miss must read as absent
data rather than a socket attempt, and the archive is on a parallel filesystem
where scanning costs far more than the lookup it replaces.

The resolved extension comes from the path the index chose, which is what the
X-Ptdata-Extension response header reports."
```

---

## Task 3: Serve it

**Files:**
- Modify: `src/HttpRelay.cc`

- [ ] **Step 1: Claim GET under the point prefix**

`MatchesPath` currently refuses everything but POST and PUT. Add, before that
check:

```cpp
        // GET, only under the point prefix. The relay refuses GET because it
        // would shadow the object namespace the Oss plugin serves; that does
        // not apply to a disjoint prefix that is not a storage path.
        if (std::strcmp(verb, "GET") == 0) {
            if (point_prefix_.empty()) return false;      // endpoint disabled
            if (std::strncmp(path, point_prefix_.c_str(), point_prefix_.size()) != 0)
                return false;
            const char after = path[point_prefix_.size()];
            return after == '\0' || after == '/';
        }
```

- [ ] **Step 2: Route it**

In `ProcessReq`, before the existing action dispatch:

```cpp
        if (std::strncmp(req.verb.c_str(), "GET", 3) == 0) return DoPoint(req);
```

Check what `XrdHttpExtReq` actually exposes for the verb — the existing code
reads `req.resource` and `req.headers`; if there is no verb member, keep a flag
set in `MatchesPath` is **not** safe (the object is shared across requests), so
instead route on the resource matching `point_prefix_`.

- [ ] **Step 3: Implement DoPoint**

```cpp
    // GET <point_prefix>/<shot>/<pointname>[?ext=...]
    //
    // ?ext is accepted and ignored: resolution is index-first and
    // JsonIndexPlugin::resolve takes only (pointname, shot), so the index
    // decides which extension holds a pointname. X-Ptdata-Extension reports
    // what actually answered, which is how a client learns.
    int DoPoint(XrdHttpExtReq &req) {
        std::string why;
        if (!Authorized(req, why, point_authpath_))
            return Fail(req, 401, "Unauthorized", why);

        const std::string rest = req.resource.substr(point_prefix_.size());
        // rest is "/<shot>/<pointname>" possibly with "?ext=..."
        std::string shot_s, point_s;
        if (!SplitPointPath(rest, shot_s, point_s))
            return Fail(req, 404, "Not Found", "expected /<shot>/<pointname>");

        char *end = nullptr;
        const long shot = std::strtol(shot_s.c_str(), &end, 10);
        if (!end || *end != '\0' || shot <= 0)
            return Fail(req, 400, "Bad Request", "shot must be a positive integer");

        PointStore::Record rec;
        try {
            rec = points_->Read(static_cast<int>(shot), point_s);
        } catch (const ptdata::PtDataError &e) {
            return Fail(req, 500, "Internal Server Error", e.what());
        }
        if (!rec.found)
            return Fail(req, 404, "Not Found",
                        "no point " + point_s + " for shot " + shot_s);

        const std::string extra = rec.extension.empty()
            ? std::string()
            : "X-Ptdata-Extension: " + rec.extension + "\r\n";
        return req.SendSimpleResp(200, "OK", extra.empty() ? 0 : extra.c_str(),
                                  reinterpret_cast<const char *>(rec.bytes.data()),
                                  static_cast<long long>(rec.bytes.size()));
    }
```

`SplitPointPath` percent-decodes the pointname and drops any query string.
Write it beside `HeaderValue`; the repo already has `UrlEncode`, so check for a
decoder before adding one.

**Verify `SendSimpleResp`'s third parameter is a header string** appended to the
response — the existing calls pass `0`. If its contract differs, find how
XrdHttpTPC sets a custom response header and follow that.

- [ ] **Step 4: Give Authorized a path argument**

It currently closes over `authpath_`. The point route needs its own, so add a
parameter and pass `authpath_` at the existing call site — one line each, and it
keeps a single implementation.

- [ ] **Step 5: Build and commit**

```bash
pixi run build && pixi run test
```

```bash
git add src/HttpRelay.cc
git commit -m "feat: serve GET <pointprefix>/<shot>/<pointname>

404 when the index has no such point, so the client's provider chain advances;
anything else propagates, so a broken origin is not mistaken for a miss.
Authorization is per request against the ptdata archive path -- unlike the
relay's session, each GET is independent and a session would only cache
authorization decisions."
```

---

## Task 4: Configuration

**Files:**
- Modify: `src/HttpRelay.cc` (the `XrdHttpGetExtHandler` loader)

- [ ] **Step 1: Parse the point parameters**

```cpp
    const std::string point_prefix    = ParmValue(parms, "pointprefix", "");
    const std::string point_authpath  = ParmValue(parms, "pointauthpath", "");
    const std::string point_index     = ParmValue(parms, "pointindex", "");
    const std::string point_pattern   = ParmValue(parms, "pointindexpattern", "");
    const std::string point_urlprefix = ParmValue(parms, "pointurlprefix", "");
    const std::string point_root      = ParmValue(parms, "pointroot", "");
```

- [ ] **Step 2: Refuse a half-configuration**

An endpoint that loads but cannot resolve anything is worse than one that does
not load — it answers 404 for every point and looks like missing data:

```cpp
    if (!point_prefix.empty()) {
        if (point_index.empty()) {
            eDest->Emsg("point", "refusing to load: pointprefix is set but "
                        "pointindex is not, so every lookup would 404 and read "
                        "as missing data rather than as misconfiguration.");
            return 0;
        }
        if (point_authpath.empty()) {
            eDest->Emsg("point", "refusing to load: pointprefix is set but "
                        "pointauthpath is not. An ext handler runs BEFORE "
                        "XRootD authorization, so without it the archive would "
                        "be readable by anyone who can reach this port.");
            return 0;
        }
        eDest->Say("++++++ XrdHttpMdsip point endpoint: ", point_prefix.c_str());
    }
```

Note `auth=none` is already a supported mode for the relay; the check above
still demands `pointauthpath` because the two are independently configured and
a silent open archive is the worse failure.

- [ ] **Step 3: Pass them through**

Extend the `HttpMdsipRelay` constructor to take the point parameters, construct
a `PointStore` when `point_prefix` is non-empty, and leave `points_` null
otherwise. `MatchesPath` already returns false for GET when the prefix is empty,
so a null store is unreachable — but assert it rather than trust that.

- [ ] **Step 4: Build and commit**

```bash
pixi run build && pixi run test
git add src/HttpRelay.cc
git commit -m "feat: configure the point endpoint from the exthandler line

Off unless pointprefix is set, so this ships dark and an origin that only wants
the relay is unaffected. Refuses to load half-configured: an endpoint that
answers 404 for everything reads as missing data rather than as a mistake."
```

---

## Task 5: Build against libptd3d

**Files:**
- Modify: `CMakeLists.txt`, `pixi.toml`, `scripts/fetch-ptdata-src.sh` (reuse), `Containerfile.build`

- [ ] **Step 1: Find ptdata**

```cmake
# libptd3d, built PTDATA_WITH_FDPIO=OFF so the handler is physically incapable
# of remote I/O -- the origin reads local files, and a library that cannot
# reach the network cannot be turned into an egress path.
set(PTDATA_ROOT "/usr/local/ptdata" CACHE PATH "libptd3d install prefix")
find_path(PTDATA_INCLUDE_DIR NAMES ptdata/shot_locator.h
    HINTS ${PTDATA_ROOT}/include ${PTDATA_ROOT}/include/ptd3d)
find_library(PTDATA_LIB_DIR NAMES ptd3d HINTS ${PTDATA_ROOT}/lib ${PTDATA_ROOT}/lib64)
```

Note the sandbox's Containerfile already installs to `/usr/local/ptdata` and
notes the `lib` vs `lib64` split — check `Containerfile.mdsip` and match it.

- [ ] **Step 2: Link it into the handler**

```cmake
    target_link_libraries(XrdHttpMdsip XrdUtils ${XROOTD_HTTPUTILS_LIB} pthread ptd3d)
```

- [ ] **Step 3: Decide how libptd3d reaches the origin**

Two options, per the design. Build the handler against a **static** libptd3d if
ptdata gains a static target (one artifact to mount); otherwise mount
`libptd3d.so` beside the handler and set an rpath. Try static first: `ptd3d` is
declared `SHARED` only in ptdata's CMake, so this needs a small upstream
addition. **Report which you did.**

- [ ] **Step 4: Build and commit**

```bash
pixi run build && pixi run test
git add CMakeLists.txt pixi.toml
git commit -m "build: link the handler against libptd3d"
```

---

## Task 6: Contract tests against the real handler

**Files:**
- Create: `tests/integration/test_point_endpoint.sh`

- [ ] **Step 1: Point ptdata's own suite at it**

ptdata#46 ships seven tests against a stub implementing this contract
(`cpp/tests/python/test_http_point_provider.py`). Run them against a locally
started xrootd with this handler loaded, by setting the endpoint to it. That
turns the client's contract suite into the server's acceptance suite, which is
the whole point of having written the contract down.

Note those tests need `PTDATA_JSON_INDEX_DIR` and `PTDATA_PTSERVERS` suppressed
on the *client* side (they have an autouse fixture that does it), or the client
resolves locally and never calls the server.

- [ ] **Step 2: Authorization**

Extend `tests/security/` in the shape already used for the relay: an
unauthenticated GET under the point prefix must be refused, and a token valid
for the archive path accepted.

- [ ] **Step 3: Commit**

```bash
git add tests/integration/test_point_endpoint.sh
git commit -m "test: run the client contract suite against the real handler"
```

---

## Task 7: Deployment notes

**Files:**
- Modify: `docs/deployment-notes.md`

- [ ] **Step 1: Document the delta**

Extend the existing ext-handler section with the point parameters, the two
read-only bind mounts (`<archive>/ptdata`, `<archive>/index/json`), and the fact
that this consumes no additional handler slot because it rides the existing one
— with the `MAX_XRDHTTPEXTHANDLERS 4` reason stated, since that is what forces
the shape.

Repeat the three-configuration-surfaces warning: `scripts/mdsip-sandbox.sh`,
`deploy/fdp-mdsip.container`, and `d3d-origin-admin`'s `site.env` +
`etc/fdp-mdsip.container.in` must move together.

- [ ] **Step 2: Commit**

```bash
git add docs/deployment-notes.md
git commit -m "docs: deploying the point endpoint on the origin"
```
