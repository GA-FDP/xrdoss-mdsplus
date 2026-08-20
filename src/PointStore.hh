#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fdp {

// Resolves one (shot, pointname) to its raw record bytes, through the ptdata
// JSON index and nothing else.
//
// Index-only is structural rather than configured: this holds an IndexPlugin
// and reads the file it names, with no ShotLocator behind it, so there is no
// tier to fall through to -- no SYS_D3 scan, and no ptserver socket. That
// matters twice. A directory scan is far more expensive than the lookup it
// would replace, because the archive lives on a parallel filesystem where
// every stat is a metadata round trip. And a process serving an origin's own
// files has no business dialling anywhere, so the absence of a ptserver tier
// is better had by construction than by setting PTDATA_PTSERVERS=none and
// trusting it.
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
        std::string extension;   // ".MAG" etc., from the path the index chose
    };

    // index_pattern may be empty (use index_dir as given) or a glob such as
    // "json_indexes_*", selecting the lexical-max match beneath index_dir --
    // chronological order, for that naming scheme.
    PointStore(const std::string &index_dir, const std::string &index_pattern,
               const std::string &url_prefix, const std::string &local_root);
    ~PointStore();

    PointStore(const PointStore &) = delete;
    PointStore &operator=(const PointStore &) = delete;

    // A miss returns {found=false} and never throws: absent data is the
    // ordinary case and the client's provider chain advances on it. Genuine
    // failures -- unreadable file, malformed header -- throw
    // ptdata::PtDataError, which the caller turns into a 500.
    Record Read(int shot, const std::string &pointname);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fdp
