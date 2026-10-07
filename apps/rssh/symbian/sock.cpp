/*
 * sock.cpp: native Symbian TCP sockets for rSSH (see rssh_sock.h).
 *
 * Each connection is a CSock (an active object for connect + receive) with
 * a CSockWriter (an active object for send), both on the UI thread. Events
 * are passed to the C network layer through rssh_sock_events.
 *
 *  - While a modal dialog is up (RsshPlatformIsModal), received data is held
 *    back and redelivered from a timer, so PuTTY is never re-entered.
 *  - rssh_sock_close() can be called from inside one of our own callbacks;
 *    the objects are then deleted when that callback has returned.
 *
 * Uses the implicit default connection, which on a phone triggers the
 * access point prompt (or uses the default destination).
 */

#include <e32base.h>
#include <es_sock.h>
#include <in_sock.h>

#include "rssh_sock.h"
#include "rssh_platform.h"
#include "rssh_trace.h"

const TInt KRecvBufSize = 4096;
const TInt KDeferInterval = 20000;   /* us */

static RSocketServ gSockServ;
static TBool gSockServOpen = EFalse;

static TInt EnsureSockServ()
{
    if (gSockServOpen)
        return KErrNone;
    TInt err = gSockServ.Connect();
    if (err == KErrNone)
        gSockServOpen = ETrue;
    return err;
}

void RsshSockShutdown()
{
    if (gSockServOpen) {
        gSockServ.Close();
        gSockServOpen = EFalse;
    }
}

class CSock;

class CSockWriter : public CActive
{
public:
    CSockWriter(CSock &aOwner) : CActive(EPriorityStandard), iOwner(aOwner)
    {
        CActiveScheduler::Add(this);
    }
    ~CSockWriter() { Cancel(); delete iBuf; }
    TInt Write(RSocket &aSocket, const TDesC8 &aData);
    TBool Busy() const { return IsActive(); }
private:
    void RunL();
    void DoCancel();
    CSock &iOwner;
    RSocket *iSocket;
    HBufC8 *iBuf;
};

class CDeferTimer : public CTimer
{
public:
    static CDeferTimer *NewL(CSock &aOwner)
    {
        CDeferTimer *self = new (ELeave) CDeferTimer(aOwner);
        CleanupStack::PushL(self);
        self->ConstructL();
        CleanupStack::Pop(self);
        return self;
    }
private:
    CDeferTimer(CSock &aOwner) : CTimer(EPriorityStandard), iOwner(aOwner)
    {
        CActiveScheduler::Add(this);
    }
    void RunL();
    CSock &iOwner;
};

class CSock : public CActive
{
public:
    static CSock *New(const rssh_sock_events *aEvents, void *aCtx)
    {
        CSock *self = new CSock(aEvents, aCtx);
        if (!self)
            return NULL;
        TRAPD(err, self->ConstructL());
        if (err != KErrNone) {
            delete self;
            return NULL;
        }
        return self;
    }

    ~CSock()
    {
        Cancel();
        delete iWriter;
        delete iDefer;
        if (iSocketOpen)
            iSocket.Close();
    }

    TInt Connect(const TInetAddr &aAddr, TBool aNoDelay)
    {
        TInt err = EnsureSockServ();
        if (err == KErrNone)
            err = iSocket.Open(gSockServ, KAfInet, KSockStream, KProtocolInetTcp);
        if (err != KErrNone)
            return err;
        iSocketOpen = ETrue;
        if (aNoDelay)
            iSocket.SetOpt(KSoTcpNoDelay, KSolInetTcp, 1);   /* best effort */
        iAddr = aAddr;
        iState = EConnecting;
        iSocket.Connect(iAddr, iStatus);
        SetActive();
        return KErrNone;
    }

    TInt Write(const TDesC8 &aData) { return iWriter->Write(iSocket, aData); }

    void ShutdownWrite()
    {
        iWantShutdown = ETrue;
        MaybeShutdown();
    }

    void MaybeShutdown()
    {
        if (iWantShutdown && !iShutdownDone && !iWriter->Busy() &&
            iState == EReading) {
            /* Half-close after the last write: no TRequestStatus wait for
             * completion beyond the immediate, synchronous request. */
            TRequestStatus st;
            iSocket.Shutdown(RSocket::EStopOutput, st);
            User::WaitForRequest(st);
            iShutdownDone = ETrue;
        }
    }

