/*
 * nsapp.cpp: NetSurf for Symbian^3 - the Avkon application around NetSurf's
 * framebuffer frontend.
 *
 * - CNsView shows NetSurf's surface (an EColor16MU bitmap, filled from
 *   nsfb_symbian_update) and turns touch and key events into NetSurf input.
 *   Dragging inside the page scrolls it; a tap is a click.
 * - CNsDriver, an active object, runs NetSurf one step at a time
 *   (nsfb_sym_step) and sleeps on an RTimer for as long as NetSurf says.
 * - The Options menu has the usual browser commands and Screen orientation
 *   (landscape / portrait / automatic), which is remembered.
 * - CNsSettings is the full-screen Settings page (Options > Settings):
 *   browsing options, orientation, the debug log and the remote debug log.
 */
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <aknquerydialog.h>
#include <aknnotewrappers.h>
#include <aknlists.h>
#include <akntitle.h>
#include <avkon.hrh>
#include <avkon.rsg>
#include <badesca.h>
#include <eikbtgpc.h>
#include <eiklbo.h>
#include <eikspane.h>
#include <eikenv.h>
#include <eikmenup.h>
#include <eikstart.h>
#include <coecntrl.h>
#include <fbs.h>
#include <f32file.h>
#include <utf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netsurf.rsg>
#include "nsapp.hrh"

extern "C" {
#include <libnsfb.h>
#include <libnsfb_event.h>
}
#include "nsfb_glue.h"
#include "rsym_log.h"
#include "rsym_rlog.h"

const TUid KUidNetSurf = { static_cast<TInt32>(0xE5A1E030) };
_LIT(KPrivate, "\\private\\e5a1e030\\");
_LIT(KDefaultSuffix, " (default)");

class CNsAppUi;
static CNsAppUi *gAppUi;

// ---------------------------------------------------------------------------
// small helpers

static HBufC *Utf8ToUnicodeLC(const char *aText)
{
    TPtrC8 in(reinterpret_cast<const TUint8 *>(aText), aText ? strlen(aText) : 0);
    HBufC *out = CnvUtfConverter::ConvertToUnicodeFromUtf8L(in);
    CleanupStack::PushL(out);
    return out;
}

static TBool AskYesNoL(const TDesC &aPrompt)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    return dlg->ExecuteLD(R_NS_YESNO_QUERY, aPrompt) != 0;
}

static void InfoL(const TDesC &aText)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    dlg->ExecuteLD(R_NS_OK_QUERY, aText);
}

// One-line text entry (the address query's layout, with its own prompt).
static TBool QueryTextL(const TDesC &aPrompt, TDes &aText)
{
    CAknTextQueryDialog *dlg = CAknTextQueryDialog::NewL(aText);
    dlg->SetPromptL(aPrompt);
    dlg->SetPredictiveTextInputPermitted(EFalse);
    return dlg->ExecuteLD(R_NS_URL_QUERY) != 0;
}

// "C:/private/x/file" (P.I.P.S. style) -> "C:\private\x\file"
static HBufC *SymbianPathLC(const char *aPath)
{
    HBufC *p = Utf8ToUnicodeLC(aPath);
    TPtr ptr = p->Des();
    for (TInt i = 0; i < ptr.Length(); i++)
        if (ptr[i] == '/')
            ptr[i] = '\\';
    return p;
}

// ---------------------------------------------------------------------------
// CNsSettings: the Settings page, a two-line list ("title", "value"). It
// exists only while shown.

class CNsSettings : public CCoeControl, public MEikListBoxObserver
{
public:
    static CNsSettings *NewL(const TRect &aRect)
    {
        CNsSettings *self = new (ELeave) CNsSettings;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CNsSettings()
    {
        delete iListBox;
        delete iItems;
    }

    void ResetL() { iItems->Reset(); }
    void AddRowL(const TDesC &aTitle, const TDesC &aValue)
    {
        HBufC *row = HBufC::NewLC(aTitle.Length() + aValue.Length() + 2);
        TPtr p = row->Des();
        p.Append('\t');
        p.Append(aTitle);
        p.Append('\t');
        p.Append(aValue);
        iItems->AppendL(*row);
        CleanupStack::PopAndDestroy(row);
    }
    void AddRowL(const TDesC &aTitle, const char *aValueUtf8)
    {
        HBufC *v = Utf8ToUnicodeLC(aValueUtf8);
        AddRowL(aTitle, *v);
        CleanupStack::PopAndDestroy(v);
    }
    void DoneL()
    {
        TInt current = iListBox->CurrentItemIndex();
        iListBox->HandleItemAdditionL();
        if (current < 0)
            current = 0;
        if (current >= iItems->Count())
            current = iItems->Count() - 1;
        iListBox->SetCurrentItemIndex(current);
        iListBox->DrawDeferred();
    }
    TInt CurrentIndex() const { return iListBox->CurrentItemIndex(); }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType)
    {
        return iListBox->OfferKeyEventL(aKey, aType);
    }
    void HandleListBoxEventL(CEikListBox *aListBox, TListBoxEvent aEvent);

private:
    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        iListBox = new (ELeave) CAknDoubleStyleListBox;
        iListBox->ConstructL(this, EAknListBoxSelectionList);
        iListBox->CreateScrollBarFrameL(ETrue);
        iListBox->ScrollBarFrame()->SetScrollBarVisibilityL(
            CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
        iListBox->SetListBoxObserver(this);
        iItems = new (ELeave) CDesCArrayFlat(16);
        iListBox->Model()->SetItemTextArray(iItems);
        iListBox->Model()->SetOwnershipType(ELbmDoesNotOwnItemArray);
        SetRect(aRect);
        ActivateL();
    }
    void SizeChanged() { iListBox->SetRect(Rect()); }
    TInt CountComponentControls() const { return 1; }
    CCoeControl *ComponentControl(TInt) const { return iListBox; }

