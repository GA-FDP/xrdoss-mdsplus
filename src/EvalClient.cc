#include "EvalClient.hh"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace {

bool WriteAll(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, buf + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::read(fd, buf + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void PutU16(std::string &s, unsigned v) {
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>( v       & 0xFF);
}

void PutU32(std::string &s, size_t v) {
    s += static_cast<char>((v >> 24) & 0xFF);
    s += static_cast<char>((v >> 16) & 0xFF);
    s += static_cast<char>((v >>  8) & 0xFF);
    s += static_cast<char>( v        & 0xFF);
}

void PutI64(std::string &s, long long v) {
    const unsigned long long u = static_cast<unsigned long long>(v);
    for (int i = 7; i >= 0; --i) s += static_cast<char>((u >> (i * 8)) & 0xFF);
}

// Closes the descriptor on any exit path, including early returns.
struct FdCloser {
    int fd;
    explicit FdCloser(int f) : fd(f) {}
    ~FdCloser() { if (fd >= 0) ::close(fd); }
};

}  // namespace

namespace fdp {

EvalClient::EvalClient(std::string socket_path, int timeout_ms)
    : socket_path_(std::move(socket_path)), timeout_ms_(timeout_ms) {}

bool EvalClient::Evaluate(const std::string &tree, long long shot,
                          const Request &request,
                          std::string &payload, std::string &error) const {
    payload.clear();
    error.clear();

    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        error = "evaluator socket path too long: " + socket_path_;
        return false;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { error = "socket(): " + std::string(std::strerror(errno)); return false; }
    FdCloser closer(fd);

    timeval tv;
    tv.tv_sec  = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        error = "connect(" + socket_path_ + "): " + std::string(std::strerror(errno));
        return false;
    }

    std::string body;
    PutU16(body, static_cast<unsigned>(tree.size()));
    body += tree;
    PutI64(body, shot);
    body += request.Serialize();

    std::string frame;
    PutU32(frame, body.size());
    if (!WriteAll(fd, frame.data(), frame.size()) ||
        !WriteAll(fd, body.data(), body.size())) {
        error = "short write to evaluator";
        return false;
    }

    char header[5];
    if (!ReadAll(fd, header, 5)) { error = "short read from evaluator"; return false; }

    const unsigned char status = static_cast<unsigned char>(header[0]);
    const size_t length =
        (static_cast<size_t>(static_cast<unsigned char>(header[1])) << 24) |
        (static_cast<size_t>(static_cast<unsigned char>(header[2])) << 16) |
        (static_cast<size_t>(static_cast<unsigned char>(header[3])) <<  8) |
         static_cast<size_t>(static_cast<unsigned char>(header[4]));

    std::string buf(length, '\0');
    if (length && !ReadAll(fd, &buf[0], length)) {
        error = "short read of evaluator payload";
        return false;
    }

    if (status != 0) { error = buf.empty() ? "evaluator error" : buf; return false; }
    payload.swap(buf);
    return true;
}

}  // namespace fdp
