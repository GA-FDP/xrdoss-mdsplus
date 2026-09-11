#include "PointStore.hh"

#include <ptdata/io_provider.h>
#include <ptdata/shot_file.h>
#include <ptdata/store_index.h>
#include <ptdata/version_spec.h>

namespace fdp {

struct PointStore::Impl {
    // LocalIoProvider, not ArchiveIoProvider: the store records no URLs, so
    // there is nothing to rewrite. See store spec §4.7b.
    ptdata::LocalIoProvider io;
    std::unique_ptr<ptdata::StoreIndex> index;

    Impl(const std::string &store_root, const std::string &catalog_pattern) {
        ptdata::StoreIndex::Config cfg;
        cfg.store_root  = store_root;
        cfg.io_provider = &io;
        if (!catalog_pattern.empty()) cfg.catalog_pattern = catalog_pattern;
        index = std::make_unique<ptdata::StoreIndex>(std::move(cfg));
    }
};

PointStore::PointStore(const std::string &store_root,
                       const std::string &catalog_pattern)
    : impl_(new Impl(store_root, catalog_pattern)) {}

PointStore::~PointStore() = default;

std::string PointStore::CurrentSnapshot() {
    return impl_->index->current_snapshot();
}

PointStore::Record PointStore::Read(int shot, const std::string &pointname) {
    Request req;
    req.shot = shot;
    req.pointname = pointname;
    return Read(req);
}

PointStore::Record PointStore::Read(const Request &req) {
    const int shot = req.shot;
    const std::string &pointname = req.pointname;
    Record out;

    ptdata::VersionSpec spec;
    if (req.version > 0) spec.version = req.version;
    if (!req.snapshot.empty()) spec.snapshot = req.snapshot;

    const auto look = impl_->index->resolve(shot, pointname, spec);
    if (!look.resolution) {
        // Only these two mean the store contradicted itself. A shot the
        // catalog never named, or a pointname this shot does not carry, is
        // ordinary absence and must stay silent -- see spec §4.3.
        out.defect = (look.miss == ptdata::StoreMiss::IndexMissing
                      || look.miss == ptdata::StoreMiss::IndexUnreadable);
        // A pin that cannot be honoured is neither absent data nor a defect.
        out.pin_failed = (look.miss == ptdata::StoreMiss::VersionMissing
                          || look.miss == ptdata::StoreMiss::SnapshotMissing);
        out.detail = look.detail;
        return out;
    }

    out.version  = look.resolution->version;
    out.snapshot = look.resolution->snapshot;

    // The snapshot is immutable, but a file it names can still be absent --
    // an incomplete publish, or a path pruned underneath us. Absent data, not
    // an error, so the client's chain advances.
    if (!impl_->io.stat(look.resolution->path)) {
        // The catalog and manifest proved this version was minted, but its
        // payload is gone -- pruned, or an incomplete publish. For an
        // UNPINNED read that is ordinary absence and the client's chain
        // advances. For a PINNED one it is the pin failing: the caller asked
        // for a specific version and cannot have it, which is a 409, not an
        // authoritative "this point never existed".
        out.pin_failed = req.version > 0 || !req.snapshot.empty();
        return out;
    }

    ptdata::ShotFile sf(impl_->io, look.resolution->path);
    const auto entry = sf.find(pointname);
    if (!entry) {
        // Same reasoning: the shotfile for the pinned version exists but does
        // not carry this pointname. Unpinned, ordinary absence; pinned, the
        // request cannot be satisfied as asked.
        out.pin_failed = req.version > 0 || !req.snapshot.empty();
        return out;
    }

    out.bytes     = sf.read_point(*entry);
    out.extension = look.resolution->extension;
    out.found     = true;
    return out;
}

}  // namespace fdp