    void SetFrozen(TBool aFrozen)
    {
        iFrozen = aFrozen;
        if (!iFrozen && iState == EReading && !IsActive() && !iHavePending)
            IssueRead();
    }

    /* Called by rssh_sock_close. */
    void Close()
    {
        iEvents = NULL;
        iCtx = NULL;
        if (iInCallback)
            iDeleteRequested = ETrue;     /* delete when the callback returns */
        else
            delete this;
    }

    /* From the writer and the defer timer. */
    void WriteDone(TInt aErr)
    {
        if (aErr != KErrNone) {
            Fail(aErr);
            return;
        }
        BeginCallback();
        if (iEvents && iEvents->sent)
            iEvents->sent(iCtx);
        if (EndCallback())
            return;
        MaybeShutdown();
    }

    void DeferredDelivery()
    {
        if (RsshPlatformIsModal()) {
            iDefer->After(KDeferInterval);
            return;
        }
        Deliver();
    }

private:
    enum TState { EIdle, EConnecting, EReading, EDead };

    CSock(const rssh_sock_events *aEvents, void *aCtx)
        : CActive(EPriorityStandard), iEvents(aEvents), iCtx(aCtx),
          iRecvPtr(NULL, 0)
    {
        CActiveScheduler::Add(this);
    }

    void ConstructL()
    {
        iWriter = new (ELeave) CSockWriter(*this);
        iDefer = CDeferTimer::NewL(*this);
        iRecvBuf = HBufC8::NewL(KRecvBufSize);
        iRecvPtr.Set(iRecvBuf->Des());
    }

    void BeginCallback() { iInCallback++; }
    /* Returns ETrue if this object has been deleted. */
    TBool EndCallback()
    {
        iInCallback--;
        if (iInCallback == 0 && iDeleteRequested) {
            delete this;
            return ETrue;
        }
        return EFalse;
    }

    void IssueRead()
    {
        iRecvPtr.Zero();
        iSocket.RecvOneOrMore(iRecvPtr, 0, iStatus, iRecvLen);
        SetActive();
    }

    void Fail(TInt aErr)
    {
        if (iState == EDead)
            return;
        iState = EDead;
        BeginCallback();
        if (iEvents && iEvents->closed)
            iEvents->closed(iCtx, aErr == KErrEof ? 0 : aErr);
        EndCallback();
    }

    void Deliver()
    {
        iHavePending = EFalse;
        BeginCallback();
        if (iEvents && iEvents->received)
            iEvents->received(iCtx, iRecvPtr.Ptr(), iRecvPtr.Length());
        if (EndCallback())
            return;
        if (iState == EReading && !iFrozen && !IsActive())
            IssueRead();
    }

    void RunL()
    {
        TInt status = iStatus.Int();
        if (iState == EConnecting) {
            rssh_trace("sock: connect -> %d", status);
            if (status == KErrNone)
                iState = EReading;
            else
                iState = EDead;
            BeginCallback();
            if (iEvents && iEvents->connected)
                iEvents->connected(iCtx, status);
            if (EndCallback())
                return;
            if (iState == EReading && !iFrozen && !IsActive())
                IssueRead();
            return;
        }
        if (iState != EReading)
            return;
        if (status != KErrNone) {
            rssh_trace("sock: recv -> %d", status);
            Fail(status);
            return;
        }
        if (RsshPlatformIsModal()) {
            iHavePending = ETrue;
            iDefer->After(KDeferInterval);
            return;
        }
        Deliver();
    }

    void DoCancel()
    {
        if (iState == EConnecting)
            iSocket.CancelConnect();
        else
            iSocket.CancelRecv();
    }

    const rssh_sock_events *iEvents;
    void *iCtx;
    RSocket iSocket;
    TBool iSocketOpen;
    TInetAddr iAddr;
    TState iState;
    CSockWriter *iWriter;
    CDeferTimer *iDefer;
    HBufC8 *iRecvBuf;
    TPtr8 iRecvPtr;
    TSockXfrLength iRecvLen;
    TBool iFrozen;
    TBool iHavePending;
    TBool iWantShutdown;
    TBool iShutdownDone;
    TInt iInCallback;
    TBool iDeleteRequested;

    friend class CSockWriter;
};

