#ifndef FDP_BASE64URL_HH
#define FDP_BASE64URL_HH

#include <string>

namespace fdp {

// RFC 4648 section 5 alphabet, padding omitted.
//
// The alphabet must exclude '/' and '%': Pelican's director runs
// path.Clean() over Go's already-decoded URL path, so a '/' becomes a real
// separator and percent-encoding cannot protect it. Verified empirically —
// see tests/fed/FINDINGS.md.
std::string Base64UrlEncode(const std::string &in);

// Returns false on any character outside the alphabet, or on a length that
// cannot result from a valid encoding.
bool Base64UrlDecode(const std::string &in, std::string &out);

}  // namespace fdp

#endif
