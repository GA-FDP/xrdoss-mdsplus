#ifndef FDP_HTTPTUNNEL_HH
#define FDP_HTTPTUNNEL_HH

#include <string>

typedef void CURL;

namespace fdp {

// Client half of the mdsip tunnel: three POSTs against the origin's
// XrdHttpMdsip handler (src/HttpRelay.cc).
//
//   /connect -> a session token       /msg -> one call up, one answer down
//   /close   -> release it
//
// One HttpTunnel is one mdsip session. The curl handle is reused across calls
// so the TLS handshake is paid once rather than per signal -- which matters,
// because the workload this exists for is thousands of one-signal get() calls.
//
// A session can die under a caller that has done nothing wrong: the relay
// retires one whenever a call fails, and reaps one that has been idle too
// long. Call() therefore redials and retries once rather than leaving the
// tunnel permanently broken -- see the comment on Recoverable() for what that
// can and cannot restore.
class HttpTunnel {
public:
    HttpTunnel();
    ~HttpTunnel();

    // `target` is what MDSplus hands the transport after stripping "fdp://":
    //   host[:port][/prefix]     e.g. d3d-origin.gat.com:8443/mdsip
    // Port defaults to 443 and prefix to /mdsip.
    bool Open(const std::string &target, std::string &error);

    bool Call(const std::string &request, std::string &answer, std::string &error);

    void Close();

    bool IsOpen() const { return !token_.empty(); }

    // Whether a lost session could still be recovered from, for tests and for
    // the diagnostic the transport prints. False once a call has been made
    // whose effect on the server a fresh session could not be given -- see
    // CallEffect.
    bool Recoverable() const { return replayable_; }

    // How many times this tunnel has silently redialled. Zero in the ordinary
    // case; a test that expects recovery has to be able to tell "recovered"
    // from "never lost the session in the first place".
    int Redials() const { return redials_; }

private:
    // Returns true on HTTP 200. `status` is the HTTP code (0 if the request
    // never got that far) and `reason` the trimmed response body, so a caller
    // can tell a retired session from an unreachable origin instead of
    // pattern-matching a prose error string.
    bool Post(const std::string &action, const std::string &body,
              bool with_session, std::string &out, std::string &error,
              long *status = 0, std::string *reason = 0);

    // Drops the dead token, re-reads the bearer and asks for a new session.
    // The bearer is deliberately re-read rather than reused: FDP access tokens
    // outlive far less than a long pipeline does, and /connect is the only
    // point at which the relay checks one.
    bool Redial(std::string &error);

    // Files a completed call under what a fresh session would need to be told
    // about it. Called only after the call succeeded -- a call that failed had
    // no effect to remember.
    void Remember(const std::string &request);

    // Replays onto a freshly dialled session everything the old one had been
    // told. Order matters: mdsip will not answer anything before the login.
    bool Replay(std::string &error);

    CURL       *curl_;
    std::string url_base_;   // scheme://host:port/prefix
    std::string method_;     // PUT; see the constructor for why
    std::string token_;
    std::string bearer_;

    // The mdsip login. MDSplus sends it as the first call on a connection --
    // the relay only opens a TCP socket, it does not speak the protocol -- so
    // a session that has not been given it is one mdsip answers nothing on.
    // Discovered the hard way: a redial that replayed only the tree open got
    // "mdsip did not answer" the moment it tried.
    std::string login_;
    std::string tree_open_;  // the tree-context call to replay after a redial
    bool        replayable_;
    int         redials_;
};

// Splits `host[:port][/prefix]` into a full URL base. Exposed for testing:
// getting this wrong sends calls to a URL that 404s, and the resulting MDSplus
// error says nothing about why.
std::string BuildUrlBase(const std::string &target);

// BEARER_TOKEN, then ~/.fdp/token -- the same precedence the fdp CLI uses.
// Empty when neither exists, in which case no Authorization header is sent
// (correct for an unauthenticated local relay, and a clean 401 against a real
// origin rather than a confusing one).
std::string FindBearerToken();

// The expression text of an mdsip call: descriptor 0, which MDSplus always
// sends as a DTYPE_T string (Connection.get is _SendArg(conid, 0, 14, ...)).
// Empty when the call is malformed, truncated, or leads with something other
// than a string -- in which case the caller must assume the worst about it.
std::string CallExpression(const std::string &call);

// What a call did to the session, and therefore what a redial owes it.
enum CallEffect {
    kNoEffect,        // a fresh session would answer it identically
    kOpensTree,       // sets the tree context; replay it after a redial
    kClosesTree,      // drops the tree context; a fresh session has none anyway
    kUnknownEffect    // may have left state we cannot reconstruct: do not redial
};

// Classifies a call by its expression.
//
// mdsip is stateful, and only some of that state is reconstructible. The tree
// context is: MDSplus sets it with TreeOpen($,$) and friends, so replaying
// that one call restores it exactly. TDI's private variables are not -- after
// `get("_sig = ...")` a fresh session answers `get("_sig")` with something
// different, and quietly returning that would be worse than the dead tunnel
// this recovery exists to avoid. Anything holding an `=` that is not a
// comparison is therefore treated as unreconstructible, which errs toward
// today's behaviour (fail) rather than toward a wrong answer.
CallEffect EffectOf(const std::string &expression);

}  // namespace fdp

#endif
