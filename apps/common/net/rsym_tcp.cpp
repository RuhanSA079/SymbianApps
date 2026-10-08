/*
 * rsym_tcp.cpp: blocking TCP client sockets on RSocket (see rsym_tcp.h).
 * Each operation is issued asynchronously and waited for together with an
 * RTimer, so it can time out; a timed-out request is cancelled and reaped.
 */
#include <e32base.h>
#include <es_sock.h>
#include <in_sock.h>

#include "rsym_tcp.h"
#include "rsym_log.h"

struct rsym_tcp
{
    RSocketServ iServ;
    RSocket iSock;
    RTimer iTimer;
};

static TTimeIntervalMicroSeconds32 Us(int aMs)
{
    if (aMs <= 0 || aMs > 1800000)
        aMs = 1800000;                 /* RTimer::After takes 32-bit microseconds */
    return TTimeIntervalMicroSeconds32(aMs * 1000);
}

/* Wait for aStatus or the timeout; on timeout cancel via aCancel. */
template <class TCancel>
static TInt WaitWithTimeout(RTimer &aTimer, TRequestStatus &aStatus, int aMs, TCancel aCancel)
{
    TRequestStatus timer;
    aTimer.After(timer, Us(aMs));
    User::WaitForRequest(aStatus, timer);
    if (aStatus == KRequestPending) {
        aCancel();
        User::WaitForRequest(aStatus);
        return KErrTimedOut;
    }
    aTimer.Cancel();
    User::WaitForRequest(timer);
    return aStatus.Int();
}

struct TCancelConnect { RSocket *s; void operator()() { s->CancelConnect(); } };
struct TCancelWrite { RSocket *s; void operator()() { s->CancelWrite(); } };
struct TCancelRecv { RSocket *s; void operator()() { s->CancelRecv(); } };

static TInt Resolve(RSocketServ &aServ, const char *aHost, TInetAddr &aAddr)
{
    TPtrC8 host8((const TUint8 *)aHost, User::StringLength((const TUint8 *)aHost));
    TBuf<256> name;
    name.Copy(host8.Left(256));
    if (aAddr.Input(name) == KErrNone)
        return KErrNone;               /* numeric address */
    RHostResolver resolver;
    TInt err = resolver.Open(aServ, KAfInet, KProtocolInetUdp);
    if (err != KErrNone)
        return err;
    TNameEntry entry;
    err = resolver.GetByName(name, entry);
    resolver.Close();
    if (err == KErrNone)
        aAddr = TInetAddr::Cast(entry().iAddr);
    return err;
}

extern "C" rsym_tcp *rsym_tcp_connect(const char *host, int port, int timeout_ms, int *err)
{
    rsym_tcp *t = new rsym_tcp;
    if (!t) {
        *err = KErrNoMemory;
        return NULL;
    }
    TInt e = t->iServ.Connect();
    if (e != KErrNone) {
        delete t;
        *err = e;
        return NULL;
    }
    TInetAddr addr;
    e = t->iTimer.CreateLocal();
    if (e == KErrNone)
        e = Resolve(t->iServ, host, addr);
    rsym_log("tcp: resolve %s -> %d", host, e);
    if (e == KErrNone)
        e = t->iSock.Open(t->iServ, KAfInet, KSockStream, KProtocolInetTcp);
    if (e == KErrNone) {
        addr.SetPort(port);
        TRequestStatus st;
        t->iSock.Connect(addr, st);
        TCancelConnect c = { &t->iSock };
        e = WaitWithTimeout(t->iTimer, st, timeout_ms, c);
        rsym_log("tcp: connect %s:%d -> %d", host, port, e);
    }
    if (e != KErrNone) {
        t->iSock.Close();
        t->iTimer.Close();
        t->iServ.Close();
        delete t;
        *err = e;
        return NULL;
    }
    t->iSock.SetOpt(KSoTcpNoDelay, KSolInetTcp, 1);
    *err = KErrNone;
    return t;
}

extern "C" int rsym_tcp_send(rsym_tcp *t, const void *buf, int len, int timeout_ms)
{
    TPtrC8 data((const TUint8 *)buf, len);
    TRequestStatus st;
    t->iSock.Write(data, st);
    TCancelWrite c = { &t->iSock };
    TInt e = WaitWithTimeout(t->iTimer, st, timeout_ms, c);
    return e == KErrNone ? len : e;
}

extern "C" int rsym_tcp_recv(rsym_tcp *t, void *buf, int len, int timeout_ms)
{
    TPtr8 data((TUint8 *)buf, 0, len);
    TSockXfrLength got;
    TRequestStatus st;
    t->iSock.RecvOneOrMore(data, 0, st, got);
    TCancelRecv c = { &t->iSock };
    TInt e = WaitWithTimeout(t->iTimer, st, timeout_ms, c);
    if (e == KErrEof)
        return 0;
    if (e != KErrNone)
        return e;
    return data.Length();
}

extern "C" void rsym_tcp_close(rsym_tcp *t)
{
    if (!t)
        return;
    t->iSock.Close();
    t->iTimer.Close();
    t->iServ.Close();
    delete t;
}

extern "C" const char *rsym_tcp_strerror(int err)
{
    switch (err) {
    case KErrNone:            return "no error";
    case KErrNotFound:        return "host not found";
    case KErrCouldNotConnect: return "could not connect";
    case KErrTimedOut:        return "timed out";
    case KErrDisconnected:    return "disconnected";
    case KErrEof:             return "connection closed";
    case KErrNoMemory:        return "out of memory";
    case KErrNetUnreach:      return "network unreachable";
    case KErrHostUnreach:     return "host unreachable";
    case KErrAccessDenied:    return "access denied";
    case KErrCancel:          return "cancelled";
    default:                  return "network error";
    }
}