    CAknDoubleStyleListBox *iListBox;
    CDesCArrayFlat *iItems;
};

// ---------------------------------------------------------------------------
// CNsDriver: runs NetSurf's loop from the active scheduler

class CNsDriver : public CActive
{
public:
    static CNsDriver *NewL(CNsAppUi &aUi)
    {
        CNsDriver *self = new (ELeave) CNsDriver(aUi);
        CleanupStack::PushL(self);
        User::LeaveIfError(self->iTimer.CreateLocal());
        CleanupStack::Pop(self);
        return self;
    }
    ~CNsDriver() { Cancel(); iTimer.Close(); }

    // Run as soon as the UI is idle (new input, a menu command, ...).
    void Kick() { Schedule(0); }

    // ms < 0: wait for input only.
    void Schedule(TInt aMs)
    {
        if (iStopped)
            return;
        Cancel();
        if (aMs < 0)
            return;
        if (aMs == 0) {
            iStatus = KRequestPending;
            SetActive();
            TRequestStatus *s = &iStatus;
            User::RequestComplete(s, KErrNone);
        } else {
            if (aMs > 10000)
                aMs = 10000;
            iTimer.After(iStatus, aMs * 1000);
            SetActive();
        }
    }

    void Stop() { iStopped = ETrue; Cancel(); }

private:
    // Below user input, so typing and touch stay responsive while a page
    // lays out.
    CNsDriver(CNsAppUi &aUi) : CActive(EPriorityLow), iUi(aUi)
    {
        CActiveScheduler::Add(this);
    }
    void RunL();
    void DoCancel() { iTimer.Cancel(); }
    TInt RunError(TInt aError)
    {
        rsym_log("driver: RunL left %d", aError);
        Schedule(10);
        return KErrNone;
    }

    CNsAppUi &iUi;
    RTimer iTimer;
    TBool iStopped;
    TInt iSteps;
};

// ---------------------------------------------------------------------------
// CNsView: NetSurf's screen

class CNsView : public CCoeControl
{
public:
    static CNsView *NewL(const TRect &aRect)
    {
        CNsView *self = new (ELeave) CNsView;
        CleanupStack::PushL(self);
        self->CreateWindowL();
        self->SetRect(aRect);
        self->EnableDragEvents();
        self->ActivateL();
        CleanupStack::Pop(self);
        return self;
    }
    ~CNsView() { delete iBitmap; }

    void SetRunning(TBool aRunning) { iRunning = aRunning; }

    // From NetSurf: copy the changed area of its surface and redraw it.
    void Update(const TUint32 *aPixels, TInt aStride, TInt aW, TInt aH,
                TInt aX0, TInt aY0, TInt aX1, TInt aY1)
    {
        if (!iBitmap || iBitmap->SizeInPixels() != TSize(aW, aH)) {
            delete iBitmap;
            iBitmap = NULL;
            CFbsBitmap *bm = new CFbsBitmap;
            if (!bm || bm->Create(TSize(aW, aH), EColor16MU) != KErrNone) {
                delete bm;
                return;
            }
            iBitmap = bm;
            aX0 = 0; aY0 = 0; aX1 = aW; aY1 = aH;     // copy everything
        }
        TInt dstStride = CFbsBitmap::ScanLineLength(aW, EColor16MU);
        iBitmap->LockHeap();
        TUint8 *dst = reinterpret_cast<TUint8 *>(iBitmap->DataAddress());
        const TUint8 *src = reinterpret_cast<const TUint8 *>(aPixels);
        TInt bytes = (aX1 - aX0) * 4;
        for (TInt y = aY0; y < aY1; y++)
            Mem::Copy(dst + y * dstStride + aX0 * 4, src + y * aStride + aX0 * 4, bytes);
        iBitmap->UnlockHeap();
        Window().Invalidate(TRect(aX0, aY0, aX1, aY1));
    }

private:
    void Draw(const TRect &aRect) const
    {
        CWindowGc &gc = SystemGc();
        if (!iBitmap) {
            gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
            gc.SetBrushColor(KRgbWhite);
            gc.Clear(aRect);
            return;
        }
        TRect bmRect(iBitmap->SizeInPixels());
        TRect r(aRect);
        r.Intersection(bmRect);
        if (!r.IsEmpty())
            gc.BitBlt(r.iTl, iBitmap, r);
        // area not covered yet (just after a resize)
        if (aRect.iBr.iX > bmRect.iBr.iX || aRect.iBr.iY > bmRect.iBr.iY) {
            gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
            gc.SetBrushColor(KRgbWhite);
            gc.SetPenStyle(CGraphicsContext::ENullPen);
            if (aRect.iBr.iX > bmRect.iBr.iX)
                gc.DrawRect(TRect(bmRect.iBr.iX, aRect.iTl.iY, aRect.iBr.iX, aRect.iBr.iY));
            if (aRect.iBr.iY > bmRect.iBr.iY)
                gc.DrawRect(TRect(aRect.iTl.iX, bmRect.iBr.iY, aRect.iBr.iX, aRect.iBr.iY));
        }
    }

