#include "TdiPath.hh"
#include "Base64Url.hh"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(sep, start);
        if (end == std::string::npos) { parts.push_back(s.substr(start)); break; }
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool AllDigits(const std::string &s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

}  // namespace

namespace fdp {

const char *const kNoTree = "-";

std::string ShotBucket(long long shot) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08lld", shot / 100);
    const std::string d(buf);
    return d.substr(0, 2) + "/" + d.substr(2, 2) + "/" +
           d.substr(4, 2) + "/" + d.substr(6, 2);
}

bool IsTdiPath(const std::string &lfn, const std::string &prefix) {
    if (lfn.size() <= prefix.size()) return false;
    if (lfn.compare(0, prefix.size(), prefix) != 0) return false;
    // The prefix must end at a separator, so "/tdifoo" is not under "/tdi".
    return lfn[prefix.size()] == '/';
}

bool ParseTdiPath(const std::string &lfn, const std::string &prefix, TdiTarget &out) {
    if (!IsTdiPath(lfn, prefix)) return false;

    const std::vector<std::string> p = Split(lfn.substr(prefix.size() + 1), '/');
    // tree, d1, d2, d3, d4, shot, and at least one payload chunk.
    if (p.size() < 7) return false;
    if (p[0].empty()) return false;
    if (p.size() - 6 > kMaxChunks) return false;

    for (size_t i = 1; i <= 4; ++i) {
        if (p[i].size() != 2 || !AllDigits(p[i])) return false;
    }
    if (!AllDigits(p[5])) return false;

    const long long shot = std::strtoll(p[5].c_str(), NULL, 10);
    // The bucket is redundant with the shot; a disagreement means the path was
    // hand-built or corrupted, and accepting it would let one object have two
    // names and so two cache entries.
    if (ShotBucket(shot) != p[1] + "/" + p[2] + "/" + p[3] + "/" + p[4]) return false;

    std::string encoded;
    for (size_t i = 6; i < p.size(); ++i) {
        if (p[i].empty() || p[i].size() > kMaxSegment) return false;
        encoded += p[i];
    }

    std::string raw;
    if (!Base64UrlDecode(encoded, raw)) return false;
    if (raw.empty()) return false;

    // The sentinel maps to an empty tree name, meaning "evaluate without
    // opening a tree".
    out.tree = (p[0] == kNoTree) ? std::string() : p[0];
    out.shot = shot;
    out.payload.swap(raw);
    return true;
}

std::string BuildTdiPath(const std::string &prefix, const std::string &tree,
                         long long shot, const std::string &payload) {
    const std::string enc = Base64UrlEncode(payload);
    char shotbuf[32];
    std::snprintf(shotbuf, sizeof(shotbuf), "%lld", shot);

    const std::string tree_seg = tree.empty() ? std::string(kNoTree) : tree;
    std::string path = prefix + "/" + tree_seg + "/" + ShotBucket(shot) + "/" + shotbuf;
    for (size_t i = 0; i < enc.size(); i += kMaxSegment) {
        path += "/" + enc.substr(i, kMaxSegment);
    }
    return path;
}

}  // namespace fdp
