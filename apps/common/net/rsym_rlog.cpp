/*
 * rsym_rlog.cpp: remote debug log (see rsym_rlog.h).
 *
 * Self-contained (native RSocket, no rsym_tcp), so that rsym_tcp's own log
 * lines cannot loop back into it, and so apps that do not link rsym_net.lib
 * (rSSH) can compile it in directly.
 *
 * Every global here is plain data or an R-class handle, which is all zeros
 * before use: constructors of statics in a static library never run on
 * Symbian, so nothing may depend on one.
 */
#include <e32base.h>
#include <es_sock.h>
#include <in_sock.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rsym_rlog.h"

#define KQueueSize      32768
#define KMaxLine        1000
#define KChunk          1024
#define KConnectMs      10000
#define KSendMs         10000
#define KRetryUs        5000000

enum { EStateOff, EStateNoHost, EStateConnecting, EStateConnected,
       EStateRetrying, EStatePaused };

static RMutex gLock;
static RSemaphore gWake;            // signalled on new lines and new settings
static TBool gReady, gThreadStarted;

static char gCfgPath[160];
static char gName[32];
static char gHost[64];
static int gPort, gOn, gGen;        // gGen: bumped on every settings change

static char gQueue[KQueueSize];     // ring buffer of '\n'-terminated lines
static int gHead, gCount, gDropped;

static int gState, gLastErr;

// ---------------------------------------------------------------------------
// settings file

static void LoadConfig()
{
    gOn = 0;
    gHost[0] = 0;
    gPort = RSYM_RLOG_DEFAULT_PORT;
    FILE *fp = fopen(gCfgPath, "r");
    if (!fp)
        return;
    char line[128];
    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *val = eq + 1;
        if (!strcmp(line, "remote-debug"))
            gOn = atoi(val) != 0;
        else if (!strcmp(line, "remote-debug-host")) {
            strncpy(gHost, val, sizeof gHost - 1);
            gHost[sizeof gHost - 1] = 0;
        } else if (!strcmp(line, "remote-debug-port")) {
            int p = atoi(val);
            if (p > 0 && p < 65536)
                gPort = p;
        }
    }
    fclose(fp);
}

static void SaveConfig()
{
    FILE *fp = fopen(gCfgPath, "w");
    if (!fp)
        return;
    fprintf(fp, "remote-debug=%d\nremote-debug-host=%s\nremote-debug-port=%d\n",
            gOn, gHost, gPort);
    fclose(fp);
}

// ---------------------------------------------------------------------------
// the sender thread

// One network connection (RConnection) is started and kept for the life of
// the thread, so a phone set to "always ask" for an access point asks once,
// not at every reconnect.
class TSender
{
public:
    TSender() : iConnected(EFalse), iConnOpen(EFalse), iConnUp(EFalse) {}
    TInt Open()
    {
        TInt err = iServ.Connect();
        if (err == KErrNone)
            err = iTimer.CreateLocal();
        return err;
    }
    // KErrCancel: the user declined the access point prompt.
    TInt StartNetwork()
    {
        if (iConnUp)
            return KErrNone;
        if (!iConnOpen)
            iConnOpen = iConn.Open(iServ) == KErrNone;
        TInt err = iConnOpen ? iConn.Start() : KErrNotSupported;
        if (err == KErrCancel)
            return err;
        // Anything else (e.g. no RConnection support in the EKA2L1
        // emulator): fall back to the implicit connection.
        iConnUp = err == KErrNone;
        return KErrNone;
    }
    // The network went away: start it again next time. (Close, not Stop:
    // Stop would take the network down for the rest of the app too.)
    void NetworkLost()
    {
        if (iConnOpen) {
            iConn.Close();
            iConnOpen = iConnUp = EFalse;
        }
    }
    TInt Connect(const char *aHost, int aPort);
    TInt Send(const char *aData, int aLen);
    void Disconnect()
    {
        if (iConnected) {
            iSock.Close();
            iConnected = EFalse;
        }
    }
    TBool Connected() const { return iConnected; }

private:
    TInt Wait(TRequestStatus &aStatus, TInt aMs);
    RSocketServ iServ;
    RConnection iConn;
    RSocket iSock;
    RTimer iTimer;
    TBool iConnected, iConnOpen, iConnUp;
};

// Wait for aStatus, or time out after aMs (the caller cancels the request).
TInt TSender::Wait(TRequestStatus &aStatus, TInt aMs)
{
    TRequestStatus timer;
    iTimer.After(timer, aMs * 1000);
    User::WaitForRequest(aStatus, timer);
    if (aStatus == KRequestPending)
        return KErrTimedOut;
    iTimer.Cancel();
    User::WaitForRequest(timer);
    return aStatus.Int();
}