    void SizeChanged();

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType);
    void HandlePointerEventL(const TPointerEvent &aEvent);

    CFbsBitmap *iBitmap;
    TBool iRunning;
    // touch
    TPoint iDown, iLast;
    TBool iPassThrough;     // pointer down outside the page: plain mouse
    TBool iScrolling;
};

// ---------------------------------------------------------------------------
// CNsAppUi

class CNsAppUi : public CAknAppUi
{
public:
    enum TOrient { EOrientLandscape = 'L', EOrientPortrait = 'P', EOrientAuto = 'A' };

    void ConstructL()
    {
        gAppUi = this;
        InitPrivateDirL();
        iOrient = LoadOrientation();
        TInt flags = EAknEnableSkin;
        if (iOrient == EOrientLandscape)
            flags |= EAppOrientationLandscape;
        else if (iOrient == EOrientPortrait)
            flags |= EAppOrientationPortrait;
        else
            flags |= EAppOrientationAutomatic;
        BaseConstructL(flags);
        // NetSurf has its own toolbar; give it the whole height
        StatusPane()->MakeVisible(EFalse);

        iView = CNsView::NewL(ClientRect());
        AddToStackL(iView);
        iSettingsTick = CPeriodic::NewL(CActive::EPriorityStandard);
        iDriver = CNsDriver::NewL(*this);
        StartNetSurfL();

        // Test hook (emulator diagnostics): C:\Data\netsurf-autotest.txt
        // containing "rotate" switches orientation 20 s after start, as
        // Options > Screen orientation does.
        RFile f;
        if (f.Open(iEikonEnv->FsSession(), _L("C:\\Data\\netsurf-autotest.txt"),
                   EFileRead) == KErrNone) {
            TBuf8<32> b;
            f.Read(b);
            f.Close();
            if (b.Find(_L8("rotate")) >= 0) {
                iAutoTest = CPeriodic::NewL(CActive::EPriorityStandard);
                iAutoTest->Start(20000000, 20000000, TCallBack(AutoRotate, this));
                rsym_log("autotest: rotate in 20 s");
            }
        }
    }

    static TInt AutoRotate(TAny *aSelf)
    {
        CNsAppUi *self = static_cast<CNsAppUi *>(aSelf);
        self->iAutoTest->Cancel();
        TOrient o = self->iOrient == EOrientPortrait ? EOrientLandscape : EOrientPortrait;
        TRAPD(err, self->SetOrientL(o));
        rsym_log("autotest: rotated (%d), view %dx%d", err,
                 self->iView->Rect().Width(), self->iView->Rect().Height());
        return 0;
    }

    ~CNsAppUi()
    {
        delete iAutoTest;
        delete iSettingsTick;
        if (iSettings) {
            RemoveFromStack(iSettings);
            delete iSettings;
        }
        if (iOptionsChanged)
            nsfb_sym_save_options();
        ShutdownNetSurf();
        delete iDriver;
        if (iView) {
            RemoveFromStack(iView);
            delete iView;
        }
        gAppUi = NULL;
        rsym_log("exit");
        rsym_rlog_flush(1000);
        // EKA2L1 hangs in the framework teardown after this; our clean-up is
        // done, so end the process (as rSSH does).
        User::Exit(KErrNone);
    }

    CNsView *View() { return iView; }
    CNsDriver *Driver() { return iDriver; }

    // NetSurf closed its window (toolbar close button)
    void NetSurfQuit()
    {
        rsym_log("netsurf asked to quit");
        Exit();
    }

private:
    void InitPrivateDirL()
    {
        // NetSurf finds its resources at /private/E5A1E030/res on the
        // current drive (NETSURF_FB_RESPATH), so make the drive the app is
        // installed on current.
        RFs &fs = iEikonEnv->FsSession();
        TFileName exe = RProcess().FileName();
        TChar drive = exe.Length() > 0 ? TChar(exe[0]) : TChar('C');
        iPriv.Zero();
        iPriv.Append(drive);
        iPriv.Append(':');
        iPriv.Append(KPrivate);
        fs.MkDirAll(iPriv);

        TBuf8<64> dir8;
        dir8.Copy(iPriv);
        char cdir[64];
        Mem::Copy(cdir, dir8.Ptr(), dir8.Length());
        cdir[dir8.Length()] = 0;
        chdir(cdir);
        // rsym_log wants "X:/private/<SID>" with forward slashes, no trailing one
        for (TInt i = 0; cdir[i]; i++)
            if (cdir[i] == '\\')
                cdir[i] = '/';
        cdir[strlen(cdir) - 1] = 0;
        Mem::Copy(iPrivDir, cdir, strlen(cdir) + 1);
        char choices[96];
        sprintf(choices, "%s/Choices", iPrivDir);
        nsfb_sym_set_user_choices(choices);
        rsym_log_init(iPrivDir, "netsurf");
        rsym_log("start, private dir %s", iPrivDir);
    }

