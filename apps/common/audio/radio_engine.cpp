/*
 * radio_engine.cpp: see radio_engine.h.
 *
 * Each RadioPlay() starts a worker with its own context; RadioStop() just
 * cancels it and bumps the generation number, so an old worker that is still
 * winding down (in a blocking read, say) can no longer touch the shared
 * state. The worker frees its own context when it ends.
 */
#include <e32base.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "radio_engine.h"
#include "minimp3.h"
#include "rsym_https.h"
#include "rsym_log.h"

#define KPcmBytes       (256 * 1024)    // ~1.5 s of 44.1 kHz stereo
#define KStartMs        1000            // buffered before playback starts
#define KMp3Bytes       16384
#define KMaxRedirects   5
#define KPlaylistBytes  16384

// ---------------------------------------------------------------------------
// shared state (all under gLock)

static RMutex gLock;
static RSemaphore gSpace;           // signalled when the consumer frees PCM room
static TBool gReady;
static int gGen;                    // generation of the current worker
static TRadioInfo gInfo;
static RMutex gHttpsLock;           // first use of rsym_https is not thread-safe
static const char *gCaPem;
static TBool gHttpsReady;

static TUint8 *gPcm;                // ring buffer of PCM bytes
static int gPcmHead, gPcmCount;
static int gHz, gChannels;
static TBool gWriterWaiting;
static TBool gInputDone;            // the stream has ended: what is queued is all
static long long gPcmRead;          // bytes handed to the output

static TRequestStatus *gNotifyStatus;
static TThreadId gNotifyThread;
static TBool gNotifyArmed, gNotifyPending;

// Under gLock: complete the UI's request, or remember to once it re-arms.
static void NotifyLocked()
{
    if (!gNotifyStatus)
        return;
    if (!gNotifyArmed) {
        gNotifyPending = ETrue;
        return;
    }
    RThread ui;
    TInt err = ui.Open(gNotifyThread);
    if (err == KErrNone) {
        gNotifyArmed = EFalse;
        gNotifyPending = EFalse;
        TRequestStatus *s = gNotifyStatus;     // (RequestComplete NULLs its argument)
        ui.RequestComplete(s, KErrNone);
        ui.Close();
    } else {
        rsym_log("radio: cannot open the UI thread (%d)", err);
    }
}

static int MsOfBytes(int aBytes)
{
    int perSec = gHz * gChannels * 2;
    return perSec > 0 ? (int)((long long)aBytes * 1000 / perSec) : 0;
}

// ---------------------------------------------------------------------------
// the worker

struct TWorker
    {
    int iGen;
    volatile int iCancel;
    char iUrl[1024];
    // ICY metadata
    int iMetaInt, iAudioLeft, iMetaNeed, iMetaGot, iMetaState;   // 0 audio, 1 length, 2 data
    char iMeta[4096];
    // response
    int iStatus;
    char iLocation[1024];
    TBool iPlaylist;
    char *iBody;                    // playlist text
    int iBodyLen;
    // MP3
    mp3dec_t iDec;
    TUint8 iMp3[KMp3Bytes];
    int iMp3Len;
    mp3d_sample_t iFrame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    unsigned iFrames;
    };

static TBool Current(TWorker *w)
{
    return !w->iCancel && w->iGen == gGen;
}

static void SetState(TWorker *w, TRadioState aState, const char *aError = NULL)
{
    gLock.Wait();
    if (w->iGen == gGen) {
        gInfo.iState = aState;
        if (aError) {
            strncpy(gInfo.iError, aError, sizeof gInfo.iError - 1);
            gInfo.iError[sizeof gInfo.iError - 1] = 0;
        }
        NotifyLocked();
    }
    gLock.Signal();
}

