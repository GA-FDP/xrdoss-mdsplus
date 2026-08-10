#include "MdsIpClient.hh"

#include <mdsdescrip.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
int  ConnectToMds(char *host);
int  MdsOpen(int id, char *tree, int shot);
int  MdsClose(int id);
void DisconnectFromMds(int id);
int  MdsIpGetConnectionVersion(int id);
int  SendDsc(int id, unsigned char idx, unsigned char nargs, struct descriptor *dsc);
int  GetAnswerInfoTO(int id, char *dtype, short *length, char *ndims, int *dims,
                     int *numbytes, void **dptr, void **m, int timeout);
int  MdsSerializeDscIn(char const *in, struct descriptor_xd *out);
int  MdsFree1Dx(struct descriptor_xd *out, void *zero);
}

// From MDSplus's ipdesc.h, which is not among the vendored headers. mdsip
// wraps an answer in its own serialization and tags it with this dtype.
#define FDP_DTYPE_SERIAL 24

// MDSplus's own STATUS_OK (status.h) is a bare macro referencing a variable
// that must literally be named `status`. Use an explicit helper instead of
// contorting local names around it.
static inline bool MdsOk(int s) { return (s & 1) != 0; }

namespace {

// Disconnects on every exit path, including early returns.
struct Connection {
    int  id;
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

MdsIpClient::MdsIpClient(std::string server, int timeout_ms, size_t max_result_bytes)
    : server_(std::move(server)),
      timeout_ms_(timeout_ms),
      max_result_bytes_(max_result_bytes) {}

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

    // SendDsc has existed since MDSIP_VERSION_DSC_ARGS (1); current is 3. A
    // server too old for it would need the legacy SendArg dance, which we do
    // not implement -- refuse clearly instead of misbehaving.
    const int version = MdsIpGetConnectionVersion(conn.id);
    if (version < 1) {
        error = "mdsip server too old (protocol version " +
                std::to_string(version) + "); need >= 1 for descriptor args";
        return false;
    }

    if (!tree.empty()) {
        const int st = MdsOpen(conn.id, const_cast<char *>(tree.c_str()),
                               static_cast<int>(shot));
        if (!MdsOk(st)) {
            error = "cannot open tree " + tree + " shot " + std::to_string(shot);
            return false;
        }
        conn.tree_open = true;
    }

    // Two arguments: the expression, then the request as an opaque byte array.
    // MDSplus is the only thing that interprets the second one.
    DESCRIPTOR_FROM_CSTRING(expr_dsc, "GetManyExecute($)");

    struct descriptor_a arg;
    std::memset(&arg, 0, sizeof(arg));
    arg.length  = 1;
    arg.dtype   = DTYPE_B;
    arg.class_  = CLASS_A;
    arg.pointer = const_cast<char *>(request.data());
    arg.arsize  = static_cast<unsigned int>(request.size());
    arg.dimct   = 1;

    int st = SendDsc(conn.id, 0, 2, reinterpret_cast<struct descriptor *>(&expr_dsc));
    if (MdsOk(st))
        st = SendDsc(conn.id, 1, 2, reinterpret_cast<struct descriptor *>(&arg));
    if (!MdsOk(st)) { error = "failed sending request to mdsip"; return false; }

    // GetAnswerInfoTO rather than MdsValueDsc because it takes a timeout;
    // MdsValueDsc does not, so a slow or hostile expression would block an
    // XRootD thread indefinitely.
    char   dtype = 0, ndims = 0;
    short  length = 0;
    int    dims[MAX_DIMS] = {0};
    int    numbytes = 0;
    void  *dptr = 0;
    void  *mem = 0;

    st = GetAnswerInfoTO(conn.id, &dtype, &length, &ndims, dims, &numbytes,
                         &dptr, &mem, timeout_ms_);
    if (!MdsOk(st)) {
        if (mem) std::free(mem);
        error = "mdsip did not answer within " + std::to_string(timeout_ms_) +
                " ms (or returned an error)";
        return false;
    }

    if (!dptr || numbytes <= 0) {
        if (mem) std::free(mem);
        error = "mdsip returned an empty answer";
        return false;
    }

    // mdsip wraps the answer in ITS OWN serialization, so what arrives is a
    // serialized descriptor whose contents are the bytes GetManyExecute
    // produced. Peel exactly one layer -- the same thing MdsValueDsc does
    // internally (MdsValue.c). Taking dptr verbatim yields the outer wrapper
    // and silently wrong data.
    if (dtype != FDP_DTYPE_SERIAL && dtype != DTYPE_B) {
        if (mem) std::free(mem);
        error = "unexpected answer dtype " + std::to_string(static_cast<int>(dtype));
        return false;
    }

    EMPTYXD(inner_xd);
    const int dst = MdsSerializeDscIn(static_cast<char *>(dptr), &inner_xd);
    if (mem) std::free(mem);
    if (!MdsOk(dst) || !inner_xd.pointer) {
        MdsFree1Dx(&inner_xd, 0);
        error = "could not deserialize the mdsip answer";
        return false;
    }

    const struct descriptor_a *inner =
        reinterpret_cast<const struct descriptor_a *>(inner_xd.pointer);
    if (inner->class_ != CLASS_A || !inner->pointer) {
        MdsFree1Dx(&inner_xd, 0);
        error = "unexpected answer descriptor class";
        return false;
    }

    // A cap applied here cannot prevent the bytes being received -- the API
    // gives no way to bound that -- but it stops an absurd result being cached
    // and served, which is what actually costs the origin.
    if (max_result_bytes_ && inner->arsize > max_result_bytes_) {
        const unsigned int got = inner->arsize;
        MdsFree1Dx(&inner_xd, 0);
        error = "result of " + std::to_string(got) + " bytes exceeds limit " +
                std::to_string(max_result_bytes_);
        return false;
    }

    payload.assign(inner->pointer, inner->arsize);
    MdsFree1Dx(&inner_xd, 0);
    return true;
}

}  // namespace fdp
