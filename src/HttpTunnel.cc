#include "HttpTunnel.hh"

#include "MdsipCall.hh"
#include "RelayProtocol.hh"

#include <curl/curl.h>

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace fdp {

namespace {

size_t Collect(char *ptr, size_t size, size_t nmemb, void *userdata) {
    std::string *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\n' || s[b] == '\r' || s[b] == '\t')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\n' || s[e-1] == '\r' || s[e-1] == '\t')) --e;
    return s.substr(b, e - b);
}

std::string Lower(const std::string &s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(out[i])));
    return out;
}

// Whether `expr` starts with `name` followed by '(' -- i.e. is a call to it.
// A prefix match alone would make TreeOpenNew look like TreeOpen, which is
// harmless here but would not be if the two ever needed different handling.
bool CallsFunction(const std::string &lowered, const char *name) {
    const size_t n = std::strlen(name);
    if (lowered.compare(0, n, name) != 0) return false;
    size_t i = n;
    while (i < lowered.size() && lowered[i] == ' ') ++i;
    return i < lowered.size() && lowered[i] == '(';
}

// A '=' that assigns rather than compares. TDI writes equality as '==' and
// has '!=', '<=' and '>='; everything else that reaches here is treated as an
// assignment, because guessing wrong in that direction only costs recovery,
// while guessing wrong in the other costs correctness.
bool HasAssignment(const std::string &expr) {
    for (size_t i = 0; i < expr.size(); ++i) {
        if (expr[i] != '=') continue;
        if (i + 1 < expr.size() && expr[i + 1] == '=') { ++i; continue; }
        if (i > 0) {
            const char p = expr[i - 1];
            if (p == '=' || p == '!' || p == '<' || p == '>') continue;
        }
        return true;
    }
    return false;
}

}  // namespace

std::string FindBearerToken() {
    const char *env = std::getenv("BEARER_TOKEN");
    if (env && *env) return Trim(env);

    const char *home = std::getenv("HOME");
    if (!home || !*home) return std::string();

    const std::string path = std::string(home) + "/.fdp/token";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();

    std::string tok;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) tok.append(buf, n);
    std::fclose(f);
    return Trim(tok);
}

std::string BuildUrlBase(const std::string &target) {
    // https unless told otherwise. FDP_TUNNEL_SCHEME exists for testing against
    // a local relay with no TLS; production is always https, and defaulting the
    // other way would make an insecure deployment the easy mistake.
    const char *scheme_env = std::getenv("FDP_TUNNEL_SCHEME");
    const std::string scheme = (scheme_env && *scheme_env) ? scheme_env : "https";

    std::string hostport = target;
    std::string prefix = "/mdsip";

    const size_t slash = target.find('/');
    if (slash != std::string::npos) {
        hostport = target.substr(0, slash);
        prefix = target.substr(slash);
        if (prefix.size() > 1 && prefix[prefix.size() - 1] == '/')
            prefix.erase(prefix.size() - 1);
    }
    if (hostport.empty()) return std::string();

    // An explicit port stays; otherwise the scheme's default applies, which
    // curl supplies. Naming 443 here would break a plain-http test target.
    return scheme + "://" + hostport + prefix;
}

std::string CallExpression(const std::string &call) {
    if (call.size() < kHeaderBytes) return std::string();

    const char *hdr = call.data();
    if (static_cast<unsigned char>(hdr[kDtypeOffset]) != kDtypeCString)
        return std::string();
    if (static_cast<unsigned char>(hdr[kDescriptorIdxOffset]) != 0)
        return std::string();

    unsigned int msglen;
    std::memcpy(&msglen, hdr + kMsgLenOffset, sizeof(msglen));
    if (msglen < kHeaderBytes || msglen > call.size()) return std::string();

    short length;
    std::memcpy(&length, hdr + kLengthOffset, sizeof(length));
    if (length <= 0) return std::string();

    const size_t want = static_cast<size_t>(length);
    if (kHeaderBytes + want > msglen) return std::string();
    return call.substr(kHeaderBytes, want);
}

CallEffect EffectOf(const std::string &expression) {
    // Unreadable is not the same as harmless. CallExpression returns empty for
    // anything it could not parse, and that has to stay on the pessimistic side.
    if (expression.empty()) return kUnknownEffect;

    const std::string lowered = Lower(Trim(expression));
    if (CallsFunction(lowered, "treeclose")) return kClosesTree;
    if (CallsFunction(lowered, "treeopen") ||
        CallsFunction(lowered, "treeopennew") ||
        CallsFunction(lowered, "treesetdefault"))
        return kOpensTree;

    if (HasAssignment(lowered)) return kUnknownEffect;
    return kNoEffect;
}

HttpTunnel::HttpTunnel() : curl_(0), replayable_(true), redials_(0) {
    // PUT by default: it is the only method that works both against a
    // directly-addressed origin AND through the Pelican director. Overridable
    // for debugging, not for deployment.
    const char *m = std::getenv("FDP_TUNNEL_METHOD");
    method_ = (m && *m) ? m : "PUT";
}

HttpTunnel::~HttpTunnel() {
    Close();
    if (curl_) { curl_easy_cleanup(static_cast<CURL *>(curl_)); curl_ = 0; }
}

bool HttpTunnel::Open(const std::string &target, std::string &error) {
    error.clear();

    url_base_ = BuildUrlBase(target);
    if (url_base_.empty()) { error = "cannot parse target '" + target + "'"; return false; }

    curl_ = curl_easy_init();
    if (!curl_) { error = "curl_easy_init failed"; return false; }

    return Redial(error);
}

