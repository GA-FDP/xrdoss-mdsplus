// XrdHttpExtHandler that tunnels mdsip over the origin's existing HTTPS port.
//
// Why a tunnel rather than the cacheable virtual-file path: real client code
// calls conn.get() one signal at a time (55 call sites locally against 0 for
// getMany), and translating that into object GETs would mean parsing the mdsip
// protocol, building batch payloads, and fabricating replies -- where a mistake
// yields plausible wrong data rather than an error. Relaying instead gives full
// protocol compatibility for free. The cost is caching: a cache keys on a URL
// and a tunnel has none. See docs/relay-spike.md.
//
// Wire protocol -- three POSTs, all under one prefix:
//
//   POST <prefix>/connect          -> 200, body is a session token
//   POST <prefix>/msg              -> 200, body is exactly one mdsip answer
//        X-Fdp-Session: <token>       request body is one COMPLETE mdsip call
//   POST <prefix>/close            -> 200
//        X-Fdp-Session: <token>
//
// The client must POST a complete call (all nargs messages) because an HTTP
// exchange carries one blob each way; it cannot express an open-ended stream.

#include "MdsipSession.hh"

#include "XrdHttp/XrdHttpExtHandler.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdVersion.hh"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

XrdVERSIONINFO(XrdHttpGetExtHandler, XrdHttpMdsip);

namespace {

const char *const kSessionHeader = "x-fdp-session";

// Ask BuffgetData for no more than this at a time. The HTTP read buffer is
// 1 MB (XrdHttpProtocol.cc: BPool->Obtain(1024*1024)), and requesting more than
// it can supply makes getDataOneShot return "no space left", which BuffgetData
// reports as 0 -- indistinguishable from a dead connection. Staying well under
// the buffer keeps 0 meaning what it looks like it means.
const int kBodyChunk = 64 * 1024;

// `http.exthandler <name> <lib> <parms>` takes parms as a single GetWord()
// token, so it cannot contain spaces -- comma is the usable separator. Both are
// accepted here since the directive's own docs call it "a free string".
std::string ParmValue(const char *parms, const std::string &key,
                      const std::string &dflt) {
    if (!parms) return dflt;
    std::string text(parms);
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == ',') text[i] = ' ';

    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        const size_t eq = token.find('=');
        if (eq != std::string::npos && token.substr(0, eq) == key)
            return token.substr(eq + 1);
    }
    return dflt;
}

// XrdHttp lowercases header names; look the value up defensively either way.
std::string HeaderValue(std::map<std::string, std::string> &headers,
                        const std::string &lower_name) {
    std::map<std::string, std::string>::iterator it = headers.find(lower_name);
    if (it != headers.end()) return it->second;
    for (it = headers.begin(); it != headers.end(); ++it) {
        std::string k = it->first;
        for (size_t i = 0; i < k.size(); ++i)
            k[i] = static_cast<char>(::tolower(k[i]));
        if (k == lower_name) return it->second;
    }
    return std::string();
}

class HttpMdsipRelay : public XrdHttpExtHandler {
public:
    HttpMdsipRelay(XrdSysError *log, const std::string &prefix,
                   const std::string &host, int port, int idle,
                   size_t max_sessions, int timeout_ms)
        : log_(log), prefix_(prefix),
          sessions_(host, port, idle, max_sessions, timeout_ms) {}

    bool MatchesPath(const char *verb, const char *path) {
        if (!path || !verb) return false;

        // POST and PUT, never GET. GET would shadow the object namespace the
        // Oss plugin serves, and this is an RPC channel rather than a resource.
        //
        // PUT is here for the federation: the Pelican director refuses to route
        // POST (404 at the namespace path, 405 at the API endpoint -- its own
        // CORS header advertises only GET, PUT, OPTIONS, PROPFIND), but routes
        // PUT with a 307 that preserves method and body. Measured in
        // tests/fed/probe_federation_post.sh. Without PUT, clients cannot reach
        // the relay through a federation URL at all and must address an origin
        // directly.
        //
        // Claiming PUT across the whole prefix also means no PUT under it ever
        // reaches storage -- which is what makes it safe to give the namespace
        // the Writes capability the director requires before it will route PUT.
        if (std::strcmp(verb, "POST") != 0 && std::strcmp(verb, "PUT") != 0)
            return false;
        return std::strncmp(path, prefix_.c_str(), prefix_.size()) == 0;
    }

    int Init(const char *) { return 0; }

    int ProcessReq(XrdHttpExtReq &req) {
        sessions_.ReapIdle();

        const std::string action = req.resource.substr(
            req.resource.size() >= prefix_.size() ? prefix_.size() : 0);

        if (action == "/connect") return DoConnect(req);
        if (action == "/msg")     return DoMsg(req);
        if (action == "/close")   return DoClose(req);

        return req.SendSimpleResp(404, "Not Found", 0,
                                  "unknown relay action\n", 21);
    }

private:
    int Fail(XrdHttpExtReq &req, int code, const char *desc,
             const std::string &msg) {
        log_->Emsg("relay", desc, msg.c_str());
        const std::string body = msg + "\n";
        return req.SendSimpleResp(code, desc, 0, body.c_str(),
                                  static_cast<long long>(body.size()));
    }

