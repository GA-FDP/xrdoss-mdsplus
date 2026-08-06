#ifndef FDP_TDIPATH_HH
#define FDP_TDIPATH_HH

#include "Request.hh"

#include <string>

namespace fdp {

// A fully decoded intercepted path: which tree and shot, and what to evaluate.
struct TdiTarget {
    std::string tree;
    long long   shot = 0;
    Request     request;
};

// "00/00/19/00" for shot 190000 — sprintf("%08lld", shot/100) in digit pairs.
// Mirrors the existing MDSplus archive layout so leaf directories hold ~100
// shots each. Not cosmetic: a flat namespace makes XrdPfc's startup scan take
// hours while holding a global lock on open (xrootd issue #2804).
std::string ShotBucket(long long shot);

// Cheap prefix test used on every Oss call; does not validate the remainder.
bool IsTdiPath(const std::string &lfn, const std::string &prefix);

// Full parse. Returns false for anything malformed; never throws.
bool ParseTdiPath(const std::string &lfn, const std::string &prefix, TdiTarget &out);

// Inverse of ParseTdiPath. Also used by tests and the future client transport,
// which is why it must stay byte-for-byte deterministic: the path is the cache
// key, so any variation is a silent cache miss.
std::string BuildTdiPath(const std::string &prefix, const std::string &tree,
                         long long shot, const Request &request);

// Maximum bytes in one path segment: NAME_MAX(255) minus room for ".cinfo",
// which the cache appends when it materialises the object.
const size_t kMaxSegment = 249;

// Maximum number of payload chunks; beyond this the origin returns 414.
const size_t kMaxChunks = 8;

}  // namespace fdp

#endif
