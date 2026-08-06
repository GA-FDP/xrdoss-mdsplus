#include "ResultCache.hh"

namespace fdp {

ResultCache::ResultCache(size_t max_bytes) : max_bytes_(max_bytes), bytes_(0) {}

bool ResultCache::Get(const std::string &key, std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::unordered_map<std::string, ListType::iterator>::iterator it = index_.find(key);
    if (it == index_.end()) return false;
    order_.splice(order_.begin(), order_, it->second);   // promote to most-recent
    out = it->second->second;
    return true;
}

void ResultCache::Put(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Drop any existing entry first, so its bytes are not counted twice and the
    // stale value cannot survive eviction of the new one.
    const std::unordered_map<std::string, ListType::iterator>::iterator it = index_.find(key);
    if (it != index_.end()) {
        bytes_ -= it->second->second.size();
        order_.erase(it->second);
        index_.erase(it);
    }

    // An entry larger than the whole budget would evict everything and then
    // itself; refusing it keeps the cache useful instead of thrashing.
    if (value.size() > max_bytes_) return;

    order_.push_front(std::make_pair(key, value));
    index_[key] = order_.begin();
    bytes_ += value.size();
    EvictLocked();
}

size_t ResultCache::BytesUsed() {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

void ResultCache::EvictLocked() {
    while (bytes_ > max_bytes_ && !order_.empty()) {
        const std::pair<std::string, std::string> &victim = order_.back();
        bytes_ -= victim.second.size();
        index_.erase(victim.first);
        order_.pop_back();
    }
}

}  // namespace fdp
