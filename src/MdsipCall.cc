#include "MdsipCall.hh"

#include <cstring>

namespace fdp {

namespace {

// A verbatim mirror of MsgHdr (mdstcpip/mdsip_connections.h). Its only purpose
// is the static_asserts below: transcribing offsets by hand is how a header
// gets mis-parsed silently, so let the compiler check the arithmetic against
// the real layout instead of trusting it.
const int kMaxDims = 8;
struct MsgHdrMirror {
    int           msglen;
    int           status;
    short         length;
    unsigned char nargs;
    unsigned char descriptor_idx;
    unsigned char message_id;
    unsigned char dtype;
    signed char   client_type;
    unsigned char ndims;
    int           dims[kMaxDims];
};

static_assert(sizeof(MsgHdrMirror) == kHeaderBytes,
              "MsgHdr is 48 bytes on the wire");
static_assert(offsetof(MsgHdrMirror, msglen) == kMsgLenOffset, "msglen offset");
static_assert(offsetof(MsgHdrMirror, length) == kLengthOffset, "length offset");
static_assert(offsetof(MsgHdrMirror, nargs) == kNargsOffset, "nargs offset");
static_assert(offsetof(MsgHdrMirror, dtype) == kDtypeOffset, "dtype offset");
static_assert(offsetof(MsgHdrMirror, descriptor_idx) == kDescriptorIdxOffset,
              "descriptor_idx offset");

unsigned int MsgLen(const char *hdr) {
    unsigned int len;
    std::memcpy(&len, hdr + kMsgLenOffset, sizeof(len));
    return len;
}

}  // namespace

CallAssembler::CallAssembler() : scanned_(0), state_(kNeedMore) {}

void CallAssembler::Reset() {
    buf_.clear();
    scanned_ = 0;
    state_ = kNeedMore;
}

CallAssembler::Status CallAssembler::Append(const char *data, size_t len) {
    if (state_ == kMalformed) return kMalformed;
    if (data && len) buf_.append(data, len);

    // Walk only the messages not yet accounted for. scanned_ is what makes
    // this safe to call on every chunk: a 4 MB call fed in 8 KB pieces would
    // otherwise be rescanned from the start several hundred times.
    while (state_ == kNeedMore && buf_.size() - scanned_ >= kHeaderBytes) {
        const char *hdr = buf_.data() + scanned_;
        const unsigned int msglen = MsgLen(hdr);

        // A message shorter than its own header cannot be real, and trusting
        // it would make scanned_ stand still or run backwards.
        if (msglen < kHeaderBytes) { state_ = kMalformed; return kMalformed; }

        if (buf_.size() - scanned_ < msglen) break;   // payload still arriving

        const unsigned char nargs = static_cast<unsigned char>(hdr[kNargsOffset]);
        const unsigned char idx =
            static_cast<unsigned char>(hdr[kDescriptorIdxOffset]);

        scanned_ += msglen;

        // int arithmetic on purpose: nargs is unsigned char, so nargs - 1
        // would wrap to 255 for a zero-argument call and never match.
        if (static_cast<int>(idx) >= static_cast<int>(nargs) - 1)
            state_ = kComplete;
    }
    return state_;
}

std::string CallAssembler::Take() {
    // Only the completed call is returned. Anything after it belongs to the
    // next call -- MDSplus does not pipeline today, but dropping those bytes
    // would be a silent truncation rather than a visible error if it ever does.
    std::string call = buf_.substr(0, scanned_);
    std::string rest = buf_.substr(scanned_);
    buf_.swap(rest);
    scanned_ = 0;
    state_ = kNeedMore;
    if (!buf_.empty()) {
        const std::string carry(buf_);
        buf_.clear();
        Append(carry.data(), carry.size());
    }
    return call;
}

}  // namespace fdp
