#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fdp {

// Resolves one (shot, pointname) to its raw record bytes through the per-shot
// versioned store: a catalog snapshot names the shot's version, and that
// version directory's own meta/index.json names the extension. The shotfile
// path is CONSTRUCTED from the version directory, never recorded (store spec
// §4.7b) -- which is why there is no URL rewrite here any more, and no
// pointurlprefix/pointroot pair that has to be set together to avoid silently
// resolving every entry as a missing file.
//
// Index-only is structural rather than configured: this holds a StoreIndex and
// reads the file it names, with no ShotLocator behind it, so there is no tier
// to fall through to -- no SYS_D3 scan, and no ptserver socket. That matters
// twice. A directory scan is far more expensive than the lookup it would
// replace, because the archive lives on a parallel filesystem where every stat
// is a metadata round trip. And a process serving an origin's own files has no
// business dialling anywhere, so the absence of a ptserver tier is better had
// by construction than by setting PTDATA_PTSERVERS=none and trusting it.
//
// No ptdata header appears here on purpose. ptdata requires C++20 (std::span,
// std::optional) while this repo builds at C++14 for XRootD and MDSplus, so
// the dependency is kept inside PointStore.cc rather than forced on every
// translation unit that wants to serve a point.
class PointStore {
public:
    struct Record {
        bool found = false;
        std::vector<std::uint8_t> bytes;
        std::string extension;   // ".MAG" etc., from the index
        int         version = 0; // the store version it came from
        std::string snapshot;    // the catalog snapshot that named that version

        // A miss the caller must LOG rather than pass on silently: the catalog
        // promised a version whose index is missing or unreadable. Kept
        // separate from `found` because one corrupt shot must not fail
        // requests for every other shot -- see spec §4.3. An ordinary miss
        // (never minted, unknown pointname) leaves this false.
        bool        defect = false;
        std::string detail;      // what to put in that log line

        // A pin the store cannot satisfy: the requested version or snapshot
        // does not exist. Distinct from both `found` and `defect` -- it is
        // neither absent data nor a corrupt publish, but a request about
        // provenance that has gone stale. The endpoint answers 409 for this,
        // never 404, because the client treats a 404 from tier 1 as
        // authoritative absence.
        bool        pin_failed = false;
    };

    // What to read. A struct rather than more positional arguments so a later
    // coordinate (B7's cohort) does not change the signature again.
    struct Request {
        int shot = 0;
        std::string pointname;
        // Empty means "latest", which is the whole of today's behaviour.
        // version = 0 means unset; a real version is >= 1.
        int version = 0;
        std::string snapshot;
    };

    // store_root: the namespace root holding catalog/ and views/, e.g.
    //             "/fdp-d3d" as seen inside the origin container.
    // catalog_pattern: glob selecting snapshots under <store_root>/catalog;
    //             empty means the built-in default.
    PointStore(const std::string &store_root,
               const std::string &catalog_pattern);
    ~PointStore();

    PointStore(const PointStore &) = delete;
    PointStore &operator=(const PointStore &) = delete;

    // A miss returns {found=false} and never throws: absent data is the
    // ordinary case and the client's provider chain advances on it. Genuine
    // failures -- unreadable shotfile, malformed header -- throw
    // ptdata::PtDataError, which the caller turns into a 500.
    Record Read(const Request &req);

    // Convenience overload for an unpinned read; delegates to the struct form.
    Record Read(int shot, const std::string &pointname);

    // The snapshot currently in use, for the startup banner.
    std::string CurrentSnapshot();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fdp
