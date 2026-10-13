#include "MdsipSession.hh"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace fdp {

namespace {

bool ReadFully(int fd, char *buf, size_t len) {
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

bool WriteFully(int fd, const char *buf, size_t len) {
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

std::string RandomToken() {
    unsigned char raw[16];
    FILE *f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        if (f) std::fclose(f);
        return std::string();
    }
    std::fclose(f);
    char hex[33];
    for (size_t i = 0; i < sizeof(raw); ++i)
        std::snprintf(hex + i * 2, 3, "%02x", raw[i]);
    return std::string(hex, 32);
}

}  // namespace

size_t MdsipMessageLength(const std::string &hdr) {
    if (hdr.size() < kMdsipHeaderBytes) return 0;
    // msglen is the first field of MsgHdr, host byte order on the wire for a
    // same-endian peer. mdsip itself tolerates either and byte-swaps; we only
    // need the length, so reject anything that cannot be a real message rather
    // than guessing at endianness.
    unsigned int len;
    std::memcpy(&len, hdr.data(), sizeof(len));
    if (len < kMdsipHeaderBytes) return 0;
    if (len > (1u << 31)) return 0;      // 2 GiB ceiling; MDSplus caps at 4 GiB
    return len;
}

bool ResolveMdsipTarget(const std::string &host_parm,
                        const std::string &port_parm,
                        const std::string &socket_parm,
                        MdsipTarget &out, std::string &error) {
    out = MdsipTarget();
    error.clear();
    if (!socket_parm.empty()) {
        if (!host_parm.empty() || !port_parm.empty()) {
            error = "socket= cannot be combined with host= or port=";
            return false;
        }
        out.socket_path = socket_parm;
        return true;
    }
    out.host = host_parm.empty() ? std::string("localhost") : host_parm;
    if (port_parm.empty()) {
        out.port = 8000;
        return true;
    }
    char *end = 0;
    const long port = std::strtol(port_parm.c_str(), &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535) {
        error = "invalid mdsip port " + port_parm;
        return false;
    }
    out.port = static_cast<int>(port);
    return true;
}

bool ReadOneMessage(int fd, std::string &out, int timeout_ms) {
    out.clear();

    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string hdr(kMdsipHeaderBytes, '\0');
    if (!ReadFully(fd, &hdr[0], kMdsipHeaderBytes)) return false;

    const size_t total = MdsipMessageLength(hdr);
    if (total == 0) return false;

    out.swap(hdr);
    if (total > kMdsipHeaderBytes) {
        const size_t rest = total - kMdsipHeaderBytes;
        out.resize(total);
        if (!ReadFully(fd, &out[kMdsipHeaderBytes], rest)) { out.clear(); return false; }
    }
    return true;
}

MdsipSessions::MdsipSessions(const MdsipTarget &target, int idle_seconds,
                             size_t max_sessions, int timeout_ms)
    : target_(target), idle_seconds_(idle_seconds),
      max_sessions_(max_sessions), timeout_ms_(timeout_ms) {}

namespace {
MdsipTarget TcpTarget(std::string host, int port) {
    MdsipTarget t;
    t.host = std::move(host);
    t.port = port;
    return t;
}
}  // namespace

MdsipSessions::MdsipSessions(std::string host, int port, int idle_seconds,
                             size_t max_sessions, int timeout_ms)
    : MdsipSessions(TcpTarget(std::move(host), port), idle_seconds,
                    max_sessions, timeout_ms) {}

MdsipSessions::~MdsipSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::map<std::string, Session>::iterator it = sessions_.begin();
         it != sessions_.end(); ++it)
        ::close(it->second.fd);
    sessions_.clear();
}

void MdsipSessions::Retire(const std::map<std::string, Session>::iterator &it) {
    if (it->second.busy) { it->second.doomed = true; return; }
    ::close(it->second.fd);
    sessions_.erase(it);
}

