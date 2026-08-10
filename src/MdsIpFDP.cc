// libMdsIpFDP.so -- an MDSplus thin-client transport that reaches FDP.
//
// Existing DIII-D code changes one string and nothing else:
//
//     conn = MDSplus.Connection('fdp://d3d-origin.gat.com:8443/mdsip')
//     conn.openTree('efit01', 190000)
//     conn.get(r'\ipmhd')
//
// MDSplus finds this library itself: parse_host() splits <scheme>://<host>, and
// LoadIo() uppercases the scheme, builds the image name "MdsIp" + SCHEME, and
// resolves the symbol Io (mdstcpip/mdsipshr/LoadIo.c:41-62). So the file must
// be named libMdsIpFDP.so exactly -- no version suffix, unlike the XRootD
// plugins in this repo, which is the opposite convention.
//
// The work is turning a byte stream into request/response. IoRoutines is a
// byte-stream vtable, but an HTTP exchange carries one blob each way, so send()
// buffers until a call is complete (CallAssembler), POSTs it, and holds the
// answer for recv() to drain. Nothing is interpreted or fabricated: a real
// mdsip server behind the relay produces every byte returned.

#include "HttpTunnel.hh"
#include "MdsipCall.hh"

#include "mdsip_io.h"

#include <cstring>
#include <string>

namespace {

// Per-connection state. A pointer to this is stashed via ConnectionSetInfo,
// which copies the pointer value, so the object itself is owned here and freed
// in disconnect.
struct FdpSession {
    fdp::HttpTunnel    tunnel;
    fdp::CallAssembler outgoing;
    std::string        answer;    // the reply being drained by recv()
    size_t             answer_off;
    std::string        last_error;

    FdpSession() : answer_off(0) {}
};

FdpSession *SessionOf(Connection *c) {
    if (!c) return 0;
    char *name = 0;
    FdpSocket fd = -1;
    size_t len = 0;
    void *info = ConnectionGetInfo(c, &name, &fd, &len);
    if (!info || len != sizeof(FdpSession *)) return 0;
    return *static_cast<FdpSession **>(info);
}

int fdp_connect(Connection *c, char * /*protocol*/, char *connectString) {
    if (!connectString) return -1;

    FdpSession *s = new FdpSession();
    std::string error;
    if (!s->tunnel.Open(connectString, error)) {
        // Nothing here can reach the user's exception, so put it where a
        // confused caller will actually look.
        std::fprintf(stderr, "fdp transport: %s\n", error.c_str());
        delete s;
        return -1;
    }

    FdpSession *ptr = s;
    // readfd = -1: there is no socket to select on. MDSplus only uses readfd
    // for its own event/select paths, which this transport does not support.
    ConnectionSetInfo(c, const_cast<char *>("fdp"), -1, &ptr, sizeof(ptr));
    return 0;
}

ssize_t fdp_send(Connection *c, const void *buffer, size_t buflen,
                 int /*nowait*/) {
    FdpSession *s = SessionOf(c);
    if (!s) return -1;

    const fdp::CallAssembler::Status st =
        s->outgoing.Append(static_cast<const char *>(buffer), buflen);

    if (st == fdp::CallAssembler::kMalformed) {
        std::fprintf(stderr, "fdp transport: malformed outgoing mdsip stream\n");
        return -1;
    }
    if (st != fdp::CallAssembler::kComplete)
        return static_cast<ssize_t>(buflen);   // still accumulating

    // A complete call: one round trip, and the answer waits for recv().
    const std::string call = s->outgoing.Take();
    std::string answer, error;
    if (!s->tunnel.Call(call, answer, error)) {
        std::fprintf(stderr, "fdp transport: %s\n", error.c_str());
        return -1;
    }

    s->answer.swap(answer);
    s->answer_off = 0;
    return static_cast<ssize_t>(buflen);
}

ssize_t fdp_recv(Connection *c, void *buffer, size_t buflen) {
    FdpSession *s = SessionOf(c);
    if (!s) return -1;

    const size_t left = s->answer.size() - s->answer_off;
    if (left == 0) {
        // MDSplus asking for more than the answer holds means the two sides
        // disagree about framing. Returning 0 would read as a clean EOF and
        // hang the caller; -1 surfaces it.
        std::fprintf(stderr, "fdp transport: no buffered answer to read\n");
        return -1;
    }

    const size_t n = buflen < left ? buflen : left;
    std::memcpy(buffer, s->answer.data() + s->answer_off, n);
    s->answer_off += n;
    if (s->answer_off == s->answer.size()) {
        s->answer.clear();
        s->answer_off = 0;
    }
    return static_cast<ssize_t>(n);
}

ssize_t fdp_recv_to(Connection *c, void *buffer, size_t len, int /*to_msec*/) {
    // The answer is already in hand by the time recv is called, so there is
    // nothing to wait for and the timeout is vacuous. Implemented anyway
    // because GetMdsMsg prefers recv_to when a timeout is requested.
    return fdp_recv(c, buffer, len);
}

int fdp_disconnect(Connection *c) {
    FdpSession *s = SessionOf(c);
    if (!s) return 0;
    s->tunnel.Close();
    delete s;
    // Clear the stashed pointer so a double disconnect cannot reach freed
    // memory. MDSplus frees its own copy of the info block.
    FdpSession *null_ptr = 0;
    ConnectionSetInfo(c, const_cast<char *>("fdp"), -1, &null_ptr,
                      sizeof(null_ptr));
    return 0;
}

// flush, listen, authorize, reuseCheck and check are NULL: this is a
// client-only transport, and MDSplus treats every one of them as optional --
// the in-tree GSI routines leave four of them NULL the same way.
//
// reuseCheck NULL in particular means connections are never silently shared.
// Sharing would be wrong here: the relay keys server-side state to a session
// token, so two callers on one session would interleave into a byte stream
// that cannot survive it.
IoRoutines fdp_routines = {
    fdp_connect,   // connect
    fdp_send,      // send
    fdp_recv,      // recv
    0,             // flush
    0,             // listen
    0,             // authorize
    0,             // reuseCheck
    fdp_disconnect,// disconnect
    fdp_recv_to,   // recv_to
    0              // check
};

}  // namespace

extern "C" __attribute__((visibility("default"))) IoRoutines *Io() {
    return &fdp_routines;
}
