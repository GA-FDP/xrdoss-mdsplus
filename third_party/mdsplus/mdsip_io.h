/* Minimal declarations for writing an out-of-tree mdsip transport.
 *
 * MDSplus installs no mdsip headers, and mdsip_connections.h pulls in five more
 * (socket_port.h, ipdesc.h, mds_stdarg.h, ...) for declarations this transport
 * does not use. What it genuinely needs is small: the IoRoutines vtable, an
 * opaque Connection, and the two accessors for per-connection state.
 *
 * Provenance: MDSplus mdstcpip/mdsip_connections.h (alpha 7.158). The
 * IoRoutines struct below is COPIED VERBATIM -- the order of those ten
 * pointers is the ABI. Do not tidy it, and re-check it when moving MDSplus
 * versions.
 *
 * Deliberately opaque Connection: this transport never dereferences one, which
 * keeps it off the struct layout entirely. All per-connection state goes
 * through ConnectionSetInfo/ConnectionGetInfo, which is what the in-tree TCP
 * and GSI transports do.
 */
#ifndef FDP_MDSIP_IO_H
#define FDP_MDSIP_IO_H

#include <stddef.h>
#include <sys/types.h> /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* socket_port.h: typedef int SOCKET; (POSIX) */
typedef int FdpSocket;

typedef struct _connection Connection;

typedef struct _io_routines
{
  int (*connect)(Connection *c, char *protocol, char *connectString);
  ssize_t (*send)(Connection *c, const void *buffer, size_t buflen, int nowait);
  ssize_t (*recv)(Connection *c, void *buffer, size_t buflen);
  int (*flush)(Connection *c);
  int (*listen)(int argc, char **argv);
  int (*authorize)(Connection *c, char *username);
  int (*reuseCheck)(char *connectString, char *uniqueString, size_t buflen);
  int (*disconnect)(Connection *c);
  ssize_t (*recv_to)(Connection *c, void *buffer, size_t len, int to_msec);
  int (*check)(Connection *c);
} IoRoutines;

/* Implemented in libMdsIpShr, which is already loaded in any process that
 * reached us -- LoadIo() lives there. ConnectionSetInfo COPIES `len` bytes,
 * so the pointer-stash pattern is: pass &ptr with len = sizeof(ptr), and
 * dereference what ConnectionGetInfo hands back. */
extern void ConnectionSetInfo(Connection *c, char *info_name, FdpSocket readfd,
                              void *info, size_t len);
extern void *ConnectionGetInfo(Connection *c, char **info_name,
                               FdpSocket *readfd, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
