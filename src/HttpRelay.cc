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

#include "PointStore.hh"
#include "RelayProtocol.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucErrInfo.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdVersion.hh"

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <cctype>
#include <sstream>
#include <string>

XrdVERSIONINFO(XrdHttpGetExtHandler, XrdHttpMdsip);

namespace {

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

// Percent-encode for use as an opaque CGI value. The Authorization header is
// "Bearer <jwt>", and the space alone would truncate the value.
std::string UrlEncode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// Percent-decode one path segment. Pointnames are uppercase alphanumerics plus
// a little punctuation, but the client percent-encodes the segment, so decode
// it rather than assume which characters survived.
std::string UrlDecode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), 0, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// "/165920/IP?ext=.MAG" -> shot "165920", pointname "IP".
//
// The query string is dropped: ?ext is a hint, and resolution here is
// index-first, where JsonIndexPlugin::resolve takes only (pointname, shot).
// The index decides which extension holds a pointname, and the response says
// which one answered.
bool SplitPointPath(const std::string &rest, std::string &shot,
                    std::string &pointname) {
    std::string path = rest;
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    if (path.empty() || path[0] != '/') return false;
    const size_t sep = path.find('/', 1);
    if (sep == std::string::npos) return false;

    shot = path.substr(1, sep - 1);
    pointname = UrlDecode(path.substr(sep + 1));
    // Reject a trailing segment: /165920/IP/extra is not this contract.
    if (pointname.empty() || pointname.find('/') != std::string::npos) return false;
    return !shot.empty();
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
                   size_t max_sessions, int timeout_ms,
                   XrdSfsFileSystem *sfs, const std::string &authpath,
                   const std::string &point_prefix,
                   const std::string &point_authpath,
                   fdp::PointStore *points)
        : log_(log), prefix_(prefix), sfs_(sfs), authpath_(authpath),
          point_prefix_(point_prefix), point_authpath_(point_authpath),
          points_(points),
          sessions_(host, port, idle, max_sessions, timeout_ms) {}

    bool MatchesPath(const char *verb, const char *path) {
        if (!path || !verb) return false;

        // GET, only under the point prefix, and only when one is configured.
        // The relay refuses GET below because it would shadow the object
        // namespace the Oss plugin serves; that reasoning does not reach a
        // disjoint prefix which is not a storage path.
        if (std::strcmp(verb, "GET") == 0) {
            if (point_prefix_.empty()) return false;
            if (std::strncmp(path, point_prefix_.c_str(), point_prefix_.size()) != 0)
                return false;
            const char after = path[point_prefix_.size()];
            return after == '\0' || after == '/';
        }

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

        if (std::strncmp(path, prefix_.c_str(), prefix_.size()) != 0)
            return false;
        // The prefix must end at a path separator. A bare strncmp would make
        // prefix=/mdsip also claim /mdsip-private and /mdsipanything, silently
        // shadowing a neighbouring namespace -- caught by the federation test,
        // where two instances sit at /mdsip and /mdsip-private and the first
        // swallowed the second's requests and 404'd them.
        const char after = path[prefix_.size()];
        return after == '\0' || after == '/';
    }

    int Init(const char *) { return 0; }

    int ProcessReq(XrdHttpExtReq &req) {
        // Route on the request's own verb rather than anything remembered from
        // MatchesPath: one handler instance serves every request, so a stashed
        // flag would race across connections.
        if (req.verb == "GET") return DoPoint(req);

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

    // Delegate authorization to XRootD rather than validating tokens here.
    //
    // An ext handler is dispatched before the file-access path, so nothing it
    // serves is authorized automatically -- but the framework hands us the
    // filesystem object, and asking it to stat a path runs the site's
    // configured ofs.authorize / SciTokens policy exactly as a normal request
    // would. This is the pattern XrdHttpTPC uses (XrdHttpTpcTPC.cc:827-830).
    //
    // The upshot: the relay is protected by whatever already protects the rest
    // of the origin, with no second token implementation to keep in step.
    bool Authorized(XrdHttpExtReq &req, std::string &why,
                    const std::string &authpath) {
        if (!sfs_) return true;   // auth=none; the loader has already warned

        // XrdAccSciTokens reads the token from the `authz` opaque, which is
        // where http.header2cgi would have put it for a normal request.
        const std::string header = HeaderValue(req.headers, "authorization");
        std::string opaque;
        if (!header.empty()) opaque = "authz=" + UrlEncode(header);

        struct stat sbuf;
        XrdOucErrInfo einfo;
        const int rc = sfs_->stat(authpath.c_str(), &sbuf, einfo,
                                  &req.GetSecEntity(),
                                  opaque.empty() ? 0 : opaque.c_str());
        if (rc == SFS_OK) return true;

        // The path need not exist: authorization runs first, so ENOENT means
        // the policy said yes and there was simply nothing there. Anything
        // else -- EACCES, or an error we do not recognise -- denies, so an
        // unexpected failure fails closed.
        const int err = einfo.getErrInfo();
        if (err == ENOENT) return true;

        const char *text = einfo.getErrText();
        why = std::string("not authorized for ") + authpath +
              (text && *text ? std::string(": ") + text : std::string());
        return false;
    }

    int DoConnect(XrdHttpExtReq &req) {
        // Checked once, at session creation. Every later call presents the
        // 128-bit session token instead, so a session can outlive the bearer
        // token that opened it -- bounded by the idle reaper, and the tradeoff
        // is deliberate: re-authorizing every call would add a round trip to a
        // workload that is thousands of one-signal gets.
        std::string why;
        if (!Authorized(req, why, authpath_))
            return Fail(req, 401, "Unauthorized", why);

        std::string error;
        const std::string token = sessions_.Open(error);
        if (token.empty()) return Fail(req, 503, "Service Unavailable", error);

        return req.SendSimpleResp(200, "OK", 0, token.c_str(),
                                  static_cast<long long>(token.size()));
    }

    int DoMsg(XrdHttpExtReq &req) {
        const std::string token = HeaderValue(req.headers, fdp::kSessionHeader);
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

    // GET <point_prefix>/<shot>/<pointname>[?ext=...]
    //
    // Authorized per request, unlike the relay's session. Each GET is
    // independent, so a session would buy nothing but a cache of
    // authorization decisions.
    int DoPoint(XrdHttpExtReq &req) {
        if (!points_)
            return Fail(req, 404, "Not Found", "point endpoint not configured");

        std::string why;
        if (!Authorized(req, why, point_authpath_))
            return Fail(req, 401, "Unauthorized", why);

        const std::string rest = req.resource.size() >= point_prefix_.size()
            ? req.resource.substr(point_prefix_.size()) : std::string();

        std::string shot_s, pointname;
        if (!SplitPointPath(rest, shot_s, pointname))
            return Fail(req, 404, "Not Found",
                        "expected <prefix>/<shot>/<pointname>");

        char *end = 0;
        const long shot = std::strtol(shot_s.c_str(), &end, 10);
        if (!end || *end != '\0' || shot <= 0)
            return Fail(req, 400, "Bad Request",
                        "shot must be a positive integer: " + shot_s);

        fdp::PointStore::Record rec;
        try {
            rec = points_->Read(static_cast<int>(shot), pointname);
        } catch (const std::exception &e) {
            // A server fault -- unreadable file, malformed header. It must not
            // reach the client as 404, or a broken origin reads as absent data
            // and its provider chain quietly moves on.
            return Fail(req, 500, "Internal Server Error", e.what());
        }

        if (!rec.found)
            return Fail(req, 404, "Not Found",
                        "no point " + pointname + " for shot " + shot_s);

        // No trailing CRLF: XrdHttpProtocol appends one itself, and a second
        // would end the header block early and truncate the response.
        const std::string extra = rec.extension.empty()
            ? std::string()
            : "X-Ptdata-Extension: " + rec.extension;

        return req.SendSimpleResp(200, "OK", extra.empty() ? 0 : extra.c_str(),
                                  reinterpret_cast<const char *>(rec.bytes.data()),
                                  static_cast<long long>(rec.bytes.size()));
    }

    int DoClose(XrdHttpExtReq &req) {
        const std::string token = HeaderValue(req.headers, fdp::kSessionHeader);
        if (!token.empty()) sessions_.Close(token);
        return req.SendSimpleResp(200, "OK", 0, "", 0);
    }

    XrdSysError      *log_;
    std::string       prefix_;
    XrdSfsFileSystem *sfs_;        // null when auth=none
    std::string       authpath_;   // the path whose policy gates a session
    std::string       point_prefix_;   // empty => point endpoint disabled
    std::string       point_authpath_; // the path whose policy gates a point read
    fdp::PointStore       *points_;         // null => point endpoint disabled
    fdp::MdsipSessions sessions_;
};

}  // namespace