    void StartNetSurfL()
    {
        TRect r = iView->Rect();
        char w[16], h[16];
        sprintf(w, "%d", r.Width());
        sprintf(h, "%d", r.Height());
        // With the debug log (or the remote one) on, NetSurf's own verbose
        // log goes to netsurf.log in the private directory (and the server).
        TBool verbose = rsym_log_enabled() || rsym_rlog_enabled();
        if (verbose) {
            char path[96];
            sprintf(path, "%s/netsurf.log", iPrivDir);
            freopen(path, "w", stderr);
        }
        static char *argv[12];
        int argc = 0;
        argv[argc++] = const_cast<char *>("netsurf");
        if (verbose)
            argv[argc++] = const_cast<char *>("-v");
        argv[argc++] = const_cast<char *>("-f");
        argv[argc++] = const_cast<char *>("symbian");
        argv[argc++] = const_cast<char *>("-b");
        argv[argc++] = const_cast<char *>("32");
        argv[argc++] = const_cast<char *>("-w");
        argv[argc++] = strdup(w);
        argv[argc++] = const_cast<char *>("-h");
        argv[argc++] = strdup(h);
        // C:\Data\netsurf-url.txt: a page to open instead of the home page
        // (first line; handy for testing).
        FILE *uf = fopen("C:\\Data\\netsurf-url.txt", "r");
        if (uf) {
            char url[1024];
            if (fgets(url, sizeof url, uf)) {
                url[strcspn(url, "\r\n")] = 0;
                if (url[0]) {
                    argv[argc++] = strdup(url);
                    rsym_log("netsurf: start URL %s", url);
                }
            }
            fclose(uf);
        }
        argv[argc] = NULL;

        rsym_log("netsurf: start %dx%d", r.Width(), r.Height());
        nsfb_symbian_register();
        if (nsfb_sym_start(argc, argv) != 0) {
            rsym_log("netsurf: start failed");
            InfoL(_L("NetSurf could not open a window."));
            iStarted = ETrue;       // still needs nsfb_sym_finish
            ShutdownNetSurf();
            return;
        }
        iStarted = ETrue;
        iView->SetRunning(ETrue);
        iDriver->Kick();
    }

    void ShutdownNetSurf()
    {
        if (!iStarted)
            return;
        iStarted = EFalse;
        if (iView)
            iView->SetRunning(EFalse);
        if (iDriver)
            iDriver->Stop();
        rsym_log("netsurf: finish");
        nsfb_sym_quit();
        nsfb_sym_finish();      // saves cookies
    }

    // ---- orientation ----

    TOrient LoadOrientation()
    {
        TFileName path(iPriv);
        path.Append(_L("orientation"));
        RFile f;
        TOrient o = EOrientLandscape;   // the E7's keyboard is landscape
        if (f.Open(iEikonEnv->FsSession(), path, EFileRead) == KErrNone) {
            TBuf8<1> b;
            if (f.Read(b) == KErrNone && b.Length() == 1 &&
                (b[0] == EOrientPortrait || b[0] == EOrientAuto || b[0] == EOrientLandscape))
                o = static_cast<TOrient>(b[0]);
            f.Close();
        }
        return o;
    }

    void SetOrientL(TOrient aOrient)
    {
        iOrient = aOrient;
        SetOrientationL(aOrient == EOrientLandscape ? EAppUiOrientationLandscape :
                        aOrient == EOrientPortrait ? EAppUiOrientationPortrait :
                        EAppUiOrientationAutomatic);
        TFileName path(iPriv);
        path.Append(_L("orientation"));
        RFile f;
        if (f.Replace(iEikonEnv->FsSession(), path, EFileWrite) == KErrNone) {
            TBuf8<1> b;
            b.Append(TUint8(aOrient));
            f.Write(b);
            f.Close();
        }
        rsym_log("orientation: %c", (char)aOrient);
    }

    // ---- Settings page ----

public:
    enum { ESetHome, ESetZoom, ESetTextSize, ESetImages, ESetBlockAds, ESetDnt,
           ESetOrient, ESetDebug, ESetLogPath, ESetExport, ESetClear,
           ESetRemote, ESetRemoteHost, ESetRemotePort };

