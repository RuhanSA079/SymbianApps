/*
 * P.I.P.S. ships no <netinet/tcp.h> and no IPPROTO_* constants. Its
 * setsockopt() hands level/name straight to RSocket::SetOpt(), the way its
 * own SO_KEEPALIVE (= KSoTcpKeepAlive, 0x305) works, so use the native
 * Symbian values from in_sock.h. PuTTY ignores setsockopt() failures.
 */
#ifndef RSSH_NETINET_TCP_H
#define RSSH_NETINET_TCP_H
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 0x106   /* KSolInetTcp */
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 0x304   /* KSoTcpNoDelay */
#endif
#endif
