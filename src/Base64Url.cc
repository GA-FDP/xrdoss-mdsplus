#include "Base64Url.hh"

namespace {

const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

}  // namespace

namespace fdp {

std::string Base64UrlEncode(const std::string &in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);

    size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i])     << 16) |
                           (static_cast<unsigned char>(in[i + 1]) <<  8) |
                            static_cast<unsigned char>(in[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >>  6) & 63];
        out += kAlphabet[ v        & 63];
        i += 3;
    }

    const size_t rem = in.size() - i;
    if (rem == 1) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (rem == 2) {
        const unsigned v = (static_cast<unsigned char>(in[i])     << 16) |
                           (static_cast<unsigned char>(in[i + 1]) <<  8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >>  6) & 63];
    }
    return out;
}

bool Base64UrlDecode(const std::string &in, std::string &out) {
    out.clear();
    // A group of 4 chars encodes 3 bytes. Remainders of 2 and 3 chars are
    // legal (1 and 2 trailing bytes); a remainder of 1 char encodes nothing
    // and cannot come from a valid encoding.
    if (in.size() % 4 == 1) return false;
    out.reserve(in.size() / 4 * 3);

    unsigned buf = 0;
    int bits = 0;
    for (const char c : in) {
        const int d = DecodeChar(c);
        if (d < 0) return false;
        buf = (buf << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return true;
}

}  // namespace fdp
