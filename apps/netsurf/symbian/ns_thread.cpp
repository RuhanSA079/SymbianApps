// ns_thread.cpp: see ns_thread.h.
#include <e32base.h>
#include "ns_thread.h"

static RMutex g_mutex;
static TBool g_mutex_ok = EFalse;

extern "C" int ns_mutex_init(void)
{
    if (g_mutex_ok)
        return 0;
    // EOwnerProcess (the default): the handle is valid in every thread.
    TInt err = g_mutex.CreateLocal();
    if (err != KErrNone)
        return err;
    g_mutex_ok = ETrue;
    return 0;
}

extern "C" void ns_mutex_lock(void)
{
    g_mutex.Wait();
}

extern "C" void ns_mutex_unlock(void)
{
    g_mutex.Signal();
}

struct TStart {
    void (*iFn)(void *);
    void *iArg;
};

static TInt ThreadMain(TAny *aPtr)
{
    TStart start = *static_cast<TStart *>(aPtr);
    delete static_cast<TStart *>(aPtr);
    // C code may call leaving Symbian APIs through TRAPs (rsym_tcp,
    // TRandom), which need a cleanup stack.
    CTrapCleanup *cleanup = CTrapCleanup::New();
    if (!cleanup)
        return KErrNoMemory;
    start.iFn(start.iArg);
    delete cleanup;
    return KErrNone;
}

extern "C" int ns_thread_start(void (*fn)(void *), void *arg, int stack_size)
{
    TStart *start = new TStart;
    if (!start)
        return KErrNoMemory;
    start->iFn = fn;
    start->iArg = arg;
    RThread thread;
    // Anonymous thread on the shared heap: memory malloc'd on one thread
    // may be freed on another.
    TInt err = thread.Create(KNullDesC, ThreadMain, stack_size,
                             &User::Allocator(), start);
    if (err != KErrNone) {
        delete start;
        return err;
    }
    thread.Resume();
    thread.Close();         // the thread keeps running without our handle
    return 0;
}
