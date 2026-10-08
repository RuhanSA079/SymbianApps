/*
 * rsym_tcp.h: blocking TCP client sockets on native Symbian RSocket, for use
 * from a worker thread (every call blocks the calling thread). Each
 * connection has its own socket-server session, so connections may be used
 * from any thread, one thread at a time.
 */
#ifndef RSYM_TCP_H
#define RSYM_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsym_tcp rsym_tcp;

#define RSYM_TCP_TIMEOUT (-33)          /* = KErrTimedOut */

/* Resolve host (name or dotted quad) and connect. NULL on failure, with *err
 * set to a (negative) Symbian error code. */
rsym_tcp *rsym_tcp_connect(const char *host, int port, int timeout_ms, int *err);
/* Send all of buf. Returns len, or a negative error. */
int rsym_tcp_send(rsym_tcp *t, const void *buf, int len, int timeout_ms);
/* Receive up to len bytes: >0 bytes read, 0 = connection closed by peer,
 * <0 error (RSYM_TCP_TIMEOUT on timeout). */
int rsym_tcp_recv(rsym_tcp *t, void *buf, int len, int timeout_ms);
void rsym_tcp_close(rsym_tcp *t);
const char *rsym_tcp_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