// Queue PCM, waiting for room. EFalse if this worker has been stopped.
static TBool PushPcm(TWorker *w, const TUint8 *aData, int aLen, int aHz, int aChannels)
{
    while (aLen > 0) {
        gLock.Wait();
        if (!Current(w)) {
            gLock.Signal();
            return EFalse;
        }
        if (gHz != aHz || gChannels != aChannels) {
            if (gPcmCount == 0 || gHz == 0) {
                gHz = aHz;
                gChannels = aChannels;
                gInfo.iHz = aHz;
                gInfo.iChannels = aChannels;
            }
            // (a format change mid-stream: played at the old rate; rare)
        }
        int room = KPcmBytes - gPcmCount;
        if (room == 0) {
            gWriterWaiting = ETrue;
            gLock.Signal();
            gSpace.Wait();
            continue;
        }
        int take = aLen < room ? aLen : room;
        for (int i = 0; i < take; i++)
            gPcm[(gPcmHead + i) % KPcmBytes] = aData[i];
        gPcmHead = (gPcmHead + take) % KPcmBytes;
        gPcmCount += take;
        aData += take;
        aLen -= take;
        if (gInfo.iState == ERadioBuffering && MsOfBytes(gPcmCount) >= KStartMs) {
            rsym_log("radio: buffered %d ms: playing", MsOfBytes(gPcmCount));
            gInfo.iState = ERadioPlaying;
            NotifyLocked();
        } else if (gNotifyPending) {
            NotifyLocked();
        }
        gLock.Signal();
    }
    return ETrue;
}

// Decode what MP3 data there is; aFlush: also a last short frame.
static TBool Decode(TWorker *w, TBool aFlush)
{
    int pos = 0;
    while (w->iMp3Len - pos >= (aFlush ? 4 : 4096)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&w->iDec, w->iMp3 + pos, w->iMp3Len - pos,
                                          w->iFrame, &info);
        if (info.frame_bytes == 0)
            break;                  // needs more data
        pos += info.frame_bytes;
        if (samples > 0) {
            if (w->iFrames++ == 0) {
                rsym_log("radio: first frame: %d Hz, %d ch, %d kbps, layer %d",
                         info.hz, info.channels, info.bitrate_kbps, info.layer);
                gLock.Wait();
                if (w->iGen == gGen)
                    gInfo.iKbps = info.bitrate_kbps;
                gLock.Signal();
            }
            if (!PushPcm(w, (const TUint8 *)w->iFrame,
                         samples * info.channels * (int)sizeof(mp3d_sample_t),
                         info.hz, info.channels))
                return EFalse;
        }
    }
    memmove(w->iMp3, w->iMp3 + pos, w->iMp3Len - pos);
    w->iMp3Len -= pos;
    return ETrue;
}

static TBool Audio(TWorker *w, const TUint8 *aData, int aLen)
{
    while (aLen > 0) {
        int room = KMp3Bytes - w->iMp3Len;
        int take = aLen < room ? aLen : room;
        memcpy(w->iMp3 + w->iMp3Len, aData, take);
        w->iMp3Len += take;
        aData += take;
        aLen -= take;
        if (!Decode(w, EFalse))
            return EFalse;
        if (w->iMp3Len == KMp3Bytes)
            w->iMp3Len = 0;         // no frame found in 16 KB: not MP3 after all
    }
    return ETrue;
}

// "StreamTitle='Artist - Song';StreamUrl='...';"
static void Metadata(TWorker *w)
{
    w->iMeta[w->iMetaGot < (int)sizeof w->iMeta ? w->iMetaGot : (int)sizeof w->iMeta - 1] = 0;
    const char *t = strstr(w->iMeta, "StreamTitle='");
    if (!t)
        return;
    t += 13;
    const char *end = strstr(t, "';");
    int len = end ? (int)(end - t) : (int)strlen(t);
    gLock.Wait();
    if (w->iGen == gGen) {
        if (len >= (int)sizeof gInfo.iTitle)
            len = sizeof gInfo.iTitle - 1;
        if (strncmp(gInfo.iTitle, t, len) || gInfo.iTitle[len]) {
            memcpy(gInfo.iTitle, t, len);
            gInfo.iTitle[len] = 0;
            rsym_log("radio: title: %s", gInfo.iTitle);
            NotifyLocked();
        }
    }
    gLock.Signal();
}

