/*
 * platform.cpp: the Symbian side of rSSH's PuTTY platform layer.
 *
 *  - entropy (TRandom::SecureRandomL) and the millisecond tick counter
 *  - the event loop, built from active objects on the UI thread:
 *      * CTimerAO runs PuTTY's timers (timer_change_notify/run_timers)
 *      * CCallbackAO runs PuTTY's top-level callback queue
 *  - startup: HOME -> the app's private directory, for unix/storage.c
 *
 * PuTTY's headers are C99 and this file is C++98, so the handful of PuTTY
 * functions used here are declared by hand below instead of via putty.h.
 */

#include <e32base.h>
#include <e32std.h>
#include <f32file.h>
#include <hal.h>
#include <random.h>

#include <stdlib.h>
#include <string.h>

#include "rssh_platform.h"
#include "rssh_ui.h"
#include "rssh_trace.h"

extern "C" {
/* timing.c / callback.c */
int run_timers(unsigned long now, unsigned long *next);   /* C bool */
int run_toplevel_callbacks(void);
int toplevel_callback_pending(void);
typedef void (*toplevel_callback_notify_fn_t)(void *ctx);
void request_callback_notifications(toplevel_callback_notify_fn_t, void *);
/* sshrand.c: NoiseSourceId is an int-sized enum */
void random_add_noise(int source, const void *noise, int length);
}

enum { KNoiseSourceTime = 0, KNoiseSourcePerfCount = 16 };  /* NoiseSourceId */

const TInt KPollInterval = 20000;   /* us: retry interval while modal */

/* While a modal dialog runs (nested active scheduler), PuTTY must not be
 * re-entered: the event loop just re-arms until the dialog closes. */
static TInt gModal = 0;

/* ------------------------------------------------------------------ */
/* time and entropy                                                     */
/* ------------------------------------------------------------------ */

static TInt TickPeriodUs()
{
    static TInt period = 0;
    if (!period && HAL::Get(HAL::ENanoTickPeriod, period) != KErrNone)
        period = 1000;
    return period;
}

extern "C" unsigned long getticks(void)
{
    /* Wraps around like GetTickCount(); PuTTY's timing code expects that. */
    TUint64 us = TUint64(User::NTickCount()) * TUint64(TickPeriodUs());
    return (unsigned long)(us / 1000);
}

extern "C" void noise_get_heavy(void (*func)(void *, int))
{
    TBuf8<64> buf;
    buf.SetMax();
    rssh_trace("noise_get_heavy: SecureRandomL...");
    TRAPD(err, TRandom::SecureRandomL(buf));
    rssh_trace("noise_get_heavy: err=%d", err);
    if (err != KErrNone)
        rssh_ui_fatal("Could not get random numbers from the system "
                      "random number generator.");
    func((void *)buf.Ptr(), buf.Length());
    Mem::FillZ((TAny *)buf.Ptr(), buf.Length());
}

extern "C" void noise_regular(void)
{
    TUint32 t = User::NTickCount();
    random_add_noise(KNoiseSourceTime, &t, sizeof(t));
    TUint32 c = User::FastCounter();
    random_add_noise(KNoiseSourcePerfCount, &c, sizeof(c));
}

extern "C" void noise_ultralight(int id, unsigned long data)
{
    random_add_noise(id, &data, sizeof(data));
    TUint32 c = User::FastCounter();
    random_add_noise(KNoiseSourcePerfCount, &c, sizeof(c));
}

extern "C" unsigned long long prng_reseed_time_ms(void)
{
    TTime now;
    now.UniversalTime();
    return (unsigned long long)(now.Int64() / 1000);
}

/* ------------------------------------------------------------------ */
/* event loop                                                           */
/* ------------------------------------------------------------------ */

class CTimerAO : public CTimer
{
public:
    static CTimerAO *NewL()
    {
        CTimerAO *self = new (ELeave) CTimerAO;
        CleanupStack::PushL(self);
        self->ConstructL();
        CleanupStack::Pop(self);
        return self;
    }
    void Schedule(unsigned long next)
    {
        Cancel();
        iNext = next;
        long ms = (long)(next - getticks());
        if (ms < 0)
            ms = 0;
        After(TTimeIntervalMicroSeconds32(ms * 1000));
    }
private:
    CTimerAO() : CTimer(EPriorityStandard) { CActiveScheduler::Add(this); }
    void RunL()
    {
        if (gModal) {
            After(KPollInterval);
            return;
        }
        unsigned long next;
        /* Pass the exact value we were given, as timing.c asks. */
        if (run_timers(iNext, &next))
            Schedule(next);
    }
    unsigned long iNext;
};

class CCallbackAO : public CTimer
{
public:
    static CCallbackAO *NewL()
    {
        CCallbackAO *self = new (ELeave) CCallbackAO;
        CleanupStack::PushL(self);
        self->ConstructL();
        CleanupStack::Pop(self);
        return self;
    }
    void Kick()
    {
        if (!IsActive())
            After(0);
    }
private:
    CCallbackAO() : CTimer(EPriorityStandard) { CActiveScheduler::Add(this); }
    void RunL()
    {
        if (gModal) {
            After(KPollInterval);
            return;
        }
        run_toplevel_callbacks();
        if (toplevel_callback_pending())
            Kick();
    }
};

static CTimerAO *gTimer;
static CCallbackAO *gCallbacks;

extern "C" void timer_change_notify(unsigned long next)
{
    if (gTimer)
        gTimer->Schedule(next);
}

static void CallbacksPending(void *)
{
    if (gCallbacks)
        gCallbacks->Kick();
}

/* ------------------------------------------------------------------ */
/* startup / shutdown                                                   */
/* ------------------------------------------------------------------ */

static char gHome[64];

static void SetHomeL()
{
    RFs fs;
    User::LeaveIfError(fs.Connect());
    CleanupClosePushL(fs);
    fs.CreatePrivatePath(EDriveC);
    TFileName priv;
    User::LeaveIfError(fs.PrivatePath(priv));       /* \private\<SID>\ */
    CleanupStack::PopAndDestroy(&fs);

    /* "C:/private/<SID>" - P.I.P.S. accepts '/' separators. */
    TPtr8 home((TUint8 *)gHome, sizeof(gHome) - 1);
    home.Copy(_L8("C:"));
    for (TInt i = 0; i < priv.Length() - 1; i++)
        home.Append(priv[i] == '\\' ? '/' : (TUint8)priv[i]);
    home.ZeroTerminate();
    setenv("HOME", gHome, 1);
    rssh_trace_init(gHome);
    rssh_trace("HOME=%s", gHome);
}

extern "C" const char *rssh_data_dir(void)
{
    return gHome;
}

void RsshPlatformInitL()
{
    SetHomeL();
    gTimer = CTimerAO::NewL();
    gCallbacks = CCallbackAO::NewL();
    request_callback_notifications(CallbacksPending, NULL);
    rssh_trace("platform init ok");
}

TBool RsshPlatformIsModal()
{
    return gModal > 0;
}

void RsshPlatformSetModal(TBool aModal)
{
    gModal += aModal ? 1 : -1;
}

void RsshPlatformShutdown()
{
    rssh_trace("exit: platform: deleting active objects");
    delete gCallbacks;
    gCallbacks = NULL;
    delete gTimer;
    gTimer = NULL;
    rssh_trace("exit: platform: closing socket server");
    RsshSockShutdown();
    rssh_trace("exit: platform: done");
}
