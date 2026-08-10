#include "HttpTunnel.hh"

#include <curl/curl.h>

#include <cstdlib>
#include <cstdio>
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

HttpTunnel::HttpTunnel() : curl_(0) {}

HttpTunnel::~HttpTunnel() {
    Close();
    if (curl_) { curl_easy_cleanup(static_cast<CURL *>(curl_)); curl_ = 0; }
}

bool HttpTunnel::Open(const std::string &target, std::string &error) {
    error.clear();

    url_base_ = BuildUrlBase(target);
    if (url_base_.empty()) { error = "cannot parse target '" + target + "'"; return false; }

    bearer_ = FindBearerToken();

    curl_ = curl_easy_init();
    if (!curl_) { error = "curl_easy_init failed"; return false; }

    std::string token;
    if (!Post("/connect", std::string(), false, token, error)) return false;

    token_ = Trim(token);
    if (token_.empty()) { error = "relay returned an empty session token"; return false; }
    return true;
}

bool HttpTunnel::Call(const std::string &request, std::string &answer,
                      std::string &error) {
    if (token_.empty()) { error = "no session"; return false; }
    return Post("/msg", request, true, answer, error);
}

void HttpTunnel::Close() {
    if (token_.empty() || !curl_) { token_.clear(); return; }
    std::string out, error;
    Post("/close", std::string(), true, out, error);   // best effort
    token_.clear();
}

bool HttpTunnel::Post(const std::string &action, const std::string &body,
                      bool with_session, std::string &out, std::string &error) {
    out.clear();
    error.clear();
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
        headers = curl_slist_append(headers, ("X-Fdp-Session: " + token_).c_str());

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // No total timeout: a legitimate call can be slow (a wide getMany over a
    // large tree). Bound the connect instead, which is what actually hangs
    // when the origin is unreachable.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        error = "POST " + url + ": " + curl_easy_strerror(rc);
        return false;
    }
    if (http_code != 200) {
        char code[32];
        std::snprintf(code, sizeof(code), "%ld", http_code);
        // The relay puts a one-line reason in the body; surfacing it turns
        // "MDSplus error" into something diagnosable.
        error = "POST " + url + " -> HTTP " + code + ": " + Trim(out);
        out.clear();
        return false;
    }
    return true;
}

}  // namespace fdp
