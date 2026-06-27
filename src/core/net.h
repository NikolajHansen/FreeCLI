#ifndef NET_H
#define NET_H

/*
 * Cross-platform socket portability shim (Linux + Windows/MSYS2).
 */

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
  typedef SOCKET net_fd_t;
# define NET_INVALID  INVALID_SOCKET
# define net_close(s) closesocket(s)
  static inline int  net_init(void)    { WSADATA w; return WSAStartup(MAKEWORD(2,2),&w) == 0; }
  static inline void net_cleanup(void) { WSACleanup(); }
  static inline int  net_set_nonblock(net_fd_t s) {
      u_long m = 1; return ioctlsocket(s, FIONBIO, &m) == 0;
  }
  static inline int  net_set_block(net_fd_t s) {
      u_long m = 0; return ioctlsocket(s, FIONBIO, &m) == 0;
  }
  static inline int  net_poll_read(net_fd_t s, int timeout_ms) {
      WSAPOLLFD pfd = { s, POLLIN, 0 };
      return WSAPoll(&pfd, 1, timeout_ms) > 0;
  }
  static inline int net_would_block(void) {
      int e = WSAGetLastError();
      return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
  }
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <sys/time.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
# include <poll.h>
  typedef int net_fd_t;
# define NET_INVALID  (-1)
# define net_close(s) close(s)
  static inline int  net_init(void)    { return 1; }
  static inline void net_cleanup(void) {}
  static inline int  net_set_nonblock(net_fd_t s) {
      int fl = fcntl(s, F_GETFL, 0);
      return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
  }
  static inline int  net_set_block(net_fd_t s) {
      int fl = fcntl(s, F_GETFL, 0);
      return fl >= 0 && fcntl(s, F_SETFL, fl & ~O_NONBLOCK) == 0;
  }
  static inline int  net_poll_read(net_fd_t s, int timeout_ms) {
      struct pollfd pfd = { s, POLLIN, 0 };
      return poll(&pfd, 1, timeout_ms) > 0;
  }
  static inline int net_would_block(void) {
      return errno == EAGAIN || errno == EWOULDBLOCK;
  }
#endif

/* Reliable send: retries until all bytes sent. Returns 0 on success, -1 on error. */
static inline int net_send_all(net_fd_t s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = (int)send(s, p, (int)len, 0);
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* Reliable recv: blocks until all bytes received. Returns 0 on success, -1 on error/disconnect. */
static inline int net_recv_all(net_fd_t s, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        int n = (int)recv(s, p, (int)len, 0);
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

#endif /* NET_H */
