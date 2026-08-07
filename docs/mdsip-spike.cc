// Spike: can the Oss plugin talk to mdsip with MdsIpShr alone -- no MDSplus
// object model, no treeshr, no TDI -- and get back serialized bytes it can
// write out verbatim as the virtual file's payload?
//
//   spike <host:port> <tree> <shot> <request.bin> <out.bin>
//
// request.bin is the output of GetMany.serialize(), i.e. a serialized APD list
// of {name, exp, args} dictionaries. We never interpret it; we just hand it to
// GetManyExecute($) as an opaque byte array.

#include <mdsdescrip.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
int  ConnectToMds(char *host);
int  MdsOpen(int id, char *tree, int shot);
int  MdsClose(int id);
void DisconnectFromMds(int id);
int  MdsValueDsc(int id, const char *expression, ...);
void MdsIpFree(void *ptr);
}

static std::string ReadFile(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::perror("open request"); std::exit(2); }
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: %s <host:port> <tree> <shot> <request.bin> <out.bin>\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    char *tree = argv[2];
    const int shot = std::atoi(argv[3]);
    const std::string request = ReadFile(argv[4]);

    const int id = ConnectToMds(const_cast<char *>(host));
    // NB: 0 is a VALID connection id; only -1 signals failure (see connection.py).
    if (id == -1) { std::fprintf(stderr, "ConnectToMds(%s) failed\n", host); return 1; }
    std::fprintf(stderr, "connected id=%d\n", id);

    // '-' would mean "no tree"; anything else opens one.
    if (std::strcmp(tree, "-") != 0) {
        const int st = MdsOpen(id, tree, shot);
        if (!(st & 1)) { std::fprintf(stderr, "MdsOpen(%s,%d) status=%d\n", tree, shot, st); return 1; }
        std::fprintf(stderr, "opened %s shot %d\n", tree, shot);
    }

    // The request travels as an opaque byte array. DTYPE_B / CLASS_A over the
    // bytes we were handed -- no MDSplus object model needed on this side.
    struct descriptor_a arg;
    std::memset(&arg, 0, sizeof(arg));
    arg.length   = 1;
    arg.dtype    = DTYPE_B;
    arg.class_   = CLASS_A;
    arg.pointer  = const_cast<char *>(request.data());
    arg.arsize   = static_cast<unsigned int>(request.size());
    arg.dimct    = 1;

    EMPTYXD(answer);
    const int status = MdsValueDsc(id, "GetManyExecute($)", &arg, &answer, NULL);
    if (!(status & 1)) {
        std::fprintf(stderr, "MdsValueDsc status=%d\n", status);
        return 1;
    }
    if (!answer.pointer) { std::fprintf(stderr, "empty answer\n"); return 1; }

    // GetManyExecute already returns SERIALIZED bytes (mdsdata.c:921 does
    // MdsSerializeDscOut before returning), so this is exactly the payload the
    // plugin would hand to XRootD -- no re-serialization on our side.
    struct descriptor_a *ans = reinterpret_cast<struct descriptor_a *>(answer.pointer);
    std::fprintf(stderr, "answer dtype=%d class=%d bytes=%u\n",
                 ans->dtype, ans->class_, ans->arsize);

    FILE *out = std::fopen(argv[5], "wb");
    if (!out) { std::perror("open out"); return 2; }
    std::fwrite(ans->pointer, 1, ans->arsize, out);
    std::fclose(out);

    MdsClose(id);
    DisconnectFromMds(id);
    std::fprintf(stderr, "wrote %u bytes to %s\n", ans->arsize, argv[5]);
    return 0;
}