static int OnBody(void *aCtx, const unsigned char *aData, int aLen)
{
    TWorker *w = static_cast<TWorker *>(aCtx);
    if (!Current(w))
        return 1;
    if (w->iStatus != 200)
        return 0;                   // a redirect's body: ignored
    if (w->iPlaylist) {
        int take = KPlaylistBytes - w->iBodyLen;
        if (take > aLen)
            take = aLen;
        if (take > 0) {
            memcpy(w->iBody + w->iBodyLen, aData, take);
            w->iBodyLen += take;
        }
        return w->iBodyLen >= KPlaylistBytes;   // enough
    }
    if (w->iMetaInt <= 0)
        return Audio(w, aData, aLen) ? 0 : 1;
    while (aLen > 0) {
        if (w->iMetaState == 0) {
            int take = aLen < w->iAudioLeft ? aLen : w->iAudioLeft;
            if (!Audio(w, aData, take))
                return 1;
            aData += take;
            aLen -= take;
            w->iAudioLeft -= take;
            if (w->iAudioLeft == 0)
                w->iMetaState = 1;
        } else if (w->iMetaState == 1) {
            w->iMetaNeed = aData[0] * 16;
            w->iMetaGot = 0;
            aData++;
            aLen--;
            w->iMetaState = w->iMetaNeed ? 2 : 0;
            if (!w->iMetaNeed)
                w->iAudioLeft = w->iMetaInt;
        } else {
            int take = w->iMetaNeed - w->iMetaGot;
            if (take > aLen)
                take = aLen;
            int room = (int)sizeof w->iMeta - 1 - w->iMetaGot;
            memcpy(w->iMeta + w->iMetaGot, aData, take < room ? take : (room > 0 ? room : 0));
            w->iMetaGot += take;
            aData += take;
            aLen -= take;
            if (w->iMetaGot == w->iMetaNeed) {
                Metadata(w);
                w->iMetaState = 0;
                w->iAudioLeft = w->iMetaInt;
            }
        }
    }
    return 0;
}

static TBool HeaderIs(const char *aHeaders, const char *aName, char *aVal, int aLen)
{
    rsym_http_response r;
    Mem::FillZ(&r, sizeof r);
    r.headers = const_cast<char *>(aHeaders);
    return rsym_http_header(&r, aName, aVal, aLen) != NULL;
}

static int OnHeaders(void *aCtx, int aStatus, const char *aHeaders)
{
    TWorker *w = static_cast<TWorker *>(aCtx);
    char val[256];
    w->iStatus = aStatus;
    rsym_log("radio: HTTP %d", aStatus);
    if (aStatus >= 300 && aStatus < 400) {
        if (HeaderIs(aHeaders, "Location", w->iLocation, sizeof w->iLocation))
            return 1;               // follow it (no need for the body)
        return 0;
    }
    if (aStatus != 200)
        return 1;
    val[0] = 0;
    HeaderIs(aHeaders, "Content-Type", val, sizeof val);
    rsym_log("radio: content-type %s", val);
    const char *u = w->iUrl + strlen(w->iUrl);
    TBool plsName = strlen(w->iUrl) > 4 &&
        (!strcasecmp(u - 4, ".pls") || !strcasecmp(u - 4, ".m3u") ||
         (strlen(w->iUrl) > 5 && !strcasecmp(u - 5, ".m3u8")));
    if (strstr(val, "scpls") || strstr(val, "mpegurl") || strstr(val, "pls+xml") ||
        (plsName && !strstr(val, "audio/mpeg"))) {
        w->iPlaylist = ETrue;
        return 0;
    }
    if (val[0] && !strstr(val, "mpeg") && !strstr(val, "mp3") &&
        !strstr(val, "octet-stream") && !strstr(val, "audio/x-mp")) {
        char err[160];
        snprintf(err, sizeof err, "Unsupported stream format (%s). Only MP3 streams play for now.", val);
        SetState(w, ERadioFailed, err);
        return 1;
    }
    if (HeaderIs(aHeaders, "icy-metaint", val, sizeof val))
        w->iMetaInt = atoi(val);
    w->iAudioLeft = w->iMetaInt;
    w->iMetaState = 0;
    gLock.Wait();
    if (w->iGen == gGen) {
        if (HeaderIs(aHeaders, "icy-name", val, sizeof val))
            strncpy(gInfo.iName, val, sizeof gInfo.iName - 1);
        if (HeaderIs(aHeaders, "icy-br", val, sizeof val))
            gInfo.iKbps = atoi(val);
        gInfo.iState = ERadioBuffering;
        NotifyLocked();
    }
    gLock.Signal();
    return 0;
}

