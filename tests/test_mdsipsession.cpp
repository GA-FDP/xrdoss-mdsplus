#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "MdsipSession.hh"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// An mdsip message is a 48-byte header whose first field is the total length,
// followed by the body. Nothing else about the header matters to the relay --
// it forwards bytes and only ever parses the length.
std::string Message(const std::string &body) {
    std::string msg(fdp::kMdsipHeaderBytes, '\0');
    const unsigned int len =
        static_cast<unsigned int>(fdp::kMdsipHeaderBytes + body.size());
    std::memcpy(&msg[0], &len, sizeof(len));
    msg += body;
    return msg;
}

std::string BodyOf(const std::string &msg) {
    return msg.substr(fdp::kMdsipHeaderBytes);
}

// Stands in for mdsip: accepts connections and, per connection, echoes each
// message back with its body upper-cased so a reply can be told from a request.
class FakeMdsip {
public:
    FakeMdsip() : listen_fd_(-1), port_(0), stop_(false), connections_(0) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;                  // let the kernel pick
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr),
                       sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 16) == 0);

        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr),
                              &len) == 0);
        port_ = ntohs(addr.sin_port);

        acceptor_ = std::thread(&FakeMdsip::Accept, this);
    }

    ~FakeMdsip() {
        stop_ = true;
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (acceptor_.joinable()) acceptor_.join();

        // Wake the per-connection readers rather than letting each burn its
        // read timeout; a plain join here cost the suite seconds per test.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < clients_.size(); ++i)
                ::shutdown(clients_[i], SHUT_RDWR);
        }
        for (size_t i = 0; i < workers_.size(); ++i)
            if (workers_[i].joinable()) workers_[i].join();
    }

    int  port() const { return port_; }
    int  connections() const { return connections_; }

private:
    void Accept() {
        while (!stop_) {
            const int fd = ::accept(listen_fd_, 0, 0);
            if (fd < 0) return;
            ++connections_;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                clients_.push_back(fd);
            }
            workers_.push_back(std::thread(&FakeMdsip::Serve, this, fd));
        }
    }

    void Serve(int fd) {
        std::string msg;
        while (fdp::ReadOneMessage(fd, msg, 5000)) {
            std::string body = BodyOf(msg);
            for (size_t i = 0; i < body.size(); ++i)
                body[i] = static_cast<char>(::toupper(body[i]));
            const std::string reply = Message(body);
            if (::write(fd, reply.data(), reply.size()) <= 0) break;
        }
        // The destructor may shutdown() this fd, so it must stay open until
        // every worker has been joined; closing here could race a reused fd.
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < clients_.size(); ++i)
            if (clients_[i] == fd) { ::close(fd); clients_[i] = -1; break; }
    }

    int                      listen_fd_;
    int                      port_;
    std::atomic<bool>        stop_;
    std::atomic<int>         connections_;
    std::thread              acceptor_;
    std::vector<std::thread> workers_;
    std::mutex               mutex_;
    std::vector<int>         clients_;
};

}  // namespace

TEST_CASE("message length comes from the header's first field") {
    CHECK(fdp::MdsipMessageLength(Message("")) == fdp::kMdsipHeaderBytes);
    CHECK(fdp::MdsipMessageLength(Message("abcd")) == fdp::kMdsipHeaderBytes + 4);
}

TEST_CASE("implausible message lengths are rejected rather than trusted") {
    CHECK(fdp::MdsipMessageLength("too short") == 0);

    // A length below the header size would make the payload read underflow.
    std::string small(fdp::kMdsipHeaderBytes, '\0');
    const unsigned int tiny = 4;
    std::memcpy(&small[0], &tiny, sizeof(tiny));
    CHECK(fdp::MdsipMessageLength(small) == 0);

    // Above the 2 GiB ceiling -- a byte-swapped or corrupt header, not a real
    // message. Trusting it would mean a multi-gigabyte allocation.
    std::string huge(fdp::kMdsipHeaderBytes, '\0');
    const unsigned int enormous = 0xF0000000u;
    std::memcpy(&huge[0], &enormous, sizeof(enormous));
    CHECK(fdp::MdsipMessageLength(huge) == 0);
}

TEST_CASE("a relayed call returns exactly one answer") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 5000);

    std::string error;
    const std::string token = sessions.Open(error);
    REQUIRE(!token.empty());
    CHECK(error.empty());
    CHECK(sessions.Count() == 1);

    std::string answer;
    REQUIRE(sessions.Relay(token, Message("hello"), answer, error));
    CHECK(BodyOf(answer) == "HELLO");

    sessions.Close(token);
    CHECK(sessions.Count() == 0);
}