    void SettingsItemL(TInt aIndex)
    {
        switch (aIndex) {
        case ESetHome: {
            char cur[1024];
            HBufC *init = Utf8ToUnicodeLC(nsfb_sym_homepage(cur, sizeof cur));
            TBuf<1024> text;
            text.Copy(init->Left(text.MaxLength()));
            CleanupStack::PopAndDestroy(init);
            if (!QueryTextL(_L("Home page (empty: NetSurf's own)"), text))
                break;
            text.TrimAll();
            HBufC8 *utf8 = HBufC8::NewLC(text.Length() * 3 + 1);
            TPtr8 p = utf8->Des();
            CnvUtfConverter::ConvertFromUnicodeToUtf8(p, text);
            nsfb_sym_set_homepage(reinterpret_cast<const char *>(p.PtrZ()));
            CleanupStack::PopAndDestroy(utf8);
            iOptionsChanged = ETrue;
            break;
        }
        case ESetZoom:
            // applies at once (browser_window_set_scale)
            nsfb_sym_set_option(NSFB_SYM_OPT_SCALE,
                                NextOf(KZooms, KNumZooms, nsfb_sym_option(NSFB_SYM_OPT_SCALE)));
            iOptionsChanged = ETrue;
            break;
        case ESetTextSize:
            nsfb_sym_set_option(NSFB_SYM_OPT_FONT_SIZE,
                                NextOf(KTextSizes, KNumTextSizes,
                                       nsfb_sym_option(NSFB_SYM_OPT_FONT_SIZE)));
            iOptionsChanged = iNeedReload = ETrue;
            break;
        case ESetImages:
        case ESetBlockAds:
        case ESetDnt: {
            TInt opt = aIndex == ESetImages ? NSFB_SYM_OPT_IMAGES :
                       aIndex == ESetBlockAds ? NSFB_SYM_OPT_BLOCK_ADS : NSFB_SYM_OPT_DNT;
            nsfb_sym_set_option(opt, !nsfb_sym_option(opt));
            iOptionsChanged = ETrue;
            iNeedReload = iNeedReload || aIndex != ESetDnt;
            break;
        }
        case ESetOrient:
            SetOrientL(iOrient == EOrientLandscape ? EOrientPortrait :
                       iOrient == EOrientPortrait ? EOrientAuto : EOrientLandscape);
            break;
        case ESetDebug:
            rsym_log_set(!rsym_log_enabled());
            if (rsym_log_enabled())
                InfoL(_L("Debug logging is on. NetSurf's own log is included from the next launch."));
            break;
        case ESetLogPath: {
            HBufC *path = SymbianPathLC(rsym_log_path());
            HBufC *msg = HBufC::NewLC(path->Length() + 120);
            msg->Des().Format(_L("Debug log:\n%S\n(app-private; use Export to copy it out)"), path);
            InfoL(*msg);
            CleanupStack::PopAndDestroy(2, path);
            break;
        }
        case ESetExport:
            ExportLogsL();
            break;
        case ESetClear:
            if (AskYesNoL(_L("Delete the debug log?"))) {
                rsym_log_clear();
                InfoL(_L("Debug log deleted."));
            }
            break;
        case ESetRemote: {
            TBool on = !rsym_rlog_enabled();
            if (on && !rsym_rlog_host()[0] && !EditRemoteHostL())
                break;
            rsym_rlog_configure(on, NULL, 0);
            if (on)
                InfoL(_L("Remote debug log is on. Run env/rlog-server.py on the PC. NetSurf's own log is included from the next launch."));
            break;
        }
        case ESetRemoteHost:
            EditRemoteHostL();
            break;
        case ESetRemotePort: {
            TBuf<16> text;
            text.AppendNum(rsym_rlog_port());
            if (!QueryTextL(_L("Remote debug port (default 7865)"), text))
                break;
            TLex lex(text);
            TInt port;
            if (lex.Val(port) != KErrNone || port <= 0 || port > 65535) {
                InfoL(_L("The port must be a number from 1 to 65535."));
                break;
            }
            rsym_rlog_configure(rsym_rlog_enabled(), NULL, port);
            break;
        }
        default:
            break;
        }
        RefreshSettingsL();
    }

private:
    static const TInt KZooms[];
    static const TInt KNumZooms = 6;
    static const TInt KTextSizes[];
    static const TInt KNumTextSizes = 4;

    // the value after aCur in aList (wrapping), or the first one
    static TInt NextOf(const TInt *aList, TInt aCount, TInt aCur)
    {
        for (TInt i = 0; i < aCount; i++)
            if (aList[i] > aCur)
                return aList[i];
        return aList[0];
    }

    void ShowSettingsL()
    {
        if (iSettings)
            return;
        // the page has a title; NetSurf's screen goes without the status pane
        StatusPane()->MakeVisible(ETrue);
        CAknTitlePane *title = static_cast<CAknTitlePane *>(
            StatusPane()->ControlL(TUid::Uid(EEikStatusPaneUidTitle)));
        title->SetTextL(_L("Settings"));
        iSettings = CNsSettings::NewL(ClientRect());
        RemoveFromStack(iView);
        iView->MakeVisible(EFalse);
        AddToStackL(iSettings);
        RefreshSettingsL();
        CEikButtonGroupContainer *cba = CEikButtonGroupContainer::Current();
        cba->SetCommandSetL(R_AVKON_SOFTKEYS_SELECT_BACK);
        cba->DrawDeferred();
        iSettingsTick->Cancel();
        iSettingsTick->Start(2000000, 2000000, TCallBack(SettingsTick, this));
        rsym_log("settings: shown");
    }

    // From the Back softkey (never from inside the list's own callbacks), so
    // the list can be deleted here.
    void CloseSettingsL()
    {
        iSettingsTick->Cancel();
        RemoveFromStack(iSettings);
        delete iSettings;
        iSettings = NULL;
        StatusPane()->MakeVisible(EFalse);
        iView->SetRect(ClientRect());       // also picks up a rotation
        iView->MakeVisible(ETrue);
        AddToStackL(iView);
        iView->DrawDeferred();
        CEikButtonGroupContainer *cba = CEikButtonGroupContainer::Current();
        cba->SetCommandSetL(R_AVKON_SOFTKEYS_OPTIONS_BACK);
        cba->DrawDeferred();
        if (iOptionsChanged) {
            TInt err = nsfb_sym_save_options();
            rsym_log("settings: saved (%d)", err);
        }
        if (iNeedReload) {
            iNeedReload = EFalse;
            nsfb_sym_reload();
        }
    }

    static TInt SettingsTick(TAny *aSelf)
    {
        CNsAppUi *self = static_cast<CNsAppUi *>(aSelf);
        char status[96];
        rsym_rlog_status(status, sizeof status);
        if (self->iSettings && self->iRemoteStatus != TPtrC8((const TUint8 *)status))
            TRAP_IGNORE(self->RefreshSettingsL());
        return 0;
    }