    // Drain the request body. BuffgetData hands back whatever has arrived and
    // never more than one ring-buffer segment, so this loops until
    // Content-Length is satisfied rather than assuming one read suffices -- a
    // 4.3 MB result's matching request is small, but a large put() would not be.
    bool ReadBody(XrdHttpExtReq &req, std::string &body) {
        body.clear();
        long long remaining = req.length;
        if (remaining <= 0) return true;

        body.reserve(static_cast<size_t>(remaining));
        while (remaining > 0) {
            char *chunk = 0;
            const int want = remaining > kBodyChunk ? kBodyChunk
                                                    : static_cast<int>(remaining);
            const int got = req.BuffgetData(want, &chunk, true);
            if (got <= 0) return false;
            body.append(chunk, static_cast<size_t>(got));
            remaining -= got;
        }
        return true;
    }

    int DoConnect(XrdHttpExtReq &req) {
        std::string error;
        const std::string token = sessions_.Open(error);
        if (token.empty()) return Fail(req, 503, "Service Unavailable", error);

        return req.SendSimpleResp(200, "OK", 0, token.c_str(),
                                  static_cast<long long>(token.size()));
    }

    int DoMsg(XrdHttpExtReq &req) {
        const std::string token = HeaderValue(req.headers, kSessionHeader);
        if (token.empty())
            return Fail(req, 400, "Bad Request", "missing X-Fdp-Session");

        std::string body;
        if (!ReadBody(req, body))
            return Fail(req, 400, "Bad Request", "could not read request body");
        if (body.empty())
            return Fail(req, 400, "Bad Request", "empty mdsip call");

        std::string answer, error;
        bool busy = false;
        if (!sessions_.Relay(token, body, answer, error, &busy))
            return busy ? Fail(req, 409, "Conflict", error)
                        : Fail(req, 502, "Bad Gateway", error);

        return req.SendSimpleResp(200, "OK", 0, answer.data(),
                                  static_cast<long long>(answer.size()));
    }

    int DoClose(XrdHttpExtReq &req) {
        const std::string token = HeaderValue(req.headers, kSessionHeader);
        if (!token.empty()) sessions_.Close(token);
        return req.SendSimpleResp(200, "OK", 0, "", 0);
    }

    XrdSysError   *log_;
    std::string    prefix_;
    fdp::MdsipSessions sessions_;
};

}  // namespace

extern "C" XrdHttpExtHandler *XrdHttpGetExtHandler(XrdSysError *eDest,
                                                   const char *confg,
                                                   const char *parms,
                                                   XrdOucEnv *myEnv) {
    (void)confg;
    (void)myEnv;

    const std::string prefix   = ParmValue(parms, "prefix", "/mdsip");
    const std::string host     = ParmValue(parms, "host", "localhost");
    const std::string port_str = ParmValue(parms, "port", "8000");
    const int port             = std::atoi(port_str.c_str());
    const int idle             = std::atoi(ParmValue(parms, "idle", "300").c_str());
    const int timeout_ms =
        std::atoi(ParmValue(parms, "timeout", "60").c_str()) * 1000;
    const size_t max_sessions =
        std::strtoul(ParmValue(parms, "maxsessions", "256").c_str(), 0, 10);

    if (port <= 0 || port > 65535) {
        eDest->Emsg("relay", "invalid mdsip port", port_str.c_str());
        return 0;
    }

    // An XrdHttpExtHandler runs BEFORE XRootD's authorization. XrdHttpReq.cc
    // dispatches at reqstate == 0, ahead of the file-access path where
    // ofs.authorize and the SciTokens plugin live, so nothing this handler
    // serves is authenticated by XRootD -- measured: PUT /mdsip/connect returns
    // 200 with no credentials and with a garbage bearer token, while the same
    // unauthenticated PUT to a path we do not claim returns 403
    // (tests/fed/probe_federation_post.sh).
    //
    // Since a session is arbitrary code execution on the mdsip host, an
    // unauthenticated relay must never be reached by accident. Refuse to load
    // unless the operator has said which it is.
    const std::string auth = ParmValue(parms, "auth", "");
    if (auth != "none") {
        eDest->Emsg("relay", "refusing to load: no 'auth' setting.",
                    "An ext handler runs BEFORE XRootD authorization, so this "
                    "relay authenticates nobody. Anyone who can reach this port "
                    "could open an mdsip session, which is arbitrary code "
                    "execution. Set auth=none to accept that deliberately "
                    "(development only); token validation is not yet "
                    "implemented -- see docs/security.md.");
        return 0;
    }
    eDest->Say("------ XrdHttpMdsip WARNING: auth=none. This relay is "
               "UNAUTHENTICATED and must not face an untrusted network.");

    const std::string banner = prefix + " -> mdsip " + host + ":" + port_str;
    eDest->Say("++++++ XrdHttpMdsip relay: ", banner.c_str());

    return new HttpMdsipRelay(eDest, prefix, host, port, idle, max_sessions,
                              timeout_ms);
}