TInt CSockWriter::Write(RSocket &aSocket, const TDesC8 &aData)
{
    if (IsActive())
        return KErrInUse;
    delete iBuf;
    iBuf = aData.Alloc();
    if (!iBuf)
        return KErrNoMemory;
    iSocket = &aSocket;
    iSocket->Write(*iBuf, iStatus);
    SetActive();
    return KErrNone;
}

void CSockWriter::RunL()
{
    iOwner.WriteDone(iStatus.Int());    /* may delete iOwner and us */
}

void CSockWriter::DoCancel()
{
    if (iSocket)
        iSocket->CancelWrite();
}

void CDeferTimer::RunL()
{
    iOwner.DeferredDelivery();          /* may delete iOwner and us */
}

/* ------------------------------------------------------------------ */
/* C API                                                                */
/* ------------------------------------------------------------------ */


extern "C" int rssh_resolve(const char *host, unsigned char ip[4])
{
    TPtrC8 host8((const TUint8 *)host, User::StringLength((const TUint8 *)host));
    TBuf<256> name;
    name.Copy(host8.Left(256));

    TInetAddr addr;
    if (addr.Input(name) != KErrNone) {
        TInt err = EnsureSockServ();
        if (err != KErrNone)
            return err;
        RHostResolver resolver;
        err = resolver.Open(gSockServ, KAfInet, KProtocolInetUdp);
        if (err != KErrNone)
            return err;
        TNameEntry entry;
        rssh_trace("resolve: GetByName %s", host);
        err = resolver.GetByName(name, entry);
        resolver.Close();
        rssh_trace("resolve: -> %d", err);
        if (err != KErrNone)
            return err;
        addr = TInetAddr::Cast(entry().iAddr);
    }
    if (addr.Family() == KAfInet6 && addr.IsV4Mapped())
        addr.ConvertToV4();
    if (addr.Family() != KAfInet)
        return KErrNotSupported;
    TUint32 a = addr.Address();
    ip[0] = (unsigned char)(a >> 24);
    ip[1] = (unsigned char)(a >> 16);
    ip[2] = (unsigned char)(a >> 8);
    ip[3] = (unsigned char)a;
    return KErrNone;
}

extern "C" rssh_sock *rssh_sock_connect(const unsigned char ip[4], int port,
                                        int nodelay,
                                        const rssh_sock_events *events,
                                        void *ctx)
{
    CSock *sock = CSock::New(events, ctx);
    if (!sock)
        return NULL;
    TInetAddr addr(INET_ADDR(ip[0], ip[1], ip[2], ip[3]), port);
    rssh_trace("sock: connecting to %d.%d.%d.%d:%d", ip[0], ip[1], ip[2], ip[3], port);
    TInt err = sock->Connect(addr, nodelay != 0);
    if (err != KErrNone) {
        rssh_trace("sock: open/connect failed %d", err);
        sock->Close();
        return NULL;
    }
    return (rssh_sock *)sock;
}

extern "C" int rssh_sock_write(rssh_sock *s, const void *data, int len)
{
    TPtrC8 buf((const TUint8 *)data, len);
    return ((CSock *)s)->Write(buf);
}

extern "C" void rssh_sock_shutdown_write(rssh_sock *s)
{
    ((CSock *)s)->ShutdownWrite();
}

extern "C" void rssh_sock_set_frozen(rssh_sock *s, int frozen)
{
    ((CSock *)s)->SetFrozen(frozen != 0);
}

extern "C" void rssh_sock_close(rssh_sock *s)
{
    if (s)
        ((CSock *)s)->Close();
}

extern "C" const char *rssh_sock_strerror(int err)
{
    switch (err) {
    case KErrNone:          return "no error";
    case KErrNotFound:      return "host not found";
    case KErrCouldNotConnect: return "could not connect";
    case KErrTimedOut:      return "connection timed out";
    case KErrDisconnected:  return "disconnected";
    case KErrEof:           return "connection closed";
    case KErrCancel:        return "cancelled";
    case KErrNoMemory:      return "out of memory";
    case KErrNotSupported:  return "not supported";
    case KErrAccessDenied:  return "access denied";
    case KErrInUse:         return "in use";
    case KErrServerTerminated: return "socket server terminated";
    case KErrNetUnreach:    return "network unreachable";
    case KErrHostUnreach:   return "host unreachable";
    default:                return "network error";
    }
}