extern "C" XrdHttpExtHandler *XrdHttpGetExtHandler(XrdSysError *eDest,
                                                   const char *confg,
                                                   const char *parms,
                                                   XrdOucEnv *myEnv) {
    (void)confg;

    const std::string prefix   = ParmValue(parms, "prefix", "/mdsip");
    const std::string host     = ParmValue(parms, "host", "localhost");
    const std::string port_str = ParmValue(parms, "port", "8000");
    const int port             = std::atoi(port_str.c_str());
    const int idle             = std::atoi(ParmValue(parms, "idle", "300").c_str());
    // How long one mdsip call may take to answer. This is not a liveness
    // check: an mdsip busy evaluating is indistinguishable, from the relay's
    // side of the socket, from an mdsip that will never answer, so the number
    // has to clear the slowest LEGITIMATE call rather than the typical one.
    //
    // 60 s did not. A 95-pointname ptdata getMany -- 285 PTDATA2/dim_of/pthead2
    // evaluations returning 539 MB, an ordinary imas_composer magnetics fetch --
    // measures 11 s of server-side compute warm and crossed 60 s under load,
    // taking the session down with it (GA-FDP/imas_composer CI run 33033220932).
    // The client sets no total timeout at all for exactly this reason, and says
    // so (HttpTunnel.cc, CURLOPT_CONNECTTIMEOUT); the two ends now agree.
    const int timeout_ms =
        std::atoi(ParmValue(parms, "timeout", "600").c_str()) * 1000;
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
    // The framework does hand us what is needed to do it ourselves, so this is
    // an opt-in rather than a gap. Since a session is arbitrary code execution
    // on the mdsip host, refuse to load unless told which mode this is.
    // The point endpoint. Absent pointprefix leaves it off entirely, so this
    // ships dark and an origin that only wants the relay is unaffected.
    const std::string point_prefix    = ParmValue(parms, "pointprefix", "");
    const std::string point_authpath  = ParmValue(parms, "pointauthpath", "");
    const std::string point_index     = ParmValue(parms, "pointindex", "");
    const std::string point_pattern   = ParmValue(parms, "pointindexpattern", "");
    const std::string point_urlprefix = ParmValue(parms, "pointurlprefix", "");
    const std::string point_root      = ParmValue(parms, "pointroot", "");

    const std::string auth = ParmValue(parms, "auth", "");
    const std::string authpath = ParmValue(parms, "authpath", prefix);
    XrdSfsFileSystem *sfs = 0;

    if (auth == "xrootd") {
        // The same object XrdHttpTPC uses (XrdHttpTpcConfigure.cc:131); the
        // framework puts it here in XrdXrootdConfig.cc:306.
        void *raw = myEnv ? myEnv->GetPtr("XrdSfsFileSystem*") : 0;
        sfs = static_cast<XrdSfsFileSystem *>(raw);
        if (!sfs) {
            eDest->Emsg("relay", "refusing to load: auth=xrootd, but the "
                        "framework offered no XrdSfsFileSystem, so "
                        "authorization cannot be delegated to it.");
            return 0;
        }
        eDest->Say("++++++ XrdHttpMdsip auth=xrootd; sessions gated by the "
                   "authorization policy on ", authpath.c_str());
    } else if (auth == "none") {
        eDest->Say("------ XrdHttpMdsip WARNING: auth=none. This relay is "
                   "UNAUTHENTICATED and must not face an untrusted network.");
    } else {
        eDest->Emsg("relay", "refusing to load: 'auth' must be xrootd or none.",
                    "An ext handler runs BEFORE XRootD authorization, so a "
                    "relay that does not check for itself is open to anyone who "
                    "can reach this port -- and an mdsip session is arbitrary "
                    "code execution. Use auth=xrootd to delegate to this "
                    "origin's existing authorization (optionally with "
                    "authpath=<path>), or auth=none on a trusted network. "
                    "See docs/security.md.");
        return 0;
    }

    // Refuse a half-configured endpoint. One that loads but cannot resolve
    // answers 404 for every point, which reads as missing data rather than as
    // a mistake -- the hardest kind of misconfiguration to trace.
    fdp::PointStore *points = 0;
    if (!point_prefix.empty()) {
        if (point_index.empty()) {
            eDest->Emsg("point", "refusing to load: pointprefix is set but "
                        "pointindex is not, so every lookup would 404 and read "
                        "as absent data rather than as misconfiguration.");
            return 0;
        }
        // Only meaningful when authorization is delegated: with auth=none
        // Authorized() returns true before ever looking at a path, so
        // demanding one there would force a value that changes nothing.
        if (auth == "xrootd" && point_authpath.empty()) {
            eDest->Emsg("point", "refusing to load: pointprefix is set with "
                        "auth=xrootd but pointauthpath is not. An ext handler "
                        "runs BEFORE XRootD authorization, so without it the "
                        "archive would be readable by anyone who can reach "
                        "this port.");
            return 0;
        }
        if (point_urlprefix.empty() != point_root.empty()) {
            eDest->Emsg("point", "refusing to load: pointurlprefix and "
                        "pointroot must be set together -- one alone silently "
                        "disables the rewrite, and every index entry then "
                        "resolves as a missing file.");
            return 0;
        }
        try {
            points = new fdp::PointStore(point_index, point_pattern,
                                    point_urlprefix, point_root);
        } catch (const std::exception &e) {
            eDest->Emsg("point", "refusing to load: cannot open the point "
                        "index:", e.what());
            return 0;
        }
        const std::string pbanner = point_prefix + " -> index " + point_index;
        eDest->Say("++++++ XrdHttpMdsip point endpoint: ", pbanner.c_str());
    }

    const std::string banner = prefix + " -> mdsip " + host + ":" + port_str;
    eDest->Say("++++++ XrdHttpMdsip relay: ", banner.c_str());

    return new HttpMdsipRelay(eDest, prefix, host, port, idle, max_sessions,
                              timeout_ms, sfs, authpath,
                              point_prefix, point_authpath, points);
}
