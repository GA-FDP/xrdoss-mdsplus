#ifndef FDP_MDSIPCLIENT_HH
#define FDP_MDSIPCLIENT_HH

#include <string>

namespace fdp {

// Talks to a local mdsip server using MdsIpShr.
//
// Only the mdsip *client* library is linked -- no MDSplus object model, no
// treeshr, no TDI evaluation in this process. The reason for keeping MDSplus
// out of XRootD was that treeshr opens files, holds state, and can hang or
// crash; a socket protocol client does none of that.
//
// The request is MDSplus's own serialized APD list and is passed through
// untouched to GetManyExecute($). That function returns bytes that are
// ALREADY serialized (mdsobjects/cpp/mdsdata.c:921 calls MdsSerializeDscOut
// before returning), so the answer is verbatim the payload we serve.
//
// Thread-safe: holds no connection state, dials per call.
class MdsIpClient {
public:
    MdsIpClient(std::string server, int timeout_ms);

    // Returns true on success and fills `payload` with serialized result
    // bytes. Returns false and fills `error` otherwise. Never throws -- this
    // runs on an XRootD server thread.
    bool Evaluate(const std::string &tree, long long shot,
                  const std::string &request,
                  std::string &payload, std::string &error) const;

private:
    std::string server_;
    int         timeout_ms_;
};

}  // namespace fdp

#endif