// "http[s]://host[:port]/path" -> parts. EFalse if not a URL we can fetch.
static TBool SplitUrl(const char *aUrl, TBool &aTls, char *aHost, int aHostLen,
                      int &aPort, const char *&aPath)
{
    const char *p;
    if (!strncasecmp(aUrl, "http://", 7)) {
        aTls = EFalse;
        p = aUrl + 7;
    } else if (!strncasecmp(aUrl, "https://", 8)) {
        aTls = ETrue;
        p = aUrl + 8;
    } else {
        return EFalse;
    }
    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);
    const char *at = (const char *)memchr(p, '@', end - p);   // user:pass@ (ignored)
    if (at)
        p = at + 1;
    const char *colon = (const char *)memchr(p, ':', end - p);
    int hlen = (int)((colon ? colon : end) - p);
    if (hlen <= 0 || hlen >= aHostLen)
        return EFalse;
    memcpy(aHost, p, hlen);
    aHost[hlen] = 0;
    aPort = colon ? atoi(colon + 1) : 0;
    aPath = slash ? slash : "/";
    return ETrue;
}

// First stream URL in a .pls or .m3u playlist.
static TBool PlaylistUrl(TWorker *w)
{
    w->iBody[w->iBodyLen < KPlaylistBytes ? w->iBodyLen : KPlaylistBytes - 1] = 0;
    char *line = w->iBody;
    while (line && *line) {
        char *next = strpbrk(line, "\r\n");
        if (next)
            *next++ = 0;
        while (*line == ' ' || *line == '\t')
            line++;
        char *http = strstr(line, "http");      // "File1=http://..." or "http://..."
        if (http && (!strncasecmp(http, "http://", 7) || !strncasecmp(http, "https://", 8)) &&
            line[0] != '#') {
            strncpy(w->iLocation, http, sizeof w->iLocation - 1);
            w->iLocation[sizeof w->iLocation - 1] = 0;
            return ETrue;
        }
        while (next && (*next == '\r' || *next == '\n'))
            next++;
        line = next;
    }
    return EFalse;
}

