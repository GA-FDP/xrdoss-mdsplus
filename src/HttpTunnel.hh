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

private:
    bool Post(const std::string &action, const std::string &body,
              bool with_session, std::string &out, std::string &error);

    CURL       *curl_;
    std::string url_base_;   // scheme://host:port/prefix
    std::string method_;     // PUT; see the constructor for why
    std::string token_;
    std::string bearer_;
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

}  // namespace fdp

#endif
