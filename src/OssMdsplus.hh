#ifndef FDP_OSSMDSPLUS_HH
#define FDP_OSSMDSPLUS_HH

#include "MdsIpClient.hh"
#include "TreeVersion.hh"
#include "ResultCache.hh"
#include "TdiPath.hh"

#include "XrdOss/XrdOssWrapper.hh"
#include "XrdSys/XrdSysError.hh"

#include <memory>
#include <string>

namespace fdp {

// Stacked storage system: intercepts paths under a configured prefix and
// serves them as MDSplus TDI evaluation results; forwards everything else
// untouched to the storage system it wraps.
//
// Loaded with "ofs.osslib ++", so the wrapped system is Pelican's own stack
// (default -> stats -> us). Only the read path is implemented for intercepted
// paths; mutating operations return -EROFS.
class OssMdsplus : public XrdOssWrapper {
public:
    OssMdsplus(XrdOss &next, XrdSysError &log, const std::string &prefix,
               const std::string &server, size_t cache_bytes, int timeout_ms,
               size_t max_result_bytes, const std::string &treepath);

    XrdOssDF *newFile(const char *tident);
    XrdOssDF *newDir(const char *tident);
    int Stat(const char *path, struct stat *buff, int opts = 0, XrdOucEnv *envP = 0);

    // Evaluate (or return a cached payload) for an intercepted path.
    bool Materialize(const std::string &lfn, std::string &payload, std::string &error);

    bool Owns(const std::string &lfn) const { return IsTdiPath(lfn, prefix_); }
    XrdSysError &Log() { return log_; }

private:
    XrdSysError &log_;
    std::string  prefix_;
    MdsIpClient  client_;
    ResultCache  cache_;
    TreeVersion  versions_;
};

// File handle for an intercepted path. Owns the wrapped handle: XrdOssWrapDF
// documents that "the object creator is responsible for deleting the df2Wrap
// object".
class FileMdsplus : public XrdOssWrapDF {
public:
    FileMdsplus(std::unique_ptr<XrdOssDF> next, OssMdsplus &oss);

    int     Open(const char *path, int Oflag, mode_t Mode, XrdOucEnv &env);
    ssize_t Read(void *buff, off_t offset, size_t blen);
    int     Fstat(struct stat *buf);
    int     Close(long long *retsz = 0);

    // Our payload lives in memory, so there is no descriptor to sendfile from.
    // Returning -1 keeps XRootD on the Read() path for intercepted files while
    // leaving pass-through files free to use sendfile.
    int     getFD() { return intercepted_ ? -1 : XrdOssWrapDF::getFD(); }

private:
    std::unique_ptr<XrdOssDF> owned_;   // keeps the wrapped handle alive
    OssMdsplus &oss_;
    bool        intercepted_;
    std::string payload_;
};

}  // namespace fdp

#endif
