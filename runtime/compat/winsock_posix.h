/* The Winsock calls of nnsocket.inc on BSD sockets (macOS), so that file builds unchanged.
 *
 * WSAGetLastError() is errno, and every WSAE* constant is the macOS errno of the same meaning, so nnsocket.inc's own
 * table from Winsock errors to the console's (Linux-numbered) errors maps macOS's errors too: EWOULDBLOCK is 35 here,
 * WSAEWOULDBLOCK 10035 on Windows, 11 on the console.
 *
 * Every socket made or accepted here has SO_NOSIGPIPE: a send on a connection the peer closed raises SIGPIPE on macOS,
 * which ends the process; Windows has no such signal and returns an error, as here. */
#ifndef RECOMP_WINSOCK_POSIX_H
#define RECOMP_WINSOCK_POSIX_H
#ifndef _WIN32

#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef int SOCKET;
typedef unsigned long u_long;
typedef unsigned short u_short;
typedef struct linger LINGER;
typedef struct pollfd WSAPOLLFD;
typedef struct { int unused; } WSADATA;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define MAKEWORD(lo, hi) ((unsigned short)(((lo) & 0xff) | (((hi) & 0xff) << 8)))

static inline int WSAStartup(unsigned short version, WSADATA *d) { (void)version; (void)d; return 0; }
static inline int WSACleanup(void) { return 0; }
static inline int WSAGetLastError(void) { return errno; }
static inline int closesocket(SOCKET s) { return close(s); }
static inline int ioctlsocket(SOCKET s, unsigned long cmd, u_long *v) { int i = (int)*v; int r = ioctl(s, cmd, &i); *v = (u_long)i; return r; }
#define InetPtonA inet_pton
static inline int WSAPoll(WSAPOLLFD *fds, unsigned long n, int timeout) { return poll(fds, (nfds_t)n, timeout); }

static inline SOCKET ws_nosigpipe(SOCKET s) {
    if (s >= 0) { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
    return s;
}
/* Winsock's lengths are int; BSD's are socklen_t (unsigned): same size, so the pointers are passed as they are. */
#define socket(af, type, proto) ws_nosigpipe(socket((af), (type), (proto)))
#define accept(s, a, l) ws_nosigpipe(accept((s), (a), (socklen_t *)(l)))
#define getpeername(s, a, l) getpeername((s), (a), (socklen_t *)(l))
#define getsockname(s, a, l) getsockname((s), (a), (socklen_t *)(l))
#define recvfrom(s, b, n, f, a, l) recvfrom((s), (b), (size_t)(n), (f), (a), (socklen_t *)(l))
#define getsockopt(s, lv, nm, v, l) getsockopt((s), (lv), (nm), (v), (socklen_t *)(l))

#define WSAEINTR EINTR
#define WSAEBADF EBADF
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEINVAL EINVAL
#define WSAEMFILE EMFILE
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAESOCKTNOSUPPORT ESOCKTNOSUPPORT
#define WSAEOPNOTSUPP EOPNOTSUPP
#define WSAEPFNOSUPPORT EPFNOSUPPORT
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAECONNRESET ECONNRESET
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTDOWN EHOSTDOWN
#define WSAEHOSTUNREACH EHOSTUNREACH

#endif
#endif
