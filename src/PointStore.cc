#include "PointStore.hh"

#include "ArchiveIo.hh"

#include <ptdata/index_dir_resolver.h>
#include <ptdata/json_index_plugin.h>
#include <ptdata/shot_file.h>

namespace fdp {
namespace {

// ".MAG" from ".../165920.MAG"; empty when there is no suffix to take.
std::string ExtensionOf(const std::string &path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    return path.substr(dot);
}

}  // namespace

struct PointStore::Impl {
    ArchiveIoProvider io;
    std::unique_ptr<ptdata::JsonIndexPlugin> index;

    Impl(const std::string &index_dir, const std::string &index_pattern,
         const std::string &url_prefix, const std::string &local_root)
        : io(url_prefix, local_root) {
        // An empty pattern returns the parent unchanged, so this is
        // unconditional. Listing goes through io, which passes the index dir
        // through untouched: it is a local path we configure, not something
        // the index recorded.
        ptdata::JsonIndexPlugin::Config cfg;
        cfg.index_dir = ptdata::resolve_index_dir(io, index_dir, index_pattern);
        cfg.io_provider = &io;
        index = std::make_unique<ptdata::JsonIndexPlugin>(cfg);
    }
};

PointStore::PointStore(const std::string &index_dir,
                       const std::string &index_pattern,
                       const std::string &url_prefix,
                       const std::string &local_root)
    : impl_(new Impl(index_dir, index_pattern, url_prefix, local_root)) {}

PointStore::~PointStore() = default;

PointStore::Record PointStore::Read(int shot, const std::string &pointname) {
    Record out;

    // resolve() returns nullopt when the index does not know the pair, and
    // throws only on genuinely broken state (malformed JSON, transport failure
    // mid-read) -- which propagates, because that is a server fault and must
    // not reach the client as missing data.
    const auto path = impl_->index->resolve(pointname, shot);
    if (!path) return out;

    // The index is a snapshot and the archive moves under it. A recorded file
    // that is no longer there is absent data, not an error.
    if (!impl_->io.stat(*path)) return out;

    ptdata::ShotFile sf(impl_->io, *path);
    const auto entry = sf.find(pointname);
    if (!entry) return out;

    out.bytes = sf.read_point(*entry);
    out.extension = ExtensionOf(*path);
    out.found = true;
    return out;
}

}  // namespace fdp