static void Run(TWorker *w)
{
    mp3dec_init(&w->iDec);
    for (int hop = 0; hop <= KMaxRedirects && Current(w); hop++) {
        TBool tls;
        char host[256];
        int port;
        const char *path;
        rsym_log("radio: open %s", w->iUrl);
        if (!SplitUrl(w->iUrl, tls, host, sizeof host, port, path)) {
            SetState(w, ERadioFailed, "Not an http:// or https:// address.");
            return;
        }
        RadioNetInit();
        w->iStatus = 0;
        w->iLocation[0] = 0;
        w->iPlaylist = EFalse;
        w->iBodyLen = 0;
        w->iMetaInt = 0;
        rsym_http_request req;
        Mem::FillZ(&req, sizeof req);
        req.method = "GET";
        req.host = host;
        req.port = port;
        req.path = path;
        req.plain = !tls;
        req.headers = "Icy-MetaData: 1\r\nAccept: */*\r\n";
        req.user_agent = "rInternetRadio/0.1 (Symbian)";
        req.timeout_ms = 20000;
        req.on_body = OnBody;
        req.on_headers = OnHeaders;
        req.body_ctx = w;
        req.cancel = &w->iCancel;
        rsym_http_response resp;
        Mem::FillZ(&resp, sizeof resp);
        int r = rsym_https_request(&req, &resp);
        if (!Current(w)) {
            rsym_http_response_free(&resp);
            return;
        }
        if (w->iLocation[0] && w->iStatus >= 300 && w->iStatus < 400) {
            rsym_http_response_free(&resp);
            if (w->iLocation[0] == '/') {
                // relative to this server
                char base[300];
                if (port)
                    snprintf(base, sizeof base, "%s://%s:%d", tls ? "https" : "http", host, port);
                else
                    snprintf(base, sizeof base, "%s://%s", tls ? "https" : "http", host);
                snprintf(w->iUrl, sizeof w->iUrl, "%s%s", base, w->iLocation);
            } else {
                strcpy(w->iUrl, w->iLocation);
            }
            continue;
        }
        if (w->iPlaylist) {
            rsym_http_response_free(&resp);
            if (!PlaylistUrl(w)) {
                SetState(w, ERadioFailed, "The playlist has no stream address in it.");
                return;
            }
            rsym_log("radio: playlist -> %s", w->iLocation);
            strcpy(w->iUrl, w->iLocation);
            continue;
        }
        char err[160];
        if (r != 0)
            snprintf(err, sizeof err, "%s", resp.error);
        else if (w->iStatus != 200)
            snprintf(err, sizeof err, "The server answered HTTP %d.", w->iStatus);
        else
            snprintf(err, sizeof err, "The stream ended.");
        rsym_log("radio: ended: %s (%u frames)", err, w->iFrames);
        rsym_http_response_free(&resp);
        if (w->iStatus == 200 && r == 0) {
            // a normal end (a track): play out what is queued
            Decode(w, ETrue);
            gLock.Wait();
            if (w->iGen == gGen && gInfo.iState != ERadioFailed) {
                gInputDone = ETrue;
                if (gPcmCount == 0)
                    gInfo.iState = ERadioFinished;
                else if (gInfo.iState == ERadioBuffering)
                    gInfo.iState = ERadioPlaying;       // shorter than the start buffer
                NotifyLocked();
            }
            gLock.Signal();
            return;
        }
        gLock.Wait();
        TBool failedAlready = w->iGen == gGen && gInfo.iState == ERadioFailed;
        gLock.Signal();
        if (!failedAlready)
            SetState(w, ERadioFailed, err);
        return;
    }
    if (Current(w))
        SetState(w, ERadioFailed, "Too many redirects.");
}

static TInt WorkerMain(TAny *aPtr)
{
    TWorker *w = static_cast<TWorker *>(aPtr);
    CTrapCleanup *cleanup = CTrapCleanup::New();
    Run(w);
    rsym_log("radio: worker %d done", w->iGen);
    free(w->iBody);
    free(w);
    delete cleanup;
    return 0;
}

// ---------------------------------------------------------------------------
// API

TInt RadioInit()
{
    if (gReady)
        return KErrNone;
    gPcm = (TUint8 *)malloc(KPcmBytes);
    if (!gPcm)
        return KErrNoMemory;
    TInt err = gLock.CreateLocal();
    if (err == KErrNone)
        err = gHttpsLock.CreateLocal();
    if (err == KErrNone)
        err = gSpace.CreateLocal(0);
    if (err != KErrNone)
        return err;
    gInfo.iState = ERadioStopped;
    gReady = ETrue;
    return KErrNone;
}

void RadioClose()
{
    RadioStop();
}

void RadioSetCa(const char *aPem)
{
    gCaPem = aPem;
}

void RadioNetInit()
{
    gHttpsLock.Wait();
    if (!gHttpsReady) {
        if (gCaPem)
            rsym_https_set_ca(gCaPem);
        gHttpsReady = rsym_https_init() == 0;
        rsym_log("radio: https init %s", gHttpsReady ? "ok" : "FAILED");
    }
    gHttpsLock.Signal();
}

static void StopLocked()
{
    gGen++;
    gPcmCount = 0;
    gPcmHead = 0;
    gHz = gChannels = 0;
    gInputDone = EFalse;
    gPcmRead = 0;
    if (gWriterWaiting) {
        gWriterWaiting = EFalse;
        gSpace.Signal();            // the old worker wakes, sees it is stale
    }
}

