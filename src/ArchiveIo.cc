#include "ArchiveIo.hh"

#include <utility>

namespace fdp {
namespace {

std::string TrimTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

ArchiveIoProvider::ArchiveIoProvider(std::string url_prefix, std::string local_root)
    : url_prefix_(TrimTrailingSlash(std::move(url_prefix))),
      local_root_(TrimTrailingSlash(std::move(local_root))) {}

std::string ArchiveIoProvider::Rewrite(const std::string &path) const {
    if (url_prefix_.empty() || local_root_.empty()) return path;
    if (path.size() < url_prefix_.size()) return path;
    if (path.compare(0, url_prefix_.size(), url_prefix_) != 0) return path;

    // The prefix must end at a separator, or ".../archives" also claims
    // ".../archives-private" and silently serves a different namespace. Same
    // trap the relay's MatchesPath documents for HTTP paths, and the same fix.
    const char after =
        path.size() > url_prefix_.size() ? path[url_prefix_.size()] : '\0';
    if (after != '/' && after != '\0') return path;

    return local_root_ + path.substr(url_prefix_.size());
}

std::unique_ptr<ptdata::IoHandle> ArchiveIoProvider::open(const std::string &path,
                                                          int flags) {
    return local_.open(Rewrite(path), flags);
}

bool ArchiveIoProvider::stat(const std::string &path) {
    return local_.stat(Rewrite(path));
}

std::string ArchiveIoProvider::resolve(const std::string &path) {
    return local_.resolve(Rewrite(path));
}

std::vector<std::string> ArchiveIoProvider::list_dirs(const std::string &path) {
    return local_.list_dirs(Rewrite(path));
}

std::vector<std::string> ArchiveIoProvider::list_files(const std::string &path) {
    return local_.list_files(Rewrite(path));
}

}  // namespace fdp
