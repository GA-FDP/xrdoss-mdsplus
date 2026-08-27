#ifndef FDP_MDSIPSESSION_HH
#define FDP_MDSIPSESSION_HH

#include <ctime>
#include <map>
#include <mutex>
#include <string>

namespace fdp {

// Holds one mdsip TCP connection per client session.
//
// This exists because mdsip is STATEFUL: openTree sets the tree context on the
// connection. A relay that opened a fresh mdsip connection per HTTP request
// would lose it, and every subsequent get() would fail -- verified, a stateless
// variant dies immediately with %MDSPLUS-E-ERROR (docs/relay-spike.md).
//
// Consequences worth knowing: sessions are sticky to one origin and cannot be
// load balanced, and an abandoned session holds an mdsip connection until it
// times out.
//
// A retired session is not the client's fault and must not be terminal for it.
// The transport redials and replays the login and tree open (HttpTunnel::Call),
// which is why `timeout_ms` can afford to be generous rather than defensive:
// the cost of a false timeout is a redial, and the cost of a real hang is an
// mdsip process held for that long.
class MdsipSessions {
public:
    MdsipSessions(std::string host, int port, int idle_seconds,
                  size_t max_sessions, int timeout_ms);

    // Closes every still-open mdsip connection. Without this each survivor
    // sits in mdsip's read loop until *its* timeout, holding a process.
    ~MdsipSessions();

    // Opens an mdsip connection and returns its opaque token, or "" on failure.
    std::string Open(std::string &error);

    // Relays a complete mdsip call and returns exactly one answer message.
    // `request` must be whole -- the client transport buffers until a call is
    // complete before POSTing, because an HTTP exchange carries one blob each
    // way and cannot express an open-ended stream.
    //
    // A session carries exactly one call at a time. A second concurrent Relay
    // on the same token fails rather than queueing: two interleaved calls would
    // corrupt the mdsip byte stream, and a real client serialises anyway
    // (MDSplus's Connection is not concurrent). `busy` is set on that refusal
    // so the caller can answer 409 rather than 502.
    bool Relay(const std::string &token, const std::string &request,
               std::string &answer, std::string &error, bool *busy = 0);

    void Close(const std::string &token);

    // Reaps sessions idle longer than the configured window. Called on every
    // request: there is no timer thread, and a relay that never reaped would
    // leak an mdsip process per abandoned client.
    void ReapIdle();

    size_t Count();

private:
    struct Session {
        int    fd;
        time_t last_used;
        bool   busy;     // a Relay is mid-flight, holding fd outside the lock
        bool   doomed;   // close requested while busy; the relayer cleans up
    };

    // Caller must hold mutex_. Closes fd and erases, or defers to the in-flight
    // relayer if one holds the fd.
    void Retire(const std::map<std::string, Session>::iterator &it);

    std::string host_;
    int         port_;
    int         idle_seconds_;
    size_t      max_sessions_;
    int         timeout_ms_;

    std::mutex                         mutex_;
    std::map<std::string, Session>     sessions_;
};

// Reads exactly one mdsip message: a 48-byte header, then msglen-48 more.
// Exposed for testing.
bool ReadOneMessage(int fd, std::string &out, int timeout_ms);

// Total length declared by an mdsip message header, or 0 if `hdr` is shorter
// than a header or declares something implausible.
size_t MdsipMessageLength(const std::string &hdr);

// Bytes in an mdsip message header.
const size_t kMdsipHeaderBytes = 48;

}  // namespace fdp

#endif