bool HttpTunnel::Redial(std::string &error) {
    token_.clear();
    bearer_ = FindBearerToken();

    std::string token;
    if (!Post("/connect", std::string(), false, token, error)) return false;

    token_ = Trim(token);
    if (token_.empty()) { error = "relay returned an empty session token"; return false; }
    return true;
}

void HttpTunnel::Remember(const std::string &request) {
    // The first call is the login, whatever it happens to look like. It is not
    // a TDI expression and must not be classified as one -- it carries a
    // username, and a username is not something to reason about the semantics
    // of.
    if (login_.empty()) { login_ = request; return; }

    switch (EffectOf(CallExpression(request))) {
        case kOpensTree:
            // Only the latest matters: MDSplus sets the tree context by
            // overwriting it, so replaying an earlier open would restore a tree
            // the caller has since moved off.
            tree_open_ = request;
            break;
        case kClosesTree:
            tree_open_.clear();
            break;
        case kUnknownEffect:
            replayable_ = false;
            break;
        case kNoEffect:
            break;
    }
}

bool HttpTunnel::Replay(std::string &error) {
    // Login first: mdsip answers nothing until it has one, so replaying the
    // tree open ahead of it fails with "mdsip did not answer" -- which reads
    // like the relay timing out rather than like a protocol violation.
    const std::string *const steps[] = { &login_, &tree_open_ };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (steps[i]->empty()) continue;
        std::string discarded;
        if (!Post("/msg", *steps[i], true, discarded, error)) return false;
    }
    return true;
}

bool HttpTunnel::Call(const std::string &request, std::string &answer,
                      std::string &error) {
    if (token_.empty()) { error = "no session"; return false; }

    long status = 0;
    std::string reason;
    if (Post("/msg", request, true, answer, error, &status, &reason)) {
        Remember(request);
        return true;
    }

    // Only a session the relay no longer holds is worth retrying. Every other
    // failure -- 401, 409, an unreachable origin, mdsip itself erroring -- is
    // either the caller's to fix or would fail again identically.
    if (status != 502 || Trim(reason) != kSessionGone) return false;

    if (!replayable_) {
        error += " (not redialling: this session holds TDI state a new one "
                 "could not be given)";
        return false;
    }

    const std::string lost = error;
    std::string redial_error;
    if (!Redial(redial_error)) {
        error = lost + " (and redialling failed: " + redial_error + ")";
        return false;
    }
    ++redials_;

    if (!Replay(redial_error)) {
        error = lost + " (and restoring the new session failed: " +
                redial_error + ")";
        return false;
    }

    if (!Post("/msg", request, true, answer, error)) return false;
    Remember(request);
    return true;
}

void HttpTunnel::Close() {
    if (token_.empty() || !curl_) { token_.clear(); return; }
    std::string out, error;
    Post("/close", std::string(), true, out, error);   // best effort
    token_.clear();
}

bool HttpTunnel::Post(const std::string &action, const std::string &body,
                      bool with_session, std::string &out, std::string &error,
                      long *status, std::string *reason) {
    out.clear();
    error.clear();
    if (status) *status = 0;
    if (reason) reason->clear();
    CURL *curl = static_cast<CURL *>(curl_);
    if (!curl) { error = "tunnel is not open"; return false; }

    const std::string url = url_base_ + action;

    curl_slist *headers = 0;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    // curl would otherwise add "Expect: 100-continue" for large bodies and wait
    // out the timeout when the peer does not answer it.
    headers = curl_slist_append(headers, "Expect:");
    if (!bearer_.empty())
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer_).c_str());
    if (with_session)
        headers = curl_slist_append(headers,
                                    (std::string(kSessionHeader) + ": " + token_).c_str());

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    // PUT rather than POST, with the body still sent as POST fields. The
    // Pelican director will not route POST -- 404 at the namespace path, 405 at
    // its API endpoint, and its CORS header advertises only GET, PUT, OPTIONS,
    // PROPFIND -- but routes PUT with a 307 that preserves method and body.
    // Both reach a directly-addressed origin, so PUT is the one that works
    // everywhere. Measured in tests/fed/probe_federation_post.sh.
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Required for the federation: the director answers with a 307 to an
    // origin, so not following it means never reaching the relay at all.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // A custom CA is a legitimate deployment need; skipping verification is
    // not, and exists only so the self-signed local test federation can be
    // exercised. Both are opt-in, so the secure behaviour is what you get by
    // doing nothing.
    const char *cainfo = std::getenv("FDP_TUNNEL_CAINFO");
    if (cainfo && *cainfo) curl_easy_setopt(curl, CURLOPT_CAINFO, cainfo);
    const char *insecure = std::getenv("FDP_TUNNEL_INSECURE");
    if (insecure && *insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    // No total timeout: a legitimate call can be slow (a wide getMany over a
    // large tree). Bound the connect instead, which is what actually hangs
    // when the origin is unreachable. The relay bounds the same call from its
    // end -- see its `timeout=` parm, which has to clear the slowest real call
    // for the same reason there is no ceiling here.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    if (status) *status = http_code;

    if (rc != CURLE_OK) {
        error = "POST " + url + ": " + curl_easy_strerror(rc);
        return false;
    }
    if (http_code != 200) {
        char code[32];
        std::snprintf(code, sizeof(code), "%ld", http_code);
        // The relay puts a one-line reason in the body; surfacing it turns
        // "MDSplus error" into something diagnosable.
        const std::string trimmed = Trim(out);
        if (reason) *reason = trimmed;
        error = "POST " + url + " -> HTTP " + code + ": " + trimmed;
        out.clear();
        return false;
    }
    return true;
}

}  // namespace fdp
