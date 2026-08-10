#ifndef FDP_TDIPATH_HH
#define FDP_TDIPATH_HH

#include <string>

namespace fdp {

// A decoded intercepted path.
//
// `payload` is the request as MDSplus serialises it -- the output of
// GetMany.serialize(), an APD list of {name, exp, args} dictionaries. We never
// interpret it: it is handed to GetManyExecute($) as an opaque byte array, and
// MDSplus is the only thing that needs to understand its shape. That keeps one
// format definition rather than ours plus theirs.
//
// An empty `tree` means "evaluate without opening a tree".
struct TdiTarget {
    std::string tree;
    long long   shot = 0;
    std::string version;   // token from the path; "-" when no tree is named
    std::string payload;
};

// "00/00/19/00" for shot 190000 -- sprintf("%08lld", shot/100) in digit pairs.
// Mirrors the existing MDSplus archive layout so leaf directories hold ~100
// shots each. Not cosmetic: a flat namespace makes XrdPfc's startup scan take
// hours while holding a global lock on open (xrootd issue #2804).
std::string ShotBucket(long long shot);

// Cheap prefix test used on every Oss call; does not validate the remainder.
bool IsTdiPath(const std::string &lfn, const std::string &prefix);

// Full parse. Returns false for anything malformed; never throws. A payload
// that decodes but is not a valid request is *not* rejected here -- MDSplus
// will reject it, and duplicating that judgement would mean maintaining a
// second opinion about its own format.
bool ParseTdiPath(const std::string &lfn, const std::string &prefix, TdiTarget &out);

// Inverse of ParseTdiPath. Used by tests and by the future client transport,
// which is why it must stay byte-for-byte deterministic: the path is the cache
// key, so any variation is a silent cache miss.
std::string BuildTdiPath(const std::string &prefix, const std::string &tree,
                         long long shot, const std::string &version,
                         const std::string &payload);

// The path grammar always carries a tree segment, but some expressions need no
// tree open at all -- pure computation, or functions like PTDATA2 that take the
// shot as an argument. This sentinel is the segment that denotes that case; it
// is not a legal MDSplus tree name, so it cannot collide with a real one.
extern const char *const kNoTree;   // "-"

// Maximum bytes in one path segment: NAME_MAX(255) minus room for ".cinfo",
// which the cache appends when it materialises the object.
const size_t kMaxSegment = 249;

// Maximum number of payload chunks; beyond this the origin refuses the request.
const size_t kMaxChunks = 8;

}  // namespace fdp

#endif