TInt TSender::Connect(const char *aHost, int aPort)
{
    TBuf<64> name;
    name.Copy(TPtrC8((const TUint8 *)aHost, strlen(aHost)));
    TInetAddr addr;
    if (addr.Input(name) != KErrNone) {
        // not a dotted quad: look the name up
        RHostResolver resolver;
        TInt err = iConnUp ? resolver.Open(iServ, KAfInet, KProtocolInetUdp, iConn)
                           : resolver.Open(iServ, KAfInet, KProtocolInetUdp);
        if (err != KErrNone)
            return err;
        TNameEntry entry;
        err = resolver.GetByName(name, entry);
        resolver.Close();
        if (err != KErrNone)
            return err;
        addr = TInetAddr::Cast(entry().iAddr);
    }
    addr.SetPort(aPort);
    TInt err = iConnUp ? iSock.Open(iServ, KAfInet, KSockStream, KProtocolInetTcp, iConn)
                       : iSock.Open(iServ, KAfInet, KSockStream, KProtocolInetTcp);
    if (err != KErrNone)
        return err;
    TRequestStatus st;
    iSock.Connect(addr, st);
    err = Wait(st, KConnectMs);
    if (err == KErrTimedOut) {
        iSock.CancelConnect();
        User::WaitForRequest(st);
    }
    if (err != KErrNone) {
        iSock.Close();
        return err;
    }
    iSock.SetOpt(KSoTcpNoDelay, KSolInetTcp, 1);
    iConnected = ETrue;
    return KErrNone;
}

TInt TSender::Send(const char *aData, int aLen)
{
    TPtrC8 data((const TUint8 *)aData, aLen);
    TRequestStatus st;
    iSock.Write(data, st);
    TInt err = Wait(st, KSendMs);
    if (err == KErrTimedOut) {
        iSock.CancelWrite();
        User::WaitForRequest(st);
    }
    return err;
}

static void SetState(int aState, int aErr)
{
    gLock.Wait();
    gState = aState;
    gLastErr = aErr;
    gLock.Signal();
}

static void SenderLoop()
{
    TSender s;
    if (s.Open() != KErrNone)
        return;
    char host[64];
    char chunk[KChunk + 64];
    int port = 0, connGen = -1, pausedGen = -1;
    for (;;) {
        gLock.Wait();
        int on = gOn, gen = gGen;
        strcpy(host, gHost);
        port = gPort;
        gLock.Signal();

        if (s.Connected() && gen != connGen)
            s.Disconnect();                 // new settings: reconnect
        if (!on || !host[0] || gen == pausedGen) {
            if (!on || !host[0])
                SetState(on ? EStateNoHost : EStateOff, 0);
            gWake.Wait();
            continue;
        }
        if (!s.Connected()) {
            SetState(EStateConnecting, 0);
            TInt err = s.StartNetwork();
            if (err == KErrNone)
                err = s.Connect(host, port);
            if (err == KErrNone) {
                int n = snprintf(chunk, sizeof chunk, "#rlog1 app=%s\n", gName);
                err = s.Send(chunk, n);
                if (err != KErrNone)
                    s.Disconnect();
            }
            if (err == KErrCancel) {
                // the user cancelled the access point prompt: do not ask
                // again until the settings change
                pausedGen = gen;
                SetState(EStatePaused, err);
                continue;
            }
            if (err != KErrNone) {
                // "connection refused" or a time-out mean the server is not
                // listening; these mean the network itself has gone
                if (err == KErrNotReady || err == KErrDisconnected ||
                    err == KErrNetUnreach || err == KErrHostUnreach)
                    s.NetworkLost();
                SetState(EStateRetrying, err);
                User::After(KRetryUs);
                continue;
            }
            connGen = gen;
            SetState(EStateConnected, 0);
        }

        // take the oldest queued lines (up to KChunk bytes)
        int n = 0, take;
        gLock.Wait();
        if (gDropped) {
            n = snprintf(chunk, sizeof chunk, "#rlog: %d lines dropped (queue full)\n",
                         gDropped);
            gDropped = 0;
        }
        take = gCount < KChunk ? gCount : KChunk;
        int tail = (gHead - gCount + KQueueSize) % KQueueSize;
        for (int i = 0; i < take; i++)
            chunk[n + i] = gQueue[(tail + i) % KQueueSize];
        gLock.Signal();
        n += take;
        if (n == 0) {
            gWake.Wait();
            continue;
        }
        TInt err = s.Send(chunk, n);
        if (err != KErrNone) {
            // lost: the server shows the gap in its timestamps
            s.Disconnect();
            SetState(EStateRetrying, err);
        }
        gLock.Wait();
        if (gGen == gen)        // else the queue may have been emptied meanwhile
            gCount -= take;
        gLock.Signal();
    }
}

