#ifndef FDP_MDSIPCALL_HH
#define FDP_MDSIPCALL_HH

#include <cstddef>
#include <string>

namespace fdp {

// Reassembles a complete mdsip call from a byte stream.
//
// This exists because IoRoutines is a BYTE-STREAM vtable -- send() receives
// whatever chunk MDSplus felt like writing, which is not one message and is not
// one call. An HTTP request carries one blob, so the transport must know when a
// call is whole before it can send anything at all.
//
// A call is nargs messages, each a 48-byte header plus payload, and the last
// one is identified by descriptor_idx == nargs - 1. Everything else here is
// arithmetic on that.
class CallAssembler {
public:
    enum Status {
        kNeedMore,    // a call is in progress; feed more bytes
        kComplete,    // Take() now returns a whole call
        kMalformed    // the stream cannot be a valid call; the caller must fail
    };

    CallAssembler();

    // Feeds bytes as they arrive. Chunks may split a header or a payload
    // anywhere, including mid-field, so this must never assume alignment with
    // message boundaries.
    Status Append(const char *data, size_t len);

    // Moves out the completed call and resets for the next one. Only meaningful
    // after kComplete.
    std::string Take();

    void Reset();

    size_t Buffered() const { return buf_.size(); }

private:
    std::string buf_;
    size_t      scanned_;   // bytes belonging to messages already accounted for
    Status      state_;
};

// Bytes in an mdsip message header.
const size_t kHeaderBytes = 48;

// Field offsets within MsgHdr, from mdstcpip/mdsip_connections.h:
//
//   int  msglen;  int status;  short length;
//   unsigned char nargs; unsigned char descriptor_idx; ...
//
// Exposed for testing; MdsipCall.cc static_asserts them against a mirror of
// that struct, because a wrong offset here would silently mis-frame rather
// than fail.
const size_t kMsgLenOffset = 0;
const size_t kNargsOffset = 10;
const size_t kDescriptorIdxOffset = 11;

}  // namespace fdp

#endif
