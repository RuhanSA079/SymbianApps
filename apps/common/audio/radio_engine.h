/*
 * radio_engine.h: plays one HTTP(S) MP3 stream: an internet radio station
 * (rInternetRadio) or a track (rJellyfin).
 *
 * A worker thread fetches the stream (HTTP or HTTPS, following redirects and
 * .pls/.m3u playlists), takes out the ICY metadata (the "now playing"
 * title), decodes the MP3 with minimp3 and queues 16-bit PCM. The UI thread
 * plays the PCM through CMdaAudioOutputStream, taking it with
 * RadioPcmRead(). Playback starts once about a second is buffered, and
 * pauses to re-buffer when the queue runs dry. When a stream with an end (a
 * track) has been fetched and its PCM all read, the state is ERadioFinished.
 *
 * The worker tells the UI thread about news (state, title, PCM after a
 * shortage) by completing the request given to RadioSetNotify().
 */
#ifndef RADIO_ENGINE_H
#define RADIO_ENGINE_H

#include <e32base.h>

enum TRadioState
    {
    ERadioStopped,
    ERadioConnecting,
    ERadioBuffering,
    ERadioPlaying,
    ERadioFailed,           // see RadioInfo's error text
    ERadioFinished          // the stream ended and everything was played
    };

struct TRadioInfo
    {
    TRadioState iState;
    char iTitle[160];       // StreamTitle from the ICY metadata, UTF-8 or Latin-1
    char iName[96];         // icy-name
    char iError[160];
    int iKbps, iHz, iChannels;
    int iBufferedMs;
    int iPlayedMs;          // PCM handed to the output so far
    int iUnderruns;
    };

TInt RadioInit();
void RadioClose();

// Start playing aUrl (http:// or https://), stopping what was playing.
void RadioPlay(const char *aUrl);
void RadioStop();
void RadioInfo(TRadioInfo &aInfo);

// The UI thread's request to complete when there is news; re-armed by the
// UI after each completion (the engine completes it at most once per arm).
void RadioSetNotify(TRequestStatus *aStatus, TThreadId aThread);
void RadioArmNotify();

// Up to aMax bytes of interleaved 16-bit PCM; 0 while (re-)buffering.
TInt RadioPcmRead(TUint8 *aBuf, TInt aMax);
// Format of the PCM being read (valid once playing).
void RadioPcmFormat(TInt &aHz, TInt &aChannels);

// Root certificates for HTTPS (PEM, NUL-terminated, kept by the caller),
// e.g. NetSurf's Mozilla bundle. Set before any request.
void RadioSetCa(const char *aPem);
// Call on a worker thread before its first rsym_https_request (it parses
// the certificates once; safe from several threads).
void RadioNetInit();

#endif
