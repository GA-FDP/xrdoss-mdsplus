// Scaffold: forwards everything to the wrapped storage system.
// Replaced in full by Task 7. Its only job right now is to prove that a
// conda-built plugin loads into the Pelican origin image's XRootD.
#include "XrdOss/XrdOssWrapper.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdVersion.hh"

XrdVERSIONINFO(XrdOssAddStorageSystem2, XrdOssMdsplus);

namespace {

class PassThrough : public XrdOssWrapper {
public:
    explicit PassThrough(XrdOss &next) : XrdOssWrapper(next) {}
};

}  // namespace

extern "C" {

XrdOss *XrdOssAddStorageSystem2(XrdOss *curr_oss, XrdSysLogger *logger,
                                const char * /*config_fn*/, const char * /*parms*/,
                                XrdOucEnv * /*envP*/) {
    static XrdSysError eDest(logger, "ossmdsplus_");
    eDest.Say("++++++ XrdOssMdsplus scaffold loaded (pass-through only)");
    return new PassThrough(*curr_oss);
}

}
