// rDrive: a Google Drive client for Symbian^3 (experimental).
//
// Milestone 1: prove HTTPS (mbedTLS 4 on native sockets) works against
// Google from the phone. Network requests run on a worker thread with plain
// blocking code; the UI thread hears back through an active object.

#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <akntitle.h>
#include <avkon.hrh>
#include <eikenv.h>
#include <eikstart.h>
#include <coecntrl.h>
#include <f32file.h>
#include <utf.h>

#include <rdrive.rsg>
#include "rdrive.hrh"
#include "rsym_https.h"
#include "rsym_log.h"

const TUid KUidRdrive = { TInt32(0xE5A1E020) };

// ---------------------------------------------------------------------------
// helpers

static HBufC *Utf8ToUnicodeLC(const char *aUtf8, TInt aLen = -1)
{
    if (aLen < 0)
        aLen = aUtf8 ? User::StringLength((const TUint8 *)aUtf8) : 0;
    TPtrC8 in((const TUint8 *)aUtf8, aLen);
    HBufC *out = HBufC::NewLC(in.Length() + 1);
    TPtr ptr = out->Des();
    if (CnvUtfConverter::ConvertToUnicodeFromUtf8(ptr, in) < 0)
        ptr.Copy(in);
    return out;
}

// ---------------------------------------------------------------------------
// log view: word-wrapped lines, newest at the bottom; arrows/drag scroll

class CLogView : public CCoeControl
{
public:
    static CLogView *NewL(const TRect &aRect)
    {
        CLogView *self = new (ELeave) CLogView;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CLogView() { iLines.ResetAndDestroy(); }

    void AddL(const TDesC &aText)
    {
        TInt width = Rect().Width() - 2 * KMargin;
        TPtrC rest(aText);
        do {
            TInt nl = rest.Locate('\n');
            TPtrC para = nl >= 0 ? rest.Left(nl) : rest;
            rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
            do {
                TInt fit = width > 20 ? iFont->TextCount(para, width) : para.Length();
                if (fit <= 0)
                    fit = 1;
                if (fit < para.Length()) {
                    TInt space = para.Left(fit).LocateReverse(' ');
                    if (space > 0)
                        fit = space;
                }
                iLines.AppendL(para.Left(fit).AllocL());
                para.Set(para.Mid(fit));
                while (para.Length() && para[0] == ' ')
                    para.Set(para.Mid(1));
            } while (para.Length());
        } while (rest.Length());
        iTop = iLines.Count() - VisibleLines();
        if (iTop < 0)
            iTop = 0;
        DrawDeferred();
    }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
    {
        if (aType != EEventKey)
            return EKeyWasNotConsumed;
        if (aKeyEvent.iCode == EKeyUpArrow)   { ScrollBy(-1); return EKeyWasConsumed; }
        if (aKeyEvent.iCode == EKeyDownArrow) { ScrollBy(1); return EKeyWasConsumed; }
        return EKeyWasNotConsumed;
    }

    void HandlePointerEventL(const TPointerEvent &aEvent)
    {
        if (aEvent.iType == TPointerEvent::EButton1Down) {
            iDragY = aEvent.iPosition.iY;
        } else if (aEvent.iType == TPointerEvent::EDrag) {
            TInt lines = (iDragY - aEvent.iPosition.iY) / iLineH;
            if (lines) {
                ScrollBy(lines);
                iDragY -= lines * iLineH;
            }
        }
    }

private:
    CLogView() {}
    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        EnableDragEvents();
        iFont = iEikonEnv->DenseFont();
        iLineH = iFont->FontMaxHeight() + 3;
        SetRect(aRect);
        ActivateL();
    }
    TInt VisibleLines() const
    {
        TInt n = (Rect().Height() - 2 * KMargin) / iLineH;
        return n > 1 ? n : 1;
    }
    void ScrollBy(TInt aLines)
    {
        TInt maxTop = iLines.Count() - VisibleLines();
        if (maxTop < 0) maxTop = 0;
        TInt top = iTop + aLines;
        if (top < 0) top = 0;
        if (top > maxTop) top = maxTop;
        if (top != iTop) {
            iTop = top;
            DrawDeferred();
        }
    }
    void Draw(const TRect &) const
    {
        CWindowGc &gc = SystemGc();
        gc.SetPenStyle(CGraphicsContext::ENullPen);
        gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        gc.SetBrushColor(KRgbWhite);
        gc.DrawRect(Rect());
        gc.SetBrushStyle(CGraphicsContext::ENullBrush);
        gc.SetPenStyle(CGraphicsContext::ESolidPen);
        gc.SetPenColor(KRgbBlack);
        gc.UseFont(iFont);
        TInt y = Rect().iTl.iY + KMargin + iFont->FontMaxAscent();
        for (TInt i = iTop; i < iLines.Count() && i < iTop + VisibleLines(); i++) {
            gc.DrawText(*iLines[i], TPoint(Rect().iTl.iX + KMargin, y));
            y += iLineH;
        }
        gc.DiscardFont();
    }

