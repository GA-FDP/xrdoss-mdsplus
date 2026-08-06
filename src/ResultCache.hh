#ifndef FDP_RESULTCACHE_HH
#define FDP_RESULTCACHE_HH

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace fdp {

// Bounded LRU keyed by object path, thread-safe.
//
// Exists because Stat must report a size before any Read, which means
// evaluating during Stat; this keeps that payload so the following Open/Read
// does not evaluate again. A miss is always recoverable by re-evaluating, so
// this is an optimisation and never a correctness dependency — that is what
// lets it be a fixed-size cache with no invalidation logic.
class ResultCache {
public:
    explicit ResultCache(size_t max_bytes);

    bool Get(const std::string &key, std::string &out);
    void Put(const std::string &key, const std::string &value);
    size_t BytesUsed();

private:
    void EvictLocked();

    typedef std::list<std::pair<std::string, std::string> > ListType;

    std::mutex  mutex_;
    size_t      max_bytes_;
    size_t      bytes_;
    ListType    order_;                                        // front = most recent
    std::unordered_map<std::string, ListType::iterator> index_;
};

}  // namespace fdp

#endif