    void RefreshSettingsL()
    {
        if (!iSettings)
            return;
        CNsSettings &s = *iSettings;
        char buf[1024];
        s.ResetL();
        nsfb_sym_homepage(buf, sizeof buf);
        s.AddRowL(_L("Home page"), buf[0] ? buf : "NetSurf's own (default)");
        TBuf<32> v;
        v.Format(_L("%d%%"), nsfb_sym_option(NSFB_SYM_OPT_SCALE));
        s.AddRowL(_L("Zoom"), v);
        TInt fs = nsfb_sym_option(NSFB_SYM_OPT_FONT_SIZE);
        v.Format(_L("%d.%d pt%S"), fs / 10, fs % 10,
                 fs == 128 ? &KDefaultSuffix() : &KNullDesC());
        s.AddRowL(_L("Text size"), v);
        s.AddRowL(_L("Load images"), nsfb_sym_option(NSFB_SYM_OPT_IMAGES) ? _L("On") : _L("Off"));
        s.AddRowL(_L("Block adverts"), nsfb_sym_option(NSFB_SYM_OPT_BLOCK_ADS) ? _L("On") : _L("Off"));
        s.AddRowL(_L("Send Do Not Track"), nsfb_sym_option(NSFB_SYM_OPT_DNT) ? _L("On") : _L("Off"));
        s.AddRowL(_L("Screen orientation"),
                  iOrient == EOrientLandscape ? _L("Landscape") :
                  iOrient == EOrientPortrait ? _L("Portrait") : _L("Automatic"));
        s.AddRowL(_L("Debug log"), rsym_log_enabled() ? _L("On") : _L("Off (default)"));
        HBufC *path = SymbianPathLC(rsym_log_path());
        s.AddRowL(_L("Debug log location"), *path);
        CleanupStack::PopAndDestroy(path);
        s.AddRowL(_L("Export debug log"), _L("copy to E:\\NetSurf\\ (mass memory)"));
        s.AddRowL(_L("Clear debug log"), _L("delete the log file"));
        char status[96];
        rsym_rlog_status(status, sizeof status);
        iRemoteStatus.Copy(TPtrC8((const TUint8 *)status));
        s.AddRowL(_L("Remote debug log"), status);
        s.AddRowL(_L("Remote debug host"),
                  rsym_rlog_host()[0] ? rsym_rlog_host() : "not set (IP address of the log server)");
        v.Zero();
        v.AppendNum(rsym_rlog_port());
        s.AddRowL(_L("Remote debug port"), v);
        s.DoneL();
    }

    // Ask for the log server's address. EFalse if cancelled or empty.
    TBool EditRemoteHostL()
    {
        HBufC *cur = Utf8ToUnicodeLC(rsym_rlog_host());
        TBuf<64> text(cur->Left(64));
        CleanupStack::PopAndDestroy(cur);
        if (!QueryTextL(_L("Remote debug host (log server IP address)"), text))
            return EFalse;
        text.TrimAll();
        TBuf8<200> host;        // room for UTF-8 and PtrZ's terminator
        CnvUtfConverter::ConvertFromUnicodeToUtf8(host, text);
        rsym_rlog_configure(rsym_rlog_enabled(), reinterpret_cast<const char *>(host.PtrZ()), 0);
        return text.Length() > 0;
    }

    // Copy the logs out of the private directory to mass memory (E:), or to
    // C:\Data if there is no E: drive.
    void ExportLogsL()
    {
        RFs &fs = iEikonEnv->FsSession();
        CFileMan *fm = CFileMan::NewL(fs);
        CleanupStack::PushL(fm);
        TFileName src(iPriv);
        src.Append(_L("*.log"));        // netsurf-debug.log, netsurf.log
        TPtrC dest(_L("E:\\NetSurf\\"));
        TInt err = fs.MkDirAll(dest);
        if (err == KErrNone || err == KErrAlreadyExists)
            err = fm->Copy(src, dest, CFileMan::EOverWrite);
        if (err != KErrNone && err != KErrNotFound) {
            dest.Set(_L("C:\\Data\\NetSurf\\"));
            err = fs.MkDirAll(dest);
            if (err == KErrNone || err == KErrAlreadyExists)
                err = fm->Copy(src, dest, CFileMan::EOverWrite);
        }
        CleanupStack::PopAndDestroy(fm);
        rsym_log("export logs -> %d", err);
        TBuf<128> msg;
        if (err == KErrNone)
            msg.Format(_L("Debug logs exported to\n%S"), &dest);
        else if (err == KErrNotFound)
            msg.Copy(_L("There is no debug log yet. Turn on Debug log first."));
        else
            msg.Format(_L("Export failed (error %d)."), err);
        InfoL(msg);
    }

    // ---- commands ----

    void OpenUrlL()
    {
        char cur[1024];
        nsfb_sym_current_url(cur, sizeof cur);
        HBufC *init = Utf8ToUnicodeLC(cur);
        TBuf<1024> text;
        text.Copy(init->Left(text.MaxLength()));
        CleanupStack::PopAndDestroy(init);
        CAknTextQueryDialog *dlg = CAknTextQueryDialog::NewL(text);
        dlg->SetPredictiveTextInputPermitted(EFalse);
        if (dlg->ExecuteLD(R_NS_URL_QUERY)) {
            HBufC8 *utf8 = CnvUtfConverter::ConvertFromUnicodeToUtf8L(text);
            CleanupStack::PushL(utf8);
            TPtr8 p = utf8->Des();
            HBufC8 *z = HBufC8::NewLC(p.Length() + 1);
            z->Des().Copy(p);
            z->Des().ZeroTerminate();
            nsfb_sym_open_url(reinterpret_cast<const char *>(z->Ptr()));
            CleanupStack::PopAndDestroy(2);
        }
    }