    static const TInt KMargin = 6;
    const CFont *iFont;
    TInt iLineH, iTop, iDragY;
    RPointerArray<HBufC> iLines;
};

// ---------------------------------------------------------------------------
// one HTTPS request on a worker thread

class MNetObserver
{
public:
    virtual void NetDoneL(const rsym_http_response &aResp, TInt aResult, TUint aMs) = 0;
};

class CNetTask : public CActive
{
public:
    CNetTask(MNetObserver &aObserver) : CActive(EPriorityStandard), iObserver(aObserver)
    {
        CActiveScheduler::Add(this);
    }
    ~CNetTask()
    {
        Cancel();
        rsym_http_response_free(&iJob.iResp);
    }

    TBool Busy() const { return IsActive(); }

    // aReq's strings must stay valid until NetDoneL.
    TInt Start(const rsym_http_request &aReq)
    {
        if (IsActive())
            return KErrInUse;
        rsym_http_response_free(&iJob.iResp);
        iJob.iReq = aReq;
        iJob.iStatus = &iStatus;
        iJob.iUiThread = RThread().Id();
        TInt err = iThread.Create(KNullDesC, ThreadMain, 0x14000, &User::Allocator(), &iJob);
        if (err != KErrNone)
            return err;
        iStatus = KRequestPending;
        SetActive();
        iThread.Resume();
        return KErrNone;
    }

private:
    struct TJob
    {
        rsym_http_request iReq;
        rsym_http_response iResp;
        TInt iResult;
        TUint iMs;
        TRequestStatus *iStatus;
        TThreadId iUiThread;
    };

    static TInt ThreadMain(TAny *aPtr)
    {
        TJob *job = static_cast<TJob *>(aPtr);
        CTrapCleanup *cleanup = CTrapCleanup::New();
        TUint start = User::NTickCount();
        job->iResult = rsym_https_request(&job->iReq, &job->iResp);
        job->iMs = User::NTickCount() - start;     /* ticks; ms on Symbian^3 */
        RThread ui;
        if (ui.Open(job->iUiThread) == KErrNone) {
            ui.RequestComplete(job->iStatus, KErrNone);
            ui.Close();
        }
        delete cleanup;
        return 0;
    }

    void RunL()
    {
        iThread.Close();
        iObserver.NetDoneL(iJob.iResp, iJob.iResult, iJob.iMs);
    }

    void DoCancel()
    {
        iThread.Kill(KErrCancel);
        iThread.Close();
        TRequestStatus *s = &iStatus;
        User::RequestComplete(s, KErrCancel);
    }

    MNetObserver &iObserver;
    RThread iThread;
    TJob iJob;
};

// ---------------------------------------------------------------------------
// app UI

class CRdriveAppUi : public CAknAppUi, public MNetObserver
{
public:
    void ConstructL()
    {
        BaseConstructL(EAknEnableSkin | EAppOrientationLandscape);
        InitPrivateDirL();
        iView = CLogView::NewL(ClientRect());
        AddToStackL(iView);
        iNet = new (ELeave) CNetTask(*this);
        iView->AddL(_L("rDrive 0.1 (experimental) - Google Drive for Symbian^3"));
        iView->AddL(_L("Options > Test HTTPS checks TLS to Google (no login needed)."));

        // Test hook (emulator diagnostics): with C:\Data\rdrive-autotest
        // present, run the HTTPS test at once and exit when it is done.
        TEntry entry;
        if (iEikonEnv->FsSession().Entry(_L("C:\\Data\\rdrive-autotest"), entry) == KErrNone) {
            iAutoTest = ETrue;
            rsym_log("autotest: start");
            TestHttpsL();
        }
    }

