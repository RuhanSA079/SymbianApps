/*
 * rssh_sock.h: native Symbian TCP sockets (symbian/sock.cpp) for the C
 * network layer (symbian/network.c). One rssh_sock per connection; events
 * arrive from active objects on the UI thread. After rssh_sock_close()
 * returns, no further events are delivered for that socket.
 */
#ifndef RSSH_SOCK_H
#define RSSH_SOCK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rssh_sock rssh_sock;

typedef struct rssh_sock_events {
    void (*connected)(void *ctx, int err);          /* err: 0 or Symbian error */
    void (*received)(void *ctx, const void *data, int len);
    void (*sent)(void *ctx);                        /* the last write finished */
    void (*closed)(void *ctx, int err);             /* 0 = remote EOF */
} rssh_sock_events;

/* Resolve host (name or dotted quad) to IPv4, synchronously. 0 or error. */
int rssh_resolve(const char *host, unsigned char ip[4]);
/* Start connecting; events->connected follows. NULL if out of memory. */
rssh_sock *rssh_sock_connect(const unsigned char ip[4], int port, int nodelay,
                             const rssh_sock_events *events, void *ctx);
/* Start an asynchronous write (data is copied). Only one at a time:
 * wait for events->sent before the next. 0 or error. */
int rssh_sock_write(rssh_sock *s, const void *data, int len);
/* Half-close once pending writes are done. */
void rssh_sock_shutdown_write(rssh_sock *s);
void rssh_sock_set_frozen(rssh_sock *s, int frozen);
void rssh_sock_close(rssh_sock *s);
/* Text for a Symbian error code (static string). */
const char *rssh_sock_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