    void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenu)
    {
        if (aResourceId == R_NS_ROTATE_MENU) {
            TInt id = iOrient == EOrientLandscape ? ENsCmdLandscape :
                      iOrient == EOrientPortrait ? ENsCmdPortrait : ENsCmdAutoRotate;
            aMenu->SetItemButtonState(id, EEikMenuItemSymbolOn);
        } else if (aResourceId == R_NS_MENU) {
            aMenu->SetItemDimmed(ENsCmdBack, !nsfb_sym_can_back());
        }
    }

    void HandleCommandL(TInt aCommand)
    {
        switch (aCommand) {
        case ENsCmdOpenUrl:    OpenUrlL(); break;
        case ENsCmdBack:       nsfb_sym_back(); break;
        case ENsCmdForward:    nsfb_sym_forward(); break;
        case ENsCmdReload:     nsfb_sym_reload(); break;
        case ENsCmdStop:       nsfb_sym_stop(); break;
        case ENsCmdHome:       nsfb_sym_home(); break;
        case ENsCmdLandscape:  SetOrientL(EOrientLandscape); break;
        case ENsCmdPortrait:   SetOrientL(EOrientPortrait); break;
        case ENsCmdAutoRotate: SetOrientL(EOrientAuto); break;
        case ENsCmdSettings:   ShowSettingsL(); break;
        case EAknSoftkeySelect:
            if (iSettings)
                SettingsItemL(iSettings->CurrentIndex());
            break;
        case ENsCmdAbout:
            InfoL(_L("NetSurf for Symbian^3\nNetSurf (netsurf-browser.org) ported by RuhanSA079\ngithub.com/RuhanSA079/SymbianApps\nApp icon: SVG Repo (www.svgrepo.com)"));
            break;
        case EAknSoftkeyBack:
            if (iSettings)
                CloseSettingsL();
            else if (nsfb_sym_can_back())
                nsfb_sym_back();
            else if (AskYesNoL(_L("Exit NetSurf?")))
                Exit();
            break;
        case ENsCmdExit:
        case EAknSoftkeyExit:
        case EAknCmdExit:
        case EEikCmdExit:
            Exit();
            break;
        default:
            break;
        }
        if (iDriver)
            iDriver->Kick();
    }

    // EKA2L1 workarounds, as in rSSH: it sends letters upper-case without
    // Shift and never sets modifier flags (but does send Shift/Ctrl
    // key-down/up events). Harmless on a phone.
    void HandleWsEventL(const TWsEvent &aEvent, CCoeControl *aDestination)
    {
        TInt type = aEvent.Type();
        if (type == EEventKeyDown || type == EEventKeyUp) {
            TInt scan = aEvent.Key()->iScanCode;
            TBool down = type == EEventKeyDown;
            if (scan == EStdKeyLeftShift || scan == EStdKeyRightShift)
                iShiftDown = down;
            else if (scan == EStdKeyLeftCtrl || scan == EStdKeyRightCtrl)
                iCtrlDown = down;
        } else if (type == EEventKey) {
            TKeyEvent *key = aEvent.Key();
            if (iShiftDown)
                key->iModifiers |= EModifierShift | EModifierLeftShift;
            if (iCtrlDown)
                key->iModifiers |= EModifierCtrl | EModifierLeftCtrl;
            if (key->iCode >= 'A' && key->iCode <= 'Z' &&
                !(key->iModifiers & (EModifierShift | EModifierCapsLock)))
                key->iCode += 'a' - 'A';
        }
        CAknAppUi::HandleWsEventL(aEvent, aDestination);
    }

    void HandleResourceChangeL(TInt aType)
    {
        CAknAppUi::HandleResourceChangeL(aType);
        if (aType == KEikDynamicLayoutVariantSwitch) {          // rotation
            if (iSettings)
                iSettings->SetRect(ClientRect());   // the page: on leaving
            else if (iView)
                iView->SetRect(ClientRect());
        }
    }

    CNsView *iView;
    CNsDriver *iDriver;
    CPeriodic *iAutoTest;
    CNsSettings *iSettings;     // only while the Settings page is shown
    CPeriodic *iSettingsTick;   // refreshes the remote log status there
    TBuf8<96> iRemoteStatus;    // as last shown
    TBool iOptionsChanged;      // save NetSurf's options on leaving Settings
    TBool iNeedReload;          // ... and reload the page
    TBool iStarted;
    TBool iShiftDown, iCtrlDown;
    TOrient iOrient;
    TBuf<64> iPriv;         // "X:\private\e5a1e030\"
    char iPrivDir[64];      // "X:/private/e5a1e030"
};

// ---------------------------------------------------------------------------

const TInt CNsAppUi::KZooms[] = { 50, 75, 100, 125, 150, 200 };
const TInt CNsAppUi::KTextSizes[] = { 100, 128, 160, 200 };

void CNsSettings::HandleListBoxEventL(CEikListBox *, TListBoxEvent aEvent)
{
    if (aEvent == EEventEnterKeyPressed || aEvent == EEventItemSingleClicked ||
        aEvent == EEventItemDoubleClicked)
        gAppUi->SettingsItemL(iListBox->CurrentItemIndex());
}

void CNsDriver::RunL()
{
    TInt next = nsfb_sym_step();
    if (next < 0 || next > 2000 || ++iSteps % 200 == 0)
        rsym_log("driver: step %d, next in %d ms", iSteps, next);
    if (next == -2) {
        Stop();
        iUi.NetSurfQuit();
        return;
    }
    Schedule(next);
}

