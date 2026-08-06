#include "Request.hh"

#include <cstdint>

namespace {

void PutU8(std::string &s, unsigned v)  { s += static_cast<char>(v & 0xFF); }
void PutU16(std::string &s, unsigned v) { PutU8(s, v >> 8); PutU8(s, v); }
void PutU32(std::string &s, uint32_t v) { PutU16(s, static_cast<unsigned>(v >> 16));
                                          PutU16(s, static_cast<unsigned>(v & 0xFFFF)); }

// Cursor over the input; every read is bounds-checked.
struct Reader {
    const std::string &s;
    size_t pos;

    explicit Reader(const std::string &in) : s(in), pos(0) {}

    bool U8(unsigned &out) {
        if (pos + 1 > s.size()) return false;
        out = static_cast<unsigned char>(s[pos++]);
        return true;
    }
    bool U16(unsigned &out) {
        unsigned a, b;
        if (!U8(a) || !U8(b)) return false;
        out = (a << 8) | b;
        return true;
    }
    bool U32(uint32_t &out) {
        unsigned a, b;
        if (!U16(a) || !U16(b)) return false;
        out = (static_cast<uint32_t>(a) << 16) | b;
        return true;
    }
    bool Bytes(size_t n, std::string &out) {
        if (pos + n > s.size()) return false;
        out.assign(s, pos, n);
        pos += n;
        return true;
    }
};

}  // namespace

namespace fdp {

std::string Request::Serialize() const {
    std::string out;
    PutU8(out, kVersion);
    PutU16(out, static_cast<unsigned>(items.size()));
    for (size_t i = 0; i < items.size(); ++i) {
        const RequestItem &it = items[i];
        PutU16(out, static_cast<unsigned>(it.name.size()));
        out += it.name;
        PutU32(out, static_cast<uint32_t>(it.exp.size()));
        out += it.exp;
        PutU8(out, static_cast<unsigned>(it.args.size()));
        for (size_t a = 0; a < it.args.size(); ++a) {
            PutU32(out, static_cast<uint32_t>(it.args[a].size()));
            out += it.args[a];
        }
    }
    return out;
}

bool Request::Parse(const std::string &in, Request &out) {
    out.items.clear();

    Reader r(in);
    unsigned version = 0, count = 0;
    if (!r.U8(version) || version != kVersion) return false;
    if (!r.U16(count) || count == 0) return false;

    for (unsigned i = 0; i < count; ++i) {
        RequestItem item;
        unsigned name_len = 0, nargs = 0;
        uint32_t exp_len = 0;

        if (!r.U16(name_len) || !r.Bytes(name_len, item.name)) { out.items.clear(); return false; }
        if (!r.U32(exp_len)  || !r.Bytes(exp_len,  item.exp))  { out.items.clear(); return false; }
        if (item.exp.empty())                                  { out.items.clear(); return false; }
        if (!r.U8(nargs))                                      { out.items.clear(); return false; }

        for (unsigned a = 0; a < nargs; ++a) {
            uint32_t arg_len = 0;
            std::string arg;
            if (!r.U32(arg_len) || !r.Bytes(arg_len, arg)) { out.items.clear(); return false; }
            item.args.push_back(arg);
        }
        out.items.push_back(item);
    }

    // Trailing bytes would give one path two meanings.
    if (r.pos != in.size()) { out.items.clear(); return false; }
    return true;
}

}  // namespace fdp
