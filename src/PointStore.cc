#include "PointStore.hh"

#include <ptdata/io_provider.h>
#include <ptdata/shot_file.h>
#include <ptdata/store_index.h>

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
    Record out;

    const auto look = impl_->index->resolve(shot, pointname);
    if (!look.resolution) {
        // Only these two mean the store contradicted itself. A shot the
        // catalog never named, or a pointname this shot does not carry, is
        // ordinary absence and must stay silent -- see spec §4.3.
        out.defect = (look.miss == ptdata::StoreMiss::IndexMissing
                      || look.miss == ptdata::StoreMiss::IndexUnreadable);
        out.detail = look.detail;
        return out;
    }

    out.version  = look.resolution->version;
    out.snapshot = look.resolution->snapshot;

    // The snapshot is immutable, but a file it names can still be absent --
    // an incomplete publish, or a path pruned underneath us. Absent data, not
    // an error, so the client's chain advances.
    if (!impl_->io.stat(look.resolution->path)) return out;

    ptdata::ShotFile sf(impl_->io, look.resolution->path);
    const auto entry = sf.find(pointname);
    if (!entry) return out;

    out.bytes     = sf.read_point(*entry);
    out.extension = look.resolution->extension;
    out.found     = true;
    return out;
}

}  // namespace fdp
