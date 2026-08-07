#include "MdsIpClient.hh"

#include <mdsdescrip.h>

#include <cstring>
#include <utility>

extern "C" {
int  ConnectToMds(char *host);
int  MdsOpen(int id, char *tree, int shot);
int  MdsClose(int id);
void DisconnectFromMds(int id);
int  MdsValueDsc(int id, const char *expression, ...);
void MdsIpFree(void *ptr);
}

namespace {

// Disconnects on every exit path, including early returns.
struct Connection {
    int id;
    bool tree_open;
    explicit Connection(int i) : id(i), tree_open(false) {}
    ~Connection() {
        if (id == -1) return;
        if (tree_open) MdsClose(id);
        DisconnectFromMds(id);
    }
};

}  // namespace

namespace fdp {

MdsIpClient::MdsIpClient(std::string server, int timeout_ms)
    : server_(std::move(server)), timeout_ms_(timeout_ms) {}

bool MdsIpClient::Evaluate(const std::string &tree, long long shot,
                           const std::string &request,
                           std::string &payload, std::string &error) const {
    payload.clear();
    error.clear();

    if (request.empty()) { error = "empty request"; return false; }

    // NB: 0 is a VALID connection id here; only -1 signals failure. Checking
    // "<= 0" silently refuses every first connection.
    Connection conn(ConnectToMds(const_cast<char *>(server_.c_str())));
    if (conn.id == -1) {
        error = "ConnectToMds(" + server_ + ") failed";
        return false;
    }

    if (!tree.empty()) {
        const int st = MdsOpen(conn.id, const_cast<char *>(tree.c_str()),
                               static_cast<int>(shot));
        if (!(st & 1)) {
            error = "cannot open tree " + tree + " shot " + std::to_string(shot);
            return false;
        }
        conn.tree_open = true;
    }

    // The request travels as an opaque byte array; MDSplus is the only thing
    // that interprets its contents.
    struct descriptor_a arg;
    std::memset(&arg, 0, sizeof(arg));
    arg.length  = 1;
    arg.dtype   = DTYPE_B;
    arg.class_  = CLASS_A;
    arg.pointer = const_cast<char *>(request.data());
    arg.arsize  = static_cast<unsigned int>(request.size());
    arg.dimct   = 1;

    EMPTYXD(answer);
    const int status = MdsValueDsc(conn.id, "GetManyExecute($)", &arg, &answer, NULL);
    if (!(status & 1)) {
        error = "GetManyExecute failed with status " + std::to_string(status);
        return false;
    }
    if (!answer.pointer) { error = "evaluator returned an empty answer"; return false; }

    const struct descriptor_a *ans =
        reinterpret_cast<const struct descriptor_a *>(answer.pointer);
    if (ans->class_ != CLASS_A || !ans->pointer) {
        error = "unexpected answer descriptor class";
        MdsIpFree(answer.pointer);
        return false;
    }

    payload.assign(ans->pointer, ans->arsize);
    MdsIpFree(answer.pointer);
    return true;
}

}  // namespace fdp