void CNsView::SizeChanged()
{
    // Rotation: NetSurf resizes its surface on the next step (gui_resize)
    // and redraws everything, which recreates our bitmap at the new size.
    if (iRunning) {
        rsym_log("view: resize to %dx%d", Rect().Width(), Rect().Height());
        nsfb_sym_resize(Rect().Width(), Rect().Height());
        if (gAppUi && gAppUi->Driver())
            gAppUi->Driver()->Kick();
    }
}

TKeyResponse CNsView::OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType)
{
    if (aType != EEventKey || !iRunning)
        return EKeyWasNotConsumed;
    TInt code = -1;
    switch (aKey.iCode) {
    case EKeyLeftArrow:  code = NSFB_KEY_LEFT; break;
    case EKeyRightArrow: code = NSFB_KEY_RIGHT; break;
    case EKeyUpArrow:    code = NSFB_KEY_UP; break;
    case EKeyDownArrow:  code = NSFB_KEY_DOWN; break;
    case EKeyBackspace:  code = NSFB_KEY_BACKSPACE; break;
    case EKeyDelete:     code = NSFB_KEY_DELETE; break;
    case EKeyEnter:
    case EKeyDevice3:    code = NSFB_KEY_RETURN; break;
    case EKeyEscape:     code = NSFB_KEY_ESCAPE; break;
    case EKeyTab:        code = NSFB_KEY_TAB; break;
    case EKeyPageUp:     code = NSFB_KEY_PAGEUP; break;
    case EKeyPageDown:   code = NSFB_KEY_PAGEDOWN; break;
    case EKeyHome:       code = NSFB_KEY_HOME; break;
    case EKeyEnd:        code = NSFB_KEY_END; break;
    default:
        break;
    }
    if (code < 0 && aKey.iCode >= 0x20 && aKey.iCode < ENonCharacterKeyBase) {
        TUint c = aKey.iCode;
        if ((aKey.iModifiers & EModifierCtrl) && c < 0x80 && TChar(c).IsAlpha()) {
            // Ctrl+letter: NetSurf's shortcuts (copy, paste, select all, ...)
            nsfb_sym_key(1, NSFB_KEY_LCTRL);
            nsfb_sym_key(1, NSFB_KEY_a + (TChar(c).GetLowerCase() - 'a'));
            nsfb_sym_key(0, NSFB_KEY_a + (TChar(c).GetLowerCase() - 'a'));
            nsfb_sym_key(0, NSFB_KEY_LCTRL);
            gAppUi->Driver()->Kick();
            return EKeyWasConsumed;
        }
        code = NSFB_SYM_UCS4 + c;
    }
    if (code < 0)
        return EKeyWasNotConsumed;
    nsfb_sym_key(1, code);
    nsfb_sym_key(0, code);
    gAppUi->Driver()->Kick();
    return EKeyWasConsumed;
}

void CNsView::HandlePointerEventL(const TPointerEvent &aEvent)
{
    if (!iRunning)
        return;
    const TInt KDragStart = 12;     // pixels before a touch becomes a scroll
    TPoint p = aEvent.iPosition;
    switch (aEvent.iType) {
    case TPointerEvent::EButton1Down:
        iDown = iLast = p;
        iScrolling = EFalse;
        // Outside the page (toolbar, scroll bars): an ordinary mouse.
        iPassThrough = !nsfb_sym_in_content(p.iX, p.iY);
        if (iPassThrough) {
            nsfb_sym_pointer_move(p.iX, p.iY);
            nsfb_sym_pointer_button(1);
        }
        break;
    case TPointerEvent::EDrag:
        if (iPassThrough) {
            nsfb_sym_pointer_move(p.iX, p.iY);
        } else {
            if (!iScrolling && (Abs(p.iX - iDown.iX) > KDragStart ||
                                Abs(p.iY - iDown.iY) > KDragStart))
                iScrolling = ETrue;
            if (iScrolling) {
                nsfb_sym_scroll(iLast.iX - p.iX, iLast.iY - p.iY);
                iLast = p;
            }
        }
        break;
    case TPointerEvent::EButton1Up:
        if (iPassThrough) {
            nsfb_sym_pointer_move(p.iX, p.iY);
            nsfb_sym_pointer_button(0);
        } else if (!iScrolling) {
            // a tap: click where the finger went down
            nsfb_sym_pointer_move(iDown.iX, iDown.iY);
            nsfb_sym_pointer_button(1);
            nsfb_sym_pointer_button(0);
        }
        iPassThrough = iScrolling = EFalse;
        break;
    default:
        return;
    }
    gAppUi->Driver()->Kick();
}

// ---------------------------------------------------------------------------
// nsfb_glue.h, implemented here

extern "C" void nsfb_symbian_update(const unsigned int *pixels, int stride_bytes,
                                    int width, int height,
                                    int x0, int y0, int x1, int y1)
{
    if (gAppUi && gAppUi->View())
        gAppUi->View()->Update(reinterpret_cast<const TUint32 *>(pixels), stride_bytes,
                               width, height, x0, y0, x1, y1);
}

// ---------------------------------------------------------------------------

class CNsDocument : public CAknDocument
{
public:
    CNsDocument(CEikApplication &aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi *CreateAppUiL() { return new (ELeave) CNsAppUi; }
};

class CNsApplication : public CAknApplication
{
private:
    TUid AppDllUid() const { return KUidNetSurf; }
    CApaDocument *CreateDocumentL() { return new (ELeave) CNsDocument(*this); }
};

LOCAL_C CApaApplication *NewApplication() { return new CNsApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
