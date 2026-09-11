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

// Parse "a=1&b=two" from the query half of a request target.
//
// This endpoint never read a query parameter before B3: ?ext was accepted and
// IGNORED because the store resolves the extension itself. An UNKNOWN
// parameter stays ignored -- deployed clients already send ?ext, and failing
// those requests would turn a cosmetic mismatch into an outage.
//
// Only the VALUE is decoded, not the key: every key this endpoint reads is a
// fixed ASCII literal. If that ever stops being true, decode both.
inline std::map<std::string, std::string> ParseQuery(const std::string &rest) {
    std::map<std::string, std::string> out;
    const size_t q = rest.find('?');
    if (q == std::string::npos) return out;

    const std::string qs = rest.substr(q + 1);
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
