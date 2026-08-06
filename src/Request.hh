#ifndef FDP_REQUEST_HH
#define FDP_REQUEST_HH

#include <string>
#include <vector>

namespace fdp {

struct RequestItem {
    std::string              name;   // caller's label; appears in the response dict
    std::string              exp;    // TDI expression, opaque bytes
    std::vector<std::string> args;   // each is MdsSerializeDscOut output
};

// The canonical wire form of a request. See design spec section 4.6:
//
//   request := u8 version(=1)  u16 count  count x item
//   item    := u16 name_len name  u32 exp_len exp  u8 nargs
//              nargs x ( u32 arg_len arg )
//
// This encoding is baked into the object path, so it is a wire format: any
// change invalidates every cached object in the federation. The version byte
// exists to make such a change explicit rather than silent.
struct Request {
    static const unsigned char kVersion = 1;

    std::vector<RequestItem> items;

    std::string Serialize() const;

    // Strict by design: rejects bad versions, truncation and trailing bytes,
    // so that a given byte string has exactly one interpretation. Ambiguity
    // here would mean two distinct paths naming the same evaluation, or one
    // path naming two.
    //
    // On failure `out` is left empty rather than partially filled.
    static bool Parse(const std::string &in, Request &out);
};

}  // namespace fdp

#endif