void RadioStop()
{
    if (!gReady)
        return;
    gLock.Wait();
    StopLocked();
    Mem::FillZ(&gInfo, sizeof gInfo);
    gInfo.iState = ERadioStopped;
    NotifyLocked();
    gLock.Signal();
}

void RadioPlay(const char *aUrl)
{
    if (!gReady)
        return;
    TWorker *w = (TWorker *)calloc(1, sizeof(TWorker));
    char *body = (char *)malloc(KPlaylistBytes);
    gLock.Wait();
    StopLocked();
    Mem::FillZ(&gInfo, sizeof gInfo);
    if (!w || !body) {
        free(w);
        free(body);
        gInfo.iState = ERadioFailed;
        strcpy(gInfo.iError, "Out of memory.");
        NotifyLocked();
        gLock.Signal();
        return;
    }
    w->iGen = gGen;
    w->iBody = body;
    strncpy(w->iUrl, aUrl, sizeof w->iUrl - 1);
    gInfo.iState = ERadioConnecting;
    NotifyLocked();
    gLock.Signal();
    RThread thread;
    // shared heap: the worker frees its context, allocated here
    TInt err = thread.Create(KNullDesC, WorkerMain, 0x14000, &User::Allocator(), w);
    if (err != KErrNone) {
        free(body);
        free(w);
        gLock.Wait();
        gInfo.iState = ERadioFailed;
        snprintf(gInfo.iError, sizeof gInfo.iError, "Could not start the stream thread (%d).", err);
        NotifyLocked();
        gLock.Signal();
        return;
    }
    thread.SetPriority(EPriorityMore);  // decoding must keep up
    thread.Resume();
    thread.Close();
}

void RadioInfo(TRadioInfo &aInfo)
{
    gLock.Wait();
    aInfo = gInfo;
    aInfo.iBufferedMs = MsOfBytes(gPcmCount);
    {
        long long perSec = (long long)gHz * gChannels * 2;
        aInfo.iPlayedMs = perSec > 0 ? (int)(gPcmRead * 1000 / perSec) : 0;
    }
    gLock.Signal();
}

void RadioSetNotify(TRequestStatus *aStatus, TThreadId aThread)
{
    gLock.Wait();
    gNotifyStatus = aStatus;
    gNotifyThread = aThread;
    gNotifyArmed = EFalse;
    gLock.Signal();
}

void RadioArmNotify()
{
    gLock.Wait();
    gNotifyArmed = ETrue;
    if (gNotifyPending)
        NotifyLocked();
    gLock.Signal();
}

TInt RadioPcmRead(TUint8 *aBuf, TInt aMax)
{
    if (!gReady)
        return 0;
    gLock.Wait();
    if (gInfo.iState != ERadioPlaying || gPcmCount == 0) {
        if (gInfo.iState == ERadioPlaying && gInputDone) {
            gInfo.iState = ERadioFinished;      // all played
            NotifyLocked();
        } else if (gInfo.iState == ERadioPlaying) {
            gInfo.iState = ERadioBuffering;     // ran dry: re-buffer
            gInfo.iUnderruns++;
            NotifyLocked();
        }
        gNotifyPending = ETrue;         // tell the UI when there is more
        gLock.Signal();
        return 0;
    }
    int n = gPcmCount < aMax ? gPcmCount : aMax;
    n &= ~3;                            // whole stereo samples
    if (n == 0)
        n = gPcmCount & ~1;
    int tail = (gPcmHead - gPcmCount + KPcmBytes) % KPcmBytes;
    for (int i = 0; i < n; i++)
        aBuf[i] = gPcm[(tail + i) % KPcmBytes];
    gPcmCount -= n;
    gPcmRead += n;
    if (gWriterWaiting) {
        gWriterWaiting = EFalse;
        gSpace.Signal();
    }
    gLock.Signal();
    return n;
}

void RadioPcmFormat(TInt &aHz, TInt &aChannels)
{
    gLock.Wait();
    aHz = gHz;
    aChannels = gChannels;
    gLock.Signal();
}
