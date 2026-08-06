#ifndef FDP_EVALCLIENT_HH
#define FDP_EVALCLIENT_HH

#include "Request.hh"

#include <string>

namespace fdp {

// Talks to the out-of-process MDSplus evaluator over a unix socket.
//
// Wire protocol, all integers big-endian:
//   request  : u32 total_len | u16 tree_len | tree | i64 shot | request bytes
//   response : u8 status | u32 len | payload
//
// The request body is already the canonical form from Request::Serialize(), so
// there is no encoding step here — this class only frames bytes it was handed.
//
// Thread-safe: holds no connection state, dials per call.
class EvalClient {
public:
    EvalClient(std::string socket_path, int timeout_ms);

    // Returns true on success and fills `payload` with serialized descriptor
    // bytes. Returns false and fills `error` otherwise. Never throws — this is
    // called from inside an XRootD server thread.
    bool Evaluate(const std::string &tree, long long shot, const Request &request,
                  std::string &payload, std::string &error) const;

private:
    std::string socket_path_;
    int         timeout_ms_;
};

}  // namespace fdp

#endif