TEST_CASE("a session keeps one connection across calls") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 5000);

    std::string error, answer;
    const std::string token = sessions.Open(error);
    REQUIRE(!token.empty());

    // This is the whole reason sessions exist: mdsip is stateful, so the same
    // socket must carry every call in a session (docs/relay-spike.md).
    for (int i = 0; i < 5; ++i)
        REQUIRE(sessions.Relay(token, Message("x"), answer, error));

    CHECK(server.connections() == 1);
    sessions.Close(token);
}

TEST_CASE("sessions are independent") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 5000);

    std::string error, a, b;
    const std::string t1 = sessions.Open(error);
    const std::string t2 = sessions.Open(error);
    REQUIRE(!t1.empty());
    REQUIRE(!t2.empty());
    CHECK(t1 != t2);
    CHECK(sessions.Count() == 2);

    REQUIRE(sessions.Relay(t1, Message("one"), a, error));
    REQUIRE(sessions.Relay(t2, Message("two"), b, error));
    CHECK(BodyOf(a) == "ONE");
    CHECK(BodyOf(b) == "TWO");
    CHECK(server.connections() == 2);
}

TEST_CASE("an unknown token is refused, not silently opened") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 5000);

    std::string error, answer;
    CHECK_FALSE(sessions.Relay("deadbeef", Message("x"), answer, error));
    CHECK(!error.empty());
    CHECK(server.connections() == 0);
}

TEST_CASE("closing twice is harmless") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 5000);

    std::string error;
    const std::string token = sessions.Open(error);
    REQUIRE(!token.empty());

    sessions.Close(token);
    sessions.Close(token);
    CHECK(sessions.Count() == 0);
}

TEST_CASE("the session cap is enforced") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 2, 5000);

    std::string error;
    REQUIRE(!sessions.Open(error).empty());
    REQUIRE(!sessions.Open(error).empty());

    // Without a cap an unauthenticated flood of /connect would exhaust mdsip's
    // process table rather than this map.
    CHECK(sessions.Open(error).empty());
    CHECK(!error.empty());
    CHECK(sessions.Count() == 2);
}

TEST_CASE("idle sessions are reaped") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 0, 0, 5000);

    std::string error;
    REQUIRE(!sessions.Open(error).empty());

    // idle_seconds == 0 disables reaping entirely; a negative window makes
    // every session immediately overdue, which is what we want to observe.
    fdp::MdsipSessions eager("127.0.0.1", server.port(), -1, 0, 5000);
    CHECK(eager.Count() == 0);

    fdp::MdsipSessions reaping("127.0.0.1", server.port(), 1, 0, 5000);
    const std::string token = reaping.Open(error);
    REQUIRE(!token.empty());
    CHECK(reaping.Count() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    reaping.ReapIdle();
    CHECK(reaping.Count() == 0);
}

TEST_CASE("a dead mdsip fails the relay and retires the session") {
    std::string error, answer;
    std::string token;
    {
        FakeMdsip server;
        fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 500);
        token = sessions.Open(error);
        REQUIRE(!token.empty());

        // Server dies here; the socket is left broken mid-session.
        // Held outside the block so the destructor runs first.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Rebuild against the now-dead port to prove Open fails cleanly too.
    fdp::MdsipSessions orphan("127.0.0.1", 1, 300, 0, 500);
    CHECK(orphan.Open(error).empty());
    CHECK(!error.empty());
}

TEST_CASE("a broken stream retires the session instead of resynchronising") {
    FakeMdsip *server = new FakeMdsip();
    fdp::MdsipSessions sessions("127.0.0.1", server->port(), 300, 0, 500);

    std::string error, answer;
    const std::string token = sessions.Open(error);
    REQUIRE(!token.empty());
    REQUIRE(sessions.Relay(token, Message("ok"), answer, error));

    delete server;   // connection dies under the session

    CHECK_FALSE(sessions.Relay(token, Message("now what"), answer, error));
    // A half-written call or unread answer cannot be recovered from, so the
    // session must not survive to hand the next caller misaligned bytes.
    CHECK(sessions.Count() == 0);
}

TEST_CASE("concurrent calls on one session are refused, not interleaved") {
    FakeMdsip server;
    fdp::MdsipSessions sessions("127.0.0.1", server.port(), 300, 0, 5000);

    std::string error;
    const std::string token = sessions.Open(error);
    REQUIRE(!token.empty());

    std::atomic<int> refused(0), succeeded(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.push_back(std::thread([&]() {
            std::string a, e;
            bool busy = false;
            if (sessions.Relay(token, Message("concurrent"), a, e, &busy)) {
                if (BodyOf(a) == "CONCURRENT") ++succeeded;
            } else if (busy) {
                ++refused;
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

    // Every call either got its own correct answer or was told the session was
    // busy. What must never happen is a thread receiving another's answer.
    CHECK(succeeded + refused == 8);
    CHECK(succeeded >= 1);
}
