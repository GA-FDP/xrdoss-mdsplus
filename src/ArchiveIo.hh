#pragma once

#include <ptdata/io_provider.h>

#include <memory>
#include <string>
#include <vector>

namespace fdp {

// An IoProvider that maps the absolute pelican:// URLs recorded in the ptdata
// JSON index onto the origin's local archive, then delegates to
// LocalIoProvider.
//
// The mdsip sandbox solves the same problem without code: the recorded URL does
// not begin with '/', so it is a relative path, and POSIX collapses its double
// slash -- with the working directory at '/' it resolves through a symlink
// chain the image provides. That works because the sandbox launches its own
// process and can cd '/'. This handler is loaded into Pelican's xrootd, whose
// working directory is not ours to set and must not be changed underneath it,
// and a wrong cwd fails as "file not found" with nothing naming the cause.
//
// Rewriting explicitly removes that dependency, and moves the federation
// hostname out of a directory name into configuration an operator can see.
//
// NOTE: the rewrite belongs in open() and stat(). IoProvider::resolve() looks
// like the hook and is not -- ShotLocator calls stat() and hands the same
// string to ShotFile, and nothing on that path calls resolve() at all, so
// overriding only resolve() would compile and silently do nothing.
class ArchiveIoProvider : public ptdata::IoProvider {
public:
    // url_prefix: e.g. "pelican://osg-htc.org:443/fdp-d3d/archives"
    // local_root: e.g. "/fdp-archives"
    // Either empty disables rewriting; paths then pass through unchanged.
    ArchiveIoProvider(std::string url_prefix, std::string local_root);

    std::unique_ptr<ptdata::IoHandle> open(const std::string &path, int flags) override;
    bool stat(const std::string &path) override;
    std::string resolve(const std::string &path) override;
    std::vector<std::string> list_dirs(const std::string &path) override;
    std::vector<std::string> list_files(const std::string &path) override;

    // The path actually handed to the filesystem. Public because it is the
    // whole behaviour worth testing.
    std::string Rewrite(const std::string &path) const;

private:
    std::string url_prefix_;   // no trailing slash
    std::string local_root_;   // no trailing slash
    ptdata::LocalIoProvider local_;
};

}  // namespace fdp
