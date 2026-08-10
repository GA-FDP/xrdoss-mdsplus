#include "OssMdsplus.hh"

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdVersion.hh"

#include <fcntl.h>       // O_WRONLY, O_RDWR, O_CREAT, O_TRUNC
#include <sys/stat.h>    // S_IFREG, struct stat

#include <algorithm>     // std::min
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

XrdVERSIONINFO(XrdOssAddStorageSystem2, XrdOssMdsplus);

namespace fdp {

namespace {

// Fill a stat buffer for a virtual file of the given size.
void FillStat(struct stat *buf, size_t size) {
    std::memset(buf, 0, sizeof(struct stat));
    buf->st_mode  = S_IFREG | 0444;
    buf->st_nlink = 1;
    buf->st_size  = static_cast<off_t>(size);
    // Results are immutable for a given path, but we have no meaningful
    // timestamp for a generated object; the version segment in the path is
    // what actually distinguishes revisions (spec section 5).
    buf->st_mtime = buf->st_ctime = buf->st_atime = ::time(0);
}

}  // namespace

// ---------------------------------------------------------------- OssMdsplus

OssMdsplus::OssMdsplus(XrdOss &next, XrdSysError &log, const std::string &prefix,
                       const std::string &server, size_t cache_bytes, int timeout_ms,
                       size_t max_result_bytes, const std::string &treepath,
                       const std::string &version_prefix)
    : XrdOssWrapper(next), log_(log), prefix_(prefix),
      version_prefix_(version_prefix),
      client_(server, timeout_ms, max_result_bytes), cache_(cache_bytes),
      versions_(treepath) {}

// A remote client cannot derive the version itself -- the token comes from a
// stat of a tree file it has no access to. So it asks. The answer is small and
// changes whenever the tree does, which is precisely what must NOT be cached;
// callers fetch it with ?directread and the origin marks it no-store.
bool OssMdsplus::ResolveVersionPath(const std::string &lfn, std::string &payload,
                                    std::string &error) {
    // <version_prefix>/<tree>/<d1>/<d2>/<d3>/<d4>/<shot>
    const std::string rest = lfn.substr(version_prefix_.size() + 1);
    std::vector<std::string> p;
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t end = rest.find('/', start);
        p.push_back(rest.substr(start, (end == std::string::npos ? rest.size() : end) - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (p.size() != 6 || p[0].empty()) { error = "malformed version-lookup path"; return false; }

    for (size_t i = 1; i <= 4; ++i) {
        if (p[i].size() != 2) { error = "malformed shot bucket"; return false; }
    }
    for (size_t i = 0; i < p[5].size(); ++i) {
        if (p[5][i] < '0' || p[5][i] > '9') { error = "malformed shot"; return false; }
    }
    if (p[5].empty()) { error = "malformed shot"; return false; }

    const long long shot = std::strtoll(p[5].c_str(), 0, 10);
    if (ShotBucket(shot) != p[1] + "/" + p[2] + "/" + p[3] + "/" + p[4]) {
        error = "shot bucket disagrees with shot";
        return false;
    }

    std::string version;
    if (!versions_.Current(p[0], shot, version, error)) return false;
    payload = version + "\n";
    return true;
}

XrdOssDF *OssMdsplus::newFile(const char *tident) {
    std::unique_ptr<XrdOssDF> next(wrapPI.newFile(tident));
    if (!next) return 0;
    return new FileMdsplus(std::move(next), *this);
}

XrdOssDF *OssMdsplus::newDir(const char *tident) {
    // The tdi namespace is synthetic and not enumerable (spec section 7.5), so
    // directory handles always belong to the wrapped storage system.
    return wrapPI.newDir(tident);
}

bool OssMdsplus::Materialize(const std::string &lfn, std::string &payload,
                             std::string &error) {
    // Version lookups are deliberately NOT cached: their whole purpose is to
    // report a value that changes, and a cached answer would defeat that.
    if (IsTdiPath(lfn, version_prefix_))
        return ResolveVersionPath(lfn, payload, error);

    if (cache_.Get(lfn, payload)) return true;

    TdiTarget target;
    if (!ParseTdiPath(lfn, prefix_, target)) {
        error = "malformed tdi path";
        return false;
    }

    // A request naming no tree has nothing to version.
    if (target.tree.empty()) {
        if (target.version != TreeVersion::kNoVersion) {
            error = "a request with no tree must carry version '-'";
            return false;
        }
    } else {
        // Refuse rather than guess when versions cannot be checked: serving an
        // unverifiable object would let a re-analysed shot be cached forever
        // under a name that no longer describes it.
        std::string current;
        if (!versions_.Current(target.tree, target.shot, current, error)) return false;

        if (target.version != current) {
            // Not an error in the usual sense -- the caller asked for a
            // version that is no longer current. Caches holding the older
            // object are still correct; they hold exactly what that version
            // was. The client should re-resolve and ask again.
            error = "stale version " + target.version + "; current is " + current;
            return false;
        }
    }

    if (!client_.Evaluate(target.tree, target.shot, target.payload, payload, error))
        return false;

    cache_.Put(lfn, payload);
    return true;
}

int OssMdsplus::Stat(const char *path, struct stat *buff, int opts, XrdOucEnv *envP) {
    const std::string lfn(path ? path : "");
    if (!Owns(lfn)) return wrapPI.Stat(path, buff, opts, envP);

    // The size must be known before any bytes are served: the director parses
    // Content-Length with strconv.Atoi and turns a failure into a 404 rather
    // than a redirect. So Stat evaluates, and the cache keeps the payload for
    // the Open/Read that follows.
    std::string payload, error;
    if (!Materialize(lfn, payload, error)) {
        log_.Emsg("Stat", lfn.c_str(), error.c_str());
        return -ENOENT;
    }

    FillStat(buff, payload.size());
    return XrdOssOK;
}

// --------------------------------------------------------------- FileMdsplus

FileMdsplus::FileMdsplus(std::unique_ptr<XrdOssDF> next, OssMdsplus &oss)
    : XrdOssWrapDF(*next), owned_(std::move(next)), oss_(oss), intercepted_(false) {}

int FileMdsplus::Open(const char *path, int Oflag, mode_t Mode, XrdOucEnv &env) {
    const std::string lfn(path ? path : "");
    if (!oss_.Owns(lfn)) return XrdOssWrapDF::Open(path, Oflag, Mode, env);

    if ((Oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0) return -EROFS;

    std::string error;
    if (!oss_.Materialize(lfn, payload_, error)) {
        oss_.Log().Emsg("Open", lfn.c_str(), error.c_str());
        return -ENOENT;
    }
    intercepted_ = true;
    return XrdOssOK;
}

ssize_t FileMdsplus::Read(void *buff, off_t offset, size_t blen) {
    if (!intercepted_) return XrdOssWrapDF::Read(buff, offset, blen);

    if (offset < 0) return -EINVAL;
    if (static_cast<size_t>(offset) >= payload_.size()) return 0;   // EOF

    const size_t n = std::min(blen, payload_.size() - static_cast<size_t>(offset));
    std::memcpy(buff, payload_.data() + offset, n);
    return static_cast<ssize_t>(n);
}

int FileMdsplus::Fstat(struct stat *buf) {
    if (!intercepted_) return XrdOssWrapDF::Fstat(buf);
    FillStat(buf, payload_.size());
    return XrdOssOK;
}

int FileMdsplus::Close(long long *retsz) {
    if (!intercepted_) return XrdOssWrapDF::Close(retsz);
    if (retsz) *retsz = static_cast<long long>(payload_.size());
    payload_.clear();
    intercepted_ = false;
    return XrdOssOK;
}

}  // namespace fdp

// ------------------------------------------------------------- configuration

namespace {

// Parse "key=value" pairs from the ofs.osslib parms string.
std::string ParmValue(const char *parms, const std::string &key,
                      const std::string &dflt) {
    if (!parms) return dflt;
    std::istringstream iss(parms);
    std::string token;
    while (iss >> token) {
        const size_t eq = token.find('=');
        if (eq != std::string::npos && token.substr(0, eq) == key)
            return token.substr(eq + 1);
    }
    return dflt;
}

}  // namespace

extern "C" {

XrdOss *XrdOssAddStorageSystem2(XrdOss *curr_oss, XrdSysLogger *logger,
                                const char * /*config_fn*/, const char *parms,
                                XrdOucEnv * /*envP*/) {
    static XrdSysError eDest(logger, "ossmdsplus_");

    const std::string prefix = ParmValue(parms, "prefix", "/tdi");
    const std::string server = ParmValue(parms, "server", "localhost:8000");
    const size_t cache_bytes =
        std::strtoull(ParmValue(parms, "cache", "268435456").c_str(), 0, 10);
    const int timeout_ms = std::atoi(ParmValue(parms, "timeout", "30000").c_str());
    // Bounds origin memory per request; 0 disables. Default 256 MiB.
    const size_t max_result = std::strtoull(
        ParmValue(parms, "maxresult", "268435456").c_str(), 0, 10);
    // Semicolon-delimited templates for locating a tree's .datafile, used to
    // derive the version token. %T tree, %S shot, %B digit-pair bucket.
    const std::string treepath = ParmValue(parms, "treepath", "");
    const std::string version_prefix = ParmValue(parms, "versionprefix", "/tdi-version");

    eDest.Say("++++++ XrdOssMdsplus initializing; prefix=", prefix.c_str(),
              " server=", server.c_str());

    if (treepath.empty())
        eDest.Say("------ XrdOssMdsplus: no treepath= configured; requests naming "
                  "a tree will be refused because their version cannot be checked");

    return new fdp::OssMdsplus(*curr_oss, eDest, prefix, server, cache_bytes,
                               timeout_ms, max_result, treepath, version_prefix);
}

}
