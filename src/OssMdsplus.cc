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
                       const std::string &server, size_t cache_bytes, int timeout_ms)
    : XrdOssWrapper(next), log_(log), prefix_(prefix),
      client_(server, timeout_ms), cache_(cache_bytes) {}

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
    if (cache_.Get(lfn, payload)) return true;

    TdiTarget target;
    if (!ParseTdiPath(lfn, prefix_, target)) {
        error = "malformed tdi path";
        return false;
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

    eDest.Say("++++++ XrdOssMdsplus initializing; prefix=", prefix.c_str(),
              " server=", server.c_str());

    return new fdp::OssMdsplus(*curr_oss, eDest, prefix, server, cache_bytes, timeout_ms);
}

}
