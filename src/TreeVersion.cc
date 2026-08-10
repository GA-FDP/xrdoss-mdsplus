#include "TreeVersion.hh"
#include "TdiPath.hh"

#include <sys/stat.h>

#include <cstdio>
#include <cstdint>

namespace fdp {

const char *const TreeVersion::kNoVersion = "-";

namespace {

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(sep, start);
        if (end == std::string::npos) {
            if (start < s.size()) parts.push_back(s.substr(start));
            break;
        }
        if (end > start) parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// FNV-1a, 64 bit. Deliberately not a cryptographic hash: this detects change,
// it does not resist forgery. A forged token cannot yield anyone else's data --
// the plugin compares against the token it computes itself and serves nothing
// on a mismatch -- so the only property needed is that a changed file almost
// certainly changes the token.
uint64_t Fnv1a(const void *data, size_t len, uint64_t h = 1469598103934665603ULL) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

TreeVersion::TreeVersion(const std::string &search) : templates_(Split(search, ';')) {}

std::string TreeVersion::Expand(const std::string &tmpl, const std::string &tree,
                                long long shot) {
    char shotbuf[32];
    std::snprintf(shotbuf, sizeof(shotbuf), "%lld", shot);
    const std::string bucket = ShotBucket(shot);

    std::string out;
    out.reserve(tmpl.size() + 32);
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%' || i + 1 >= tmpl.size()) { out += tmpl[i]; continue; }
        switch (tmpl[i + 1]) {
            case 'T': out += tree;   ++i; break;
            case 'S': out += shotbuf; ++i; break;
            case 'B': out += bucket; ++i; break;
            case '%': out += '%';    ++i; break;
            default:  out += tmpl[i];      break;
        }
    }
    return out;
}

bool TreeVersion::Current(const std::string &tree, long long shot,
                          std::string &version, std::string &error) const {
    version.clear();
    error.clear();

    if (templates_.empty()) {
        error = "no treepath configured, so versions cannot be checked";
        return false;
    }

    for (size_t i = 0; i < templates_.size(); ++i) {
        const std::string path = Expand(templates_[i], tree, shot);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;

        // Identity, size and mtime. Size and mtime alone can both be preserved
        // by a careless copy; the inode makes that far less likely to slip
        // through. No file content is read.
        const uint64_t ino  = static_cast<uint64_t>(st.st_ino);
        const uint64_t size = static_cast<uint64_t>(st.st_size);
        const uint64_t mt   = static_cast<uint64_t>(st.st_mtime);

        uint64_t h = Fnv1a(&ino,  sizeof(ino));
        h = Fnv1a(&size, sizeof(size), h);
        h = Fnv1a(&mt,   sizeof(mt),   h);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "v%016llx",
                      static_cast<unsigned long long>(h));
        version = buf;
        return true;
    }

    error = "no tree file found for " + tree + " shot " + std::to_string(shot);
    return false;
}

}  // namespace fdp
