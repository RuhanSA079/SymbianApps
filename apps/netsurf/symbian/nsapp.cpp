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
 */
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <aknquerydialog.h>
#include <aknnotewrappers.h>
#include <avkon.hrh>
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

const TUid KUidNetSurf = { static_cast<TInt32>(0xE5A1E030) };
_LIT(KPrivate, "\\private\\e5a1e030\\");

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
        ShutdownNetSurf();
        delete iDriver;
        if (iView) {
            RemoveFromStack(iView);
            delete iView;
        }
        gAppUi = NULL;
        rsym_log("exit");
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
        rsym_log_init(iPrivDir, "netsurf");
        rsym_log("start, private dir %s", iPrivDir);
    }

    void StartNetSurfL()
    {
        TRect r = iView->Rect();
        char w[16], h[16];
        sprintf(w, "%d", r.Width());
        sprintf(h, "%d", r.Height());
        // With the debug log on, NetSurf's own (verbose) log goes to
        // netsurf.log in the private directory.
        TBool verbose = rsym_log_enabled();
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
            aMenu->SetItemTextL(ENsCmdDebugLog, rsym_log_enabled() ?
                                _L("Debug log: on") : _L("Debug log: off"));
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
        case ENsCmdDebugLog:
            rsym_log_set(!rsym_log_enabled());
            InfoL(rsym_log_enabled() ?
                  _L("Debug logging is on. NetSurf's own log starts at the next launch.") :
                  _L("Debug logging is off."));
            break;
        case ENsCmdAbout:
            InfoL(_L("NetSurf for Symbian^3\nNetSurf (netsurf-browser.org) ported by RuhanSA079\ngithub.com/RuhanSA079/SymbianApps"));
            break;
        case EAknSoftkeyBack:
            if (nsfb_sym_can_back())
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
        if (aType == KEikDynamicLayoutVariantSwitch && iView)   // rotation
            iView->SetRect(ClientRect());
    }

    CNsView *iView;
    CNsDriver *iDriver;
    CPeriodic *iAutoTest;
    TBool iStarted;
    TBool iShiftDown, iCtrlDown;
    TOrient iOrient;
    TBuf<64> iPriv;         // "X:\private\e5a1e030\"
    char iPrivDir[64];      // "X:/private/e5a1e030"
};

// ---------------------------------------------------------------------------

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