static TInt SenderMain(TAny *)
{
    CTrapCleanup *cleanup = CTrapCleanup::New();
    if (!cleanup)
        return KErrNoMemory;
    SenderLoop();
    delete cleanup;
    return KErrNone;
}

static void StartSender()
{
    if (gThreadStarted)
        return;
    RThread thread;
    // shared heap, so the thread may use memory from any other
    TInt err = thread.Create(KNullDesC, SenderMain, 0x4000, &User::Allocator(), NULL);
    if (err != KErrNone)
        return;
    thread.SetPriority(EPriorityLess);  // after the UI and the apps' workers
    thread.Resume();
    thread.Close();
    gThreadStarted = ETrue;
}

// ---------------------------------------------------------------------------
// API

extern "C" void rsym_rlog_init(const char *dir, const char *name)
{
    if (gReady)
        return;
    snprintf(gCfgPath, sizeof gCfgPath, "%s/remote-debug.cfg", dir);
    strncpy(gName, name, sizeof gName - 1);
    LoadConfig();
    // EOwnerProcess (the default): the handles work in every thread
    if (gLock.CreateLocal() != KErrNone)
        return;
    if (gWake.CreateLocal(0) != KErrNone) {
        gLock.Close();
        return;
    }
    gState = EStateOff;
    gReady = ETrue;
    if (gOn)
        StartSender();
}

extern "C" int rsym_rlog_enabled(void)
{
    return gReady && gOn;
}

extern "C" void rsym_rlog_line(const char *line)
{
    if (!gReady || !gOn)
        return;
    int len = strlen(line);
    if (len > KMaxLine)
        len = KMaxLine;
    gLock.Wait();
    TBool wake = gCount == 0;
    if (gCount + len + 1 > KQueueSize) {
        gDropped++;
        wake = EFalse;
    } else {
        for (int i = 0; i < len; i++) {
            char c = line[i];
            gQueue[gHead] = c == '\n' ? ' ' : c;
            gHead = (gHead + 1) % KQueueSize;
        }
        gQueue[gHead] = '\n';
        gHead = (gHead + 1) % KQueueSize;
        gCount += len + 1;
    }
    gLock.Signal();
    if (wake)
        gWake.Signal();
}

extern "C" void rsym_rlog_configure(int on, const char *host, int port)
{
    if (!gReady)
        return;
    gLock.Wait();
    gOn = on != 0;
    if (host) {
        strncpy(gHost, host, sizeof gHost - 1);
        gHost[sizeof gHost - 1] = 0;
    }
    if (port > 0 && port < 65536)
        gPort = port;
    gGen++;
    if (!gOn) {
        gCount = 0;                     // nothing more will be sent
        gDropped = 0;
    }
    SaveConfig();
    gLock.Signal();
    if (gOn)
        StartSender();
    gWake.Signal();
}

extern "C" const char *rsym_rlog_host(void) { return gHost; }

extern "C" int rsym_rlog_port(void) { return gPort ? gPort : RSYM_RLOG_DEFAULT_PORT; }

extern "C" void rsym_rlog_status(char *buf, int buflen)
{
    if (!gReady) {
        snprintf(buf, buflen, "unavailable");
        return;
    }
    gLock.Wait();
    int state = gOn ? gState : EStateOff;
    if (gOn && !gHost[0])
        state = EStateNoHost;
    switch (state) {
    case EStateOff:        snprintf(buf, buflen, "Off (default)"); break;
    case EStateNoHost:     snprintf(buf, buflen, "On, but no host set"); break;
    case EStateConnecting: snprintf(buf, buflen, "On, connecting to %s:%d", gHost, gPort); break;
    case EStateConnected:  snprintf(buf, buflen, "On, sending to %s:%d", gHost, gPort); break;
    case EStateRetrying:   snprintf(buf, buflen, "On, cannot reach %s:%d (%d), retrying",
                                    gHost, gPort, gLastErr); break;
    case EStatePaused:     snprintf(buf, buflen, "On, paused (no access point)"); break;
    default:               snprintf(buf, buflen, "On"); break;
    }
    gLock.Signal();
}

extern "C" void rsym_rlog_flush(int timeout_ms)
{
    if (!gReady || !gOn)
        return;
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        gLock.Wait();
        TBool done = gCount == 0 || gState != EStateConnected;
        gLock.Signal();
        if (done)
            return;
        User::After(100000);
    }
}
