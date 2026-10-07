/*
 * network.c: PuTTY's network interface (network.h) on native Symbian
 * sockets (rssh_sock.h / sock.cpp). Replaces unix/network.c, whose
 * select()/non-blocking use relies on socket ioctls that EKA2L1 lacks.
 * IPv4 TCP client connections only.
 */

#include <stdio.h>
#include <string.h>
#include "putty.h"
#include "network.h"
#include "rssh_sock.h"
#include "rssh_trace.h"

/* ------------------------------------------------------------------ */
/* addresses                                                            */
/* ------------------------------------------------------------------ */

struct SockAddr {
    int refcount;
    const char *error;          /* static string, or NULL */
    bool resolved;
    unsigned char ip[4];
    char hostname[256];         /* for ADDRTYPE_NAME / logging */
};

void sk_init(void) {}
void sk_cleanup(void) {}

SockAddr *sk_namelookup(const char *host, char **canonicalname,
                        int address_family)
{
    SockAddr *addr = snew(SockAddr);
    int err;

    memset(addr, 0, sizeof(*addr));
    addr->refcount = 1;
    strncpy(addr->hostname, host, sizeof(addr->hostname) - 1);
    err = rssh_resolve(host, addr->ip);
    if (err == 0) {
        addr->resolved = true;
    } else {
        addr->error = rssh_sock_strerror(err);
    }
    *canonicalname = dupstr(host);
    return addr;
}

SockAddr *sk_nonamelookup(const char *host)
{
    SockAddr *addr = snew(SockAddr);
    memset(addr, 0, sizeof(*addr));
    addr->refcount = 1;
    strncpy(addr->hostname, host, sizeof(addr->hostname) - 1);
    return addr;
}

const char *sk_addr_error(SockAddr *addr) { return addr->error; }

void sk_getaddr(SockAddr *addr, char *buf, int buflen)
{
    if (addr->resolved)
        snprintf(buf, buflen, "%d.%d.%d.%d",
                 addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3]);
    else
        snprintf(buf, buflen, "%s", addr->hostname);
}

bool sk_addr_needs_port(SockAddr *addr) { return true; }

bool sk_hostname_is_local(const char *name)
{
    return !strcmp(name, "localhost") || !strcmp(name, "127.0.0.1");
}

bool sk_address_is_local(SockAddr *addr)
{
    return addr->resolved && addr->ip[0] == 127;
}

bool sk_address_is_special_local(SockAddr *addr) { return false; }

int sk_addrtype(SockAddr *addr)
{
    return addr->resolved ? ADDRTYPE_IPV4 : ADDRTYPE_NAME;
}

void sk_addrcopy(SockAddr *addr, char *buf)
{
    memcpy(buf, addr->ip, 4);
}

SockAddr *sk_addr_dup(SockAddr *addr)
{
    addr->refcount++;
    return addr;
}

void sk_addr_free(SockAddr *addr)
{
    if (--addr->refcount <= 0)
        sfree(addr);
}

int net_service_lookup(const char *service)
{
    if (!strcmp(service, "ssh")) return 22;
    if (!strcmp(service, "telnet")) return 23;
    if (!strcmp(service, "http")) return 80;
    return 0;
}

char *get_hostname(void) { return dupstr("rssh"); }

void *sk_getxdmdata(Socket *sock, int *lenp) { return NULL; }

/* ------------------------------------------------------------------ */
/* sockets                                                              */
/* ------------------------------------------------------------------ */

static int trace_budget = 60;     /* limit trace lines */

typedef struct NetSocket {
    Plug *plug;
    SockAddr *addr;
    int port;
    rssh_sock *rs;
    bufchain output;
    bool writing;
    bool connected;
    bool frozen;
    bool want_eof;
    const char *error;
    Socket sock;
} NetSocket;

static void net_try_send(NetSocket *ns)
{
    ptrlen data;
    if (!ns->rs || !ns->connected || ns->writing || !bufchain_size(&ns->output))
        return;
    data = bufchain_prefix(&ns->output);
    if (data.len > 4096)
        data.len = 4096;
    if (trace_budget > 0 && trace_budget--)
        rssh_trace("net: send %d", (int)data.len);
    if (rssh_sock_write(ns->rs, data.ptr, (int)data.len) == 0) {
        ns->writing = true;
        bufchain_consume(&ns->output, data.len);
    }
}

static void ev_connected(void *ctx, int err)
{
    NetSocket *ns = (NetSocket *)ctx;
    if (err) {
        ns->error = rssh_sock_strerror(err);
        plug_log(ns->plug, &ns->sock, PLUGLOG_CONNECT_FAILED, ns->addr,
                 ns->port, ns->error, err);
        plug_closing_error(ns->plug, ns->error);
        return;
    }
    ns->connected = true;
    plug_log(ns->plug, &ns->sock, PLUGLOG_CONNECT_SUCCESS, ns->addr,
             ns->port, NULL, 0);
    net_try_send(ns);
}