    ~CRdriveAppUi()
    {
        delete iNet;
        if (iView) {
            RemoveFromStack(iView);
            delete iView;
        }
        rsym_log("exit");
        // EKA2L1 hangs in the framework teardown after this; our clean-up is
        // done, so end the process (as rSSH does).
        User::Exit(KErrNone);
    }

private:
    void InitPrivateDirL()
    {
        RFs &fs = iEikonEnv->FsSession();
        fs.CreatePrivatePath(EDriveC);
        TFileName priv;
        User::LeaveIfError(fs.PrivatePath(priv));        // \private\<SID>\ (no drive)
        TPtr8 dir(reinterpret_cast<TUint8 *>(iPrivDir), 0, sizeof(iPrivDir) - 1);
        dir.Copy(_L8("C:"));
        for (TInt i = 0; i < priv.Length() - 1; i++)
            dir.Append(priv[i] == '\\' ? '/' : TUint8(priv[i]));
        dir.ZeroTerminate();
        rsym_log_init(iPrivDir, "rdrive");
    }

    void TestHttpsL()
    {
        if (iNet->Busy()) {
            iView->AddL(_L("(a request is already running)"));
            return;
        }
        rsym_http_request req;
        Mem::FillZ(&req, sizeof(req));
        req.method = "GET";
        req.host = "www.googleapis.com";
        req.path = "/discovery/v1/apis?name=drive&preferred=true";
        req.timeout_ms = 30000;
        iView->AddL(_L("GET https://www.googleapis.com/discovery/v1/apis?name=drive ..."));
        TInt err = iNet->Start(req);
        if (err != KErrNone) {
            TBuf<64> msg;
            msg.Format(_L("Could not start the worker thread (%d)"), err);
            iView->AddL(msg);
        }
    }

    void NetDoneL(const rsym_http_response &aResp, TInt aResult, TUint aMs)
    {
        TBuf<200> line;
        if (aResult != 0) {
            HBufC *e = Utf8ToUnicodeLC(aResp.error);
            line.Format(_L("FAILED after %u ms: "), aMs);
            line.Append(e->Left(line.MaxLength() - line.Length()));
            CleanupStack::PopAndDestroy(e);
            iView->AddL(line);
            rsym_log("test: FAILED after %u ms: %s", aMs, aResp.error);
            if (iAutoTest)
                Exit();
            return;
        }
        line.Format(_L("HTTP %d, %d bytes in %u ms"), aResp.status, aResp.body_len, aMs);
        iView->AddL(line);
        rsym_log("test: HTTP %d, %d bytes in %u ms", aResp.status, aResp.body_len, aMs);
        if (aResp.body)
            rsym_log("test: body starts: %.120s", (const char *)aResp.body);
        if (aResp.body && aResp.body_len > 0) {
            HBufC *b = Utf8ToUnicodeLC((const char *)aResp.body,
                                       aResp.body_len < 300 ? aResp.body_len : 300);
            iView->AddL(*b);
            CleanupStack::PopAndDestroy(b);
        }
        if (iAutoTest)
            Exit();
    }

    void HandleCommandL(TInt aCommand)
    {
        switch (aCommand) {
        case ERdriveCmdTestHttps:
            TestHttpsL();
            break;
        case ERdriveCmdExit:
        case EAknSoftkeyExit:
        case EAknCmdExit:
        case EEikCmdExit:
            Exit();
            break;
        default:
            break;
        }
    }

    void HandleResourceChangeL(TInt aType)
    {
        CAknAppUi::HandleResourceChangeL(aType);
        if (aType == KEikDynamicLayoutVariantSwitch && iView)
            iView->SetRect(ClientRect());
    }

    CLogView *iView;
    CNetTask *iNet;
    char iPrivDir[64];
    TBool iAutoTest;
};

// ---------------------------------------------------------------------------

class CRdriveDocument : public CAknDocument
{
public:
    CRdriveDocument(CEikApplication &aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi *CreateAppUiL() { return new (ELeave) CRdriveAppUi; }
};

class CRdriveApplication : public CAknApplication
{
private:
    TUid AppDllUid() const { return KUidRdrive; }
    CApaDocument *CreateDocumentL() { return new (ELeave) CRdriveDocument(*this); }
};

LOCAL_C CApaApplication *NewApplication() { return new CRdriveApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
