/*
 * audio_out.h: plays radio_engine's PCM on the UI thread (header-only).
 *
 * - CAudioOut feeds RadioPcmRead()'s PCM to CMdaAudioOutputStream, in a few
 *   16 KB buffers. Once the output has played everything it was given (the
 *   end of a track, or an underrun), it tells its observer.
 * - CRadioNotify is the UI thread's side of RadioSetNotify(): it calls
 *   MRadioNews::RadioNewsL() whenever the engine has news.
 *
 * Link with mediaclientaudiostream.lib.
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <e32base.h>
#include <mdaaudiooutputstream.h>
#include <mda/common/audio.h>

#include "radio_engine.h"
#include "rsym_log.h"

class MAudioOutObserver
{
public:
    // The output ran out of data and has stopped (all of it was heard).
    virtual void AudioDrainedL() = 0;
};

class CAudioOut : public CBase, public MMdaAudioOutputStreamCallback
{
public:
    CAudioOut(MAudioOutObserver *aObserver = NULL) : iObserver(aObserver) {}
    ~CAudioOut()
    {
        Stop();
        delete iDrained;
        for (TInt i = 0; i < KBuffers; i++)
            delete[] iBuf[i];
    }
    void ConstructL()
    {
        for (TInt i = 0; i < KBuffers; i++)
            iBuf[i] = new (ELeave) TUint8[KBufBytes];
        // the observer hears of the end from the scheduler, not from inside
        // the output's own callback (it may well start the next track)
        iDrained = new (ELeave) CAsyncCallBack(TCallBack(Drained, this),
                                               CActive::EPriorityStandard);
    }

    TBool Running() const { return iStream != NULL && !iDead; }

    // Open the output at the engine's current format (once it plays).
    void StartL(TInt aVolume)
    {
        Reap();
        if (iStream)
            return;
        RadioPcmFormat(iHz, iChannels);
        if (iHz <= 0)
            return;
        iVolume = aVolume;
        iStream = CMdaAudioOutputStream::NewL(*this);
        iOpen = EFalse;
        rsym_log("audio: open (%d Hz, %d ch)", iHz, iChannels);
        iStream->Open(&iSettings);
    }

    void Stop()
    {
        if (!iStream)
            return;
        rsym_log("audio: stop (%d buffers written)", iWritten);
        delete iStream;         // stops; no more callbacks
        iStream = NULL;
        iOpen = iDead = EFalse;
        iDrained->Cancel();
        for (TInt i = 0; i < KBuffers; i++)
            iBusy[i] = EFalse;
    }

    // volume: 0..10
    void SetVolume(TInt aVolume)
    {
        iVolume = aVolume;
        if (iStream && iOpen)
            iStream->SetVolume(iStream->MaxVolume() * iVolume / 10);
    }

    // Hand the engine's PCM to the free buffers.
    void Feed()
    {
        Reap();
        if (!iStream || !iOpen)
            return;
        for (TInt i = 0; i < KBuffers; i++) {
            if (iBusy[i])
                continue;
            TInt n = RadioPcmRead(iBuf[i], KBufBytes);
            if (n <= 0)
                return;
            iBusy[i] = ETrue;
            iPtr[i].Set(iBuf[i], n);
            TRAPD(err, iStream->WriteL(iPtr[i]));
            if (err != KErrNone) {
                rsym_log("audio: WriteL left %d", err);
                iBusy[i] = EFalse;
                return;
            }
            iWritten++;
        }
    }

private:
    void MaoscOpenComplete(TInt aError)
    {
        rsym_log("audio: open complete %d", aError);
        if (aError != KErrNone) {
            Stop();
            return;
        }
        TRAPD(err, iStream->SetAudioPropertiesL(RateCaps(iHz),
            iChannels == 1 ? TMdaAudioDataSettings::EChannelsMono
                           : TMdaAudioDataSettings::EChannelsStereo));
        if (err != KErrNone)
            rsym_log("audio: SetAudioPropertiesL(%d Hz) left %d", iHz, err);
        iStream->SetPriority(EPriorityNormal, EMdaPriorityPreferenceTimeAndQuality);
        iOpen = ETrue;
        SetVolume(iVolume);
        Feed();
    }

    void MaoscBufferCopied(TInt aError, const TDesC8 &aBuffer)
    {
        for (TInt i = 0; i < KBuffers; i++)
            if (iBusy[i] && aBuffer.Ptr() == iBuf[i])
                iBusy[i] = EFalse;
        if (aError == KErrNone)
            Feed();
        else if (aError != KErrAbort)
            rsym_log("audio: buffer copied, error %d", aError);
    }

    void MaoscPlayComplete(TInt aError)
    {
        // KErrUnderflow: we ran out of data; it reopens once the engine has
        // buffered again (CRadioAppUi::RadioNewsL).
        rsym_log("audio: play complete %d", aError);
        iOpen = EFalse;
        iDead = ETrue;          // deleted on the next call, not inside its callback
        for (TInt i = 0; i < KBuffers; i++)
            iBusy[i] = EFalse;
        if (iObserver)
            iDrained->CallBack();
    }

    static TInt Drained(TAny *aSelf)
    {
        CAudioOut *self = static_cast<CAudioOut *>(aSelf);
        self->Reap();
        TRAPD(err, self->iObserver->AudioDrainedL());
        if (err != KErrNone)
            rsym_log("audio: drained callback left %d", err);
        return 0;
    }

    void Reap()
    {
        if (iDead) {
            delete iStream;
            iStream = NULL;
            iDead = EFalse;
        }
    }

    static TInt RateCaps(TInt aHz)
    {
        switch (aHz) {
        case 8000:  return TMdaAudioDataSettings::ESampleRate8000Hz;
        case 11025: return TMdaAudioDataSettings::ESampleRate11025Hz;
        case 12000: return TMdaAudioDataSettings::ESampleRate12000Hz;
        case 16000: return TMdaAudioDataSettings::ESampleRate16000Hz;
        case 22050: return TMdaAudioDataSettings::ESampleRate22050Hz;
        case 24000: return TMdaAudioDataSettings::ESampleRate24000Hz;
        case 32000: return TMdaAudioDataSettings::ESampleRate32000Hz;
        case 48000: return TMdaAudioDataSettings::ESampleRate48000Hz;
        default:    return TMdaAudioDataSettings::ESampleRate44100Hz;
        }
    }

    enum { KBuffers = 4, KBufBytes = 16384 };    // ~90 ms each at 44.1 kHz stereo
    MAudioOutObserver *iObserver;
    CAsyncCallBack *iDrained;
    CMdaAudioOutputStream *iStream;
    TMdaAudioDataSettings iSettings;
    TBool iOpen, iDead;
    TInt iHz, iChannels, iVolume, iWritten;
    TUint8 *iBuf[KBuffers];
    TBool iBusy[KBuffers];
    TPtrC8 iPtr[KBuffers];
};

class MRadioNews
{
public:
    virtual void RadioNewsL() = 0;
};

class CRadioNotify : public CActive
{
public:
    CRadioNotify(MRadioNews &aObserver) : CActive(EPriorityStandard), iObserver(aObserver)
    {
        CActiveScheduler::Add(this);
        RadioSetNotify(&iStatus, RThread().Id());
        Arm();
    }
    ~CRadioNotify() { Cancel(); }

private:
    void Arm()
    {
        iStatus = KRequestPending;
        SetActive();
        RadioArmNotify();
    }
    void RunL()
    {
        Arm();
        iObserver.RadioNewsL();
    }
    TInt RunError(TInt aError)
    {
        rsym_log("notify: RunL left %d", aError);
        return KErrNone;
    }
    void DoCancel()
    {
        RadioSetNotify(NULL, RThread().Id());
        TRequestStatus *s = &iStatus;
        User::RequestComplete(s, KErrCancel);
    }
    MRadioNews &iObserver;
};

#endif