static void ev_received(void *ctx, const void *data, int len)
{
    NetSocket *ns = (NetSocket *)ctx;
    if (trace_budget > 0 && trace_budget--)
        rssh_trace("net: recv %d", len);
    plug_receive(ns->plug, 0, (const char *)data, len);
}

static void ev_sent(void *ctx)
{
    NetSocket *ns = (NetSocket *)ctx;
    ns->writing = false;
    net_try_send(ns);
    if (!ns->writing && ns->want_eof && ns->rs)
        rssh_sock_shutdown_write(ns->rs);
    plug_sent(ns->plug, bufchain_size(&ns->output));
}

static void ev_closed(void *ctx, int err)
{
    NetSocket *ns = (NetSocket *)ctx;
    if (err) {
        ns->error = rssh_sock_strerror(err);
        plug_closing_error(ns->plug, ns->error);
    } else {
        plug_closing_normal(ns->plug);
    }
}

static const rssh_sock_events net_events = {
    ev_connected, ev_received, ev_sent, ev_closed,
};

static Plug *net_plug(Socket *s, Plug *p)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    Plug *old = ns->plug;
    if (p)
        ns->plug = p;
    return old;
}

static void net_close(Socket *s)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    if (ns->rs)
        rssh_sock_close(ns->rs);     /* no more events after this */
    bufchain_clear(&ns->output);
    sk_addr_free(ns->addr);
    sfree(ns);
}

static size_t net_write(Socket *s, const void *data, size_t len)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    bufchain_add(&ns->output, data, len);
    net_try_send(ns);
    return bufchain_size(&ns->output);
}

static size_t net_write_oob(Socket *s, const void *data, size_t len)
{
    return net_write(s, data, len);  /* no urgent data on Symbian */
}

static void net_write_eof(Socket *s)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    ns->want_eof = true;
    if (ns->rs && ns->connected && !ns->writing && !bufchain_size(&ns->output))
        rssh_sock_shutdown_write(ns->rs);
}

static void net_set_frozen(Socket *s, bool is_frozen)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    ns->frozen = is_frozen;
    if (ns->rs)
        rssh_sock_set_frozen(ns->rs, is_frozen);
}

static const char *net_socket_error(Socket *s)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    return ns->error;
}

static SocketEndpointInfo *net_endpoint_info(Socket *s, bool peer)
{
    NetSocket *ns = container_of(s, NetSocket, sock);
    SocketEndpointInfo *pi = snew(SocketEndpointInfo);
    memset(pi, 0, sizeof(*pi));
    if (!peer) {
        pi->addressfamily = ADDRTYPE_UNSPEC;
        return pi;
    }
    pi->addressfamily = ADDRTYPE_IPV4;
    memcpy(pi->addr_bin.ipv4, ns->addr->ip, 4);
    pi->port = ns->port;
    pi->addr_text = dupprintf("%d.%d.%d.%d", ns->addr->ip[0], ns->addr->ip[1],
                              ns->addr->ip[2], ns->addr->ip[3]);
    pi->log_text = dupprintf("%s:%d", pi->addr_text, pi->port);
    return pi;
}

static const SocketVtable net_socket_vt = {
    .plug = net_plug,
    .close = net_close,
    .write = net_write,
    .write_oob = net_write_oob,
    .write_eof = net_write_eof,
    .set_frozen = net_set_frozen,
    .socket_error = net_socket_error,
    .endpoint_info = net_endpoint_info,
};

Socket *sk_new(SockAddr *addr, int port, bool privport, bool oobinline,
               bool nodelay, bool keepalive, Plug *plug)
{
    NetSocket *ns = snew(NetSocket);
    memset(ns, 0, sizeof(*ns));
    ns->sock.vt = &net_socket_vt;
    ns->plug = plug;
    ns->addr = addr;                 /* sk_new takes ownership */
    ns->port = port;
    bufchain_init(&ns->output);

    if (!addr->resolved) {
        ns->error = addr->error ? addr->error : "host name not resolved";
        return &ns->sock;
    }
    plug_log(plug, &ns->sock, PLUGLOG_CONNECT_TRYING, addr, port, NULL, 0);
    ns->rs = rssh_sock_connect(addr->ip, port, nodelay, &net_events, ns);
    if (!ns->rs)
        ns->error = "could not create socket";
    return &ns->sock;
}

Socket *sk_newlistener(const char *srcaddr, int port, Plug *plug,
                       bool local_host_only, int address_family)
{
    return new_error_socket_fmt(plug, "listening sockets are not "
                                "supported by rSSH yet");
}

void plug_closing_errno(Plug *plug, int error)
{
    plug_closing_error(plug, strerror(error));
}
