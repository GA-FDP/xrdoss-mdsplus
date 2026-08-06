#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "EvalClient.hh"
#include "Request.hh"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>   // mkdtemp
#include <cstring>
#include <string>
#include <thread>

namespace {

// Accepts one connection, records the request body, replies (status, payload).
struct StubServer {
    std::string path;
    int         listen_fd = -1;
    std::string received;
    std::thread thr;

    StubServer(unsigned char status, const std::string &payload) {
        char tmpl[] = "/tmp/evalclient_XXXXXX";
        path = std::string(mkdtemp(tmpl)) + "/s.sock";

        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd, 1) == 0);

        thr = std::thread([this, status, payload] {
            const int fd = ::accept(listen_fd, NULL, NULL);
            if (fd < 0) return;
            unsigned char len4[4];
            if (::read(fd, len4, 4) != 4) { ::close(fd); return; }
            const unsigned n = (len4[0] << 24) | (len4[1] << 16) | (len4[2] << 8) | len4[3];
            received.resize(n);
            size_t got = 0;
            while (got < n) {
                const ssize_t r = ::read(fd, &received[got], n - got);
                if (r <= 0) break;
                got += static_cast<size_t>(r);
            }
            std::string out;
            out += static_cast<char>(status);
            const size_t m = payload.size();
            out += static_cast<char>((m >> 24) & 0xFF);
            out += static_cast<char>((m >> 16) & 0xFF);
            out += static_cast<char>((m >>  8) & 0xFF);
            out += static_cast<char>( m        & 0xFF);
            out += payload;
            (void)::write(fd, out.data(), out.size());
            ::close(fd);
        });
    }

    ~StubServer() {
        if (thr.joinable()) thr.join();
        if (listen_fd >= 0) ::close(listen_fd);
        ::unlink(path.c_str());
    }
};

fdp::Request OneExpr(const std::string &exp) {
    fdp::Request r;
    r.items.push_back(fdp::RequestItem{"r0", exp, {}});
    return r;
}

}  // namespace

TEST_CASE("returns the payload on status 0") {
    StubServer stub(0, "SERIALIZED");
    fdp::EvalClient client(stub.path, 5000);

    std::string payload, error;
    CHECK(client.Evaluate("efit01", 190000, OneExpr("\\ipmhd"), payload, error));
    CHECK(payload == "SERIALIZED");
    CHECK(error.empty());
}

TEST_CASE("frames tree, shot and the canonical request body") {
    StubServer stub(0, "X");
    fdp::EvalClient client(stub.path, 5000);

    const fdp::Request req = OneExpr("\\ipmhd");
    std::string payload, error;
    REQUIRE(client.Evaluate("efit01", 190000, req, payload, error));
    stub.thr.join();

    // u16 tree_len | tree | i64 shot | request bytes
    REQUIRE(stub.received.size() > 16);
    const unsigned tree_len = (static_cast<unsigned char>(stub.received[0]) << 8) |
                               static_cast<unsigned char>(stub.received[1]);
    CHECK(tree_len == 6);
    CHECK(stub.received.substr(2, 6) == "efit01");

    long long shot = 0;
    for (int i = 0; i < 8; ++i)
        shot = (shot << 8) | static_cast<unsigned char>(stub.received[8 + i]);
    CHECK(shot == 190000);

    CHECK(stub.received.substr(16) == req.Serialize());
}

TEST_CASE("carries binary arguments through untouched") {
    StubServer stub(0, "X");
    fdp::EvalClient client(stub.path, 5000);

    fdp::Request req;
    req.items.push_back(fdp::RequestItem{"r0", "$ * 2", {std::string("\x00\xff\x01", 3)}});
    std::string payload, error;
    REQUIRE(client.Evaluate("t", 1, req, payload, error));
    stub.thr.join();

    CHECK(stub.received.substr(2 + 1 + 8) == req.Serialize());
}

TEST_CASE("handles an empty tree name") {
    StubServer stub(0, "X");
    fdp::EvalClient client(stub.path, 5000);

    std::string payload, error;
    REQUIRE(client.Evaluate("", 0, OneExpr("1"), payload, error));
    stub.thr.join();

    CHECK(static_cast<unsigned char>(stub.received[0]) == 0);
    CHECK(static_cast<unsigned char>(stub.received[1]) == 0);
}

TEST_CASE("returns a large payload intact") {
    const std::string big(1 << 20, 'z');   // 1 MiB, exercises partial reads
    StubServer stub(0, big);
    fdp::EvalClient client(stub.path, 5000);

    std::string payload, error;
    REQUIRE(client.Evaluate("efit01", 190000, OneExpr("\\psirz"), payload, error));
    CHECK(payload.size() == big.size());
    CHECK(payload == big);
}

TEST_CASE("returns an empty payload without error") {
    StubServer stub(0, "");
    fdp::EvalClient client(stub.path, 5000);

    std::string payload, error;
    CHECK(client.Evaluate("efit01", 1, OneExpr("1"), payload, error));
    CHECK(payload.empty());
    CHECK(error.empty());
}

TEST_CASE("reports an error on status 1") {
    StubServer stub(1, "tree not found");
    fdp::EvalClient client(stub.path, 5000);

    std::string payload, error;
    CHECK_FALSE(client.Evaluate("nope", 1, OneExpr("1"), payload, error));
    CHECK(error == "tree not found");
    CHECK(payload.empty());
}

TEST_CASE("fails cleanly when the socket does not exist") {
    fdp::EvalClient client("/tmp/definitely_not_a_socket_12345", 1000);
    std::string payload, error;
    CHECK_FALSE(client.Evaluate("efit01", 1, OneExpr("1"), payload, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("rejects a socket path that cannot fit in sockaddr_un") {
    fdp::EvalClient client(std::string(200, 'x'), 1000);
    std::string payload, error;
    CHECK_FALSE(client.Evaluate("efit01", 1, OneExpr("1"), payload, error));
    CHECK_FALSE(error.empty());
}
