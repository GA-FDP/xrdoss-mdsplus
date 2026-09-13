// src/PointWire.hh
//
// The parts of the point endpoint's wire handling that are pure functions, so
// they can be unit-tested. HttpRelay.cc is compiled only into the XRootD
// plugin module and is linked by no test target, so anything left inside it
// has NO automated guardrail -- a 409-to-404 regression was proved to break
// nothing at all. These live here for that reason.
#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace fdp {

// Percent-decode. Shared with the path parser so a query value and a path
// segment decode identically -- they used to be separate copies.
inline std::string UrlDecode(const std::string &s) {
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

// Parse "a=1&b=two" -- a RAW query string, with no leading '?'. That is the
// shape XRootD hands an ext handler (see QueryFromHeaders below).
//
// This endpoint never read a query parameter before B3: ?ext was accepted and
// IGNORED because the store resolves the extension itself. An UNKNOWN
// parameter stays ignored -- deployed clients already send ?ext, and failing
// those requests would turn a cosmetic mismatch into an outage.
//
// Only the VALUE is decoded, not the key: every key this endpoint reads is a
// fixed ASCII literal. If that ever stops being true, decode both.
inline std::map<std::string, std::string> ParseQueryString(const std::string &qs) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < qs.size()) {
        const size_t amp = qs.find('&', pos);
        const std::string pair =
            qs.substr(pos, amp == std::string::npos ? std::string::npos
                                                    : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && eq > 0) {
            out[pair.substr(0, eq)] = UrlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

// Same, for a whole request target: everything after the first '?'.
inline std::map<std::string, std::string> ParseQuery(const std::string &rest) {
    const size_t q = rest.find('?');
    if (q == std::string::npos) return {};
    return ParseQueryString(rest.substr(q + 1));
}

// Where the query ACTUALLY is, for an XRootD ext handler.
//
// XrdHttpExtReq::resource is stripped of the query. XRootD's own header says
// so -- "The resource specified by the request, stripped of opaque data" --
// and XrdHttpExtReq's constructor assigns `resource = req->resource`, the
// stripped one. It puts the query somewhere else entirely:
//
//     headers["xrd-http-query"]        "version=2&ext=.MAG"   (no '?')
//     headers["xrd-http-fullresource"] "/165920/IP?version=2"
//
// So ParseQuery(req.resource) can only ever return nothing. That is exactly
// how 7.26.0-fdp2.6.0 shipped: every ?version= and ?snapshot= pin was
// silently dropped and the latest version served in its place, with a 200.
// Nothing caught it because the parser was correct in isolation -- it was
// being handed the wrong string.
//
// xrd-http-query is preferred; xrd-http-fullresource is the fallback for a
// build that fills one and not the other.
inline std::string QueryFromHeaders(
        const std::map<std::string, std::string> &headers) {
    const auto q = headers.find("xrd-http-query");
    if (q != headers.end() && !q->second.empty()) return q->second;

    const auto full = headers.find("xrd-http-fullresource");
    if (full != headers.end()) {
        const size_t mark = full->second.find('?');
        if (mark != std::string::npos) return full->second.substr(mark + 1);
    }
    return std::string();
}

// What a record means on the wire. Extracted so the cross-repo contract is
// TESTABLE: a pin that cannot be honoured must be 409 and never 404, because
// the client treats a 404 from this tier as an authoritative miss and stops --
// which would make stale provenance indistinguishable from data that never
// existed. Nothing else in either repo guards that.
//
// Takes the three flags rather than the Record so this header stays free of
// PointStore.hh and its ptdata dependency.
inline int StatusForRecord(bool found, bool pin_failed) {
    if (pin_failed) return 409;   // Conflict: well-formed, unsatisfiable
    if (!found)     return 404;   // ordinary absence
    return 200;
}

}  // namespace fdp