std::string MdsipSessions::Open(std::string &error) {
    error.clear();
    ReapIdle();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (max_sessions_ && sessions_.size() >= max_sessions_) {
            error = "too many open mdsip sessions";
            return std::string();
        }
    }

    int fd = -1;
    if (!target_.socket_path.empty()) {
        // One connect, no name resolution, no Nagle to disable: a Unix socket
        // has neither. The path length check is the only way this can be
        // misconfigured, and it is caught at load as well as here.
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (target_.socket_path.size() >= sizeof(addr.sun_path)) {
            error = "mdsip socket path too long: " + target_.socket_path;
            return std::string();
        }
        std::strncpy(addr.sun_path, target_.socket_path.c_str(),
                     sizeof(addr.sun_path) - 1);
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr *>(&addr),
                                 sizeof(addr)) != 0) {
            ::close(fd);
            fd = -1;
        }
        if (fd < 0) {
            error = "cannot connect to mdsip at " + target_.socket_path;
            return std::string();
        }
    } else {
        char portbuf[16];
        std::snprintf(portbuf, sizeof(portbuf), "%d", target_.port);

        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *res = 0;
        if (::getaddrinfo(target_.host.c_str(), portbuf, &hints, &res) != 0 || !res) {
            error = "cannot resolve mdsip host " + target_.host;
            return std::string();
        }
        for (addrinfo *ai = res; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);
        if (fd < 0) {
            error = "cannot connect to mdsip at " + target_.host;
            return std::string();
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    const std::string token = RandomToken();
    if (token.empty()) { ::close(fd); error = "cannot generate a session token"; return std::string(); }

    std::lock_guard<std::mutex> lock(mutex_);
    Session s;
    s.fd        = fd;
    s.last_used = ::time(0);
    s.busy      = false;
    s.doomed    = false;
    sessions_[token] = s;
    return token;
}

bool MdsipSessions::Relay(const std::string &token, const std::string &request,
                          std::string &answer, std::string &error, bool *busy) {
    answer.clear();
    error.clear();
    if (busy) *busy = false;

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::map<std::string, Session>::iterator it = sessions_.find(token);
        if (it == sessions_.end()) { error = "unknown or expired session"; return false; }
        if (it->second.busy) {
            error = "session already has a call in flight";
            if (busy) *busy = true;
            return false;
        }
        it->second.busy      = true;
        it->second.last_used = ::time(0);
        fd = it->second.fd;
    }

    // One call up, one answer down. The network round trip happens outside the
    // lock -- holding it would stall every other session -- which is safe only
    // because `busy` reserves this fd for the duration.
    bool ok = true;
    if (!WriteFully(fd, request.data(), request.size())) {
        error = "mdsip connection lost while sending";
        ok = false;
    } else if (!ReadOneMessage(fd, answer, timeout_ms_)) {
        error = "mdsip did not answer";
        ok = false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, Session>::iterator it = sessions_.find(token);
    if (it != sessions_.end()) {
        it->second.busy      = false;
        it->second.last_used = ::time(0);
        // A broken stream cannot be resynchronised: a half-written call or an
        // unread answer leaves the next caller reading the wrong bytes.
        if (!ok || it->second.doomed) Retire(it);
    }
    return ok;
}

void MdsipSessions::Close(const std::string &token) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, Session>::iterator it = sessions_.find(token);
    if (it == sessions_.end()) return;
    Retire(it);
}

void MdsipSessions::ReapIdle() {
    if (idle_seconds_ <= 0) return;
    const time_t now = ::time(0);

    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, Session>::iterator it = sessions_.begin();
    while (it != sessions_.end()) {
        // A busy session is by definition not idle, and its fd belongs to the
        // relayer; skip it rather than reaping the socket out from under it.
        if (!it->second.busy && now - it->second.last_used > idle_seconds_) {
            const std::map<std::string, Session>::iterator dead = it++;
            ::close(dead->second.fd);
            sessions_.erase(dead);
        } else {
            ++it;
        }
    }
}

size_t MdsipSessions::Count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace fdp
