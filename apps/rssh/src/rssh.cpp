// rSSH: an SSH client for Symbian^3 built on PuTTY 0.85 (experimental).
//
// The C++ side: the Avkon application, a terminal view that keeps a
// character grid filled by PuTTY's terminal through rssh_ui_draw(), key
// handling, dialogs, and the rssh_ui_* C functions from rssh_ui.h.

#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <akntitle.h>
#include <aknquerydialog.h>
#include <aknmessagequerydialog.h>
#include <aknnotewrappers.h>
#include <aknpopupheadingpane.h>
#include <avkon.hrh>
#include <avkon.rsg>
#include <eikenv.h>
#include <eikspane.h>
#include <eikstart.h>
#include <eikmenup.h>
#include <coecntrl.h>
#include <aknlists.h>
#include <eiklbo.h>
#include <badesca.h>
#include <utf.h>

#include <rssh.rsg>
#include "rssh.hrh"
#include "rssh_ui.h"
#include "rssh_session.h"
#include "rssh_platform.h"
#include "rssh_trace.h"
#include "about_text.h"

const TUid KUidRssh = { TInt32(0xE5A1E010) };

class CTermView;
class CRsshAppUi;
static CTermView *gView;
static CRsshAppUi *gAppUi;

// ---------------------------------------------------------------------------
// helpers

static HBufC *Utf8ToUnicodeLC(const char *aUtf8)
{
    TPtrC8 in((const TUint8 *)aUtf8, aUtf8 ? User::StringLength((const TUint8 *)aUtf8) : 0);
    HBufC *out = HBufC::NewLC(in.Length() + 1);
    TPtr ptr = out->Des();
    if (CnvUtfConverter::ConvertToUnicodeFromUtf8(ptr, in) < 0)
        ptr.Copy(in);
    return out;
}

static TRgb ToRgb(TUint aRgb)       // 0xRRGGBB
{
    return TRgb((aRgb >> 16) & 0xFF, (aRgb >> 8) & 0xFF, aRgb & 0xFF);
}

// Short Yes/No confirmation query; returns ETrue for Yes.
static TBool AskYesNoL(const TDesC &aPrompt)
{
    rssh_trace("yesno: NewL");
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    rssh_trace("yesno: ExecuteLD");
    RsshPlatformSetModal(ETrue);
    TInt ret = 0;
    TRAPD(err, ret = dlg->ExecuteLD(R_RSSH_YESNO_QUERY, aPrompt));
    RsshPlatformSetModal(EFalse);
    rssh_trace("yesno: err=%d ret=%d", err, ret);
    User::LeaveIfError(err);
    return ret != 0;
}

// Information dialog with an OK button (connection closed etc.).
static void AskOkL(const TDesC &aText)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    RsshPlatformSetModal(ETrue);
    TRAPD(err, dlg->ExecuteLD(R_RSSH_OK_QUERY, aText));
    RsshPlatformSetModal(EFalse);
    rssh_trace("ok dialog err=%d", err);
    User::LeaveIfError(err);
}

// Non-blocking information / error notes.
static void ShowNoteL(const TDesC &aText, TBool aError)
{
    rssh_trace("note: %s", aError ? "error" : "info");
    if (aError) {
        CAknErrorNote *note = new (ELeave) CAknErrorNote(ETrue);
        RsshPlatformSetModal(ETrue);
        TRAPD(err, note->ExecuteLD(aText));
        RsshPlatformSetModal(EFalse);
        User::LeaveIfError(err);
    } else {
        CAknInformationNote *note = new (ELeave) CAknInformationNote(EFalse);
        note->ExecuteLD(aText);
    }
}

// ---------------------------------------------------------------------------
// terminal view: a grid of cells PuTTY draws into

class CTermView : public CCoeControl
{
public:
    static CTermView *NewL(const TRect &aRect)
    {
        CTermView *self = new (ELeave) CTermView;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CTermView()
    {
        FreeGrid();
        if (iOwnFont)
            iCoeEnv->ScreenDevice()->ReleaseFont(const_cast<CFont *>(iFont));
    }

    TInt Cols() const { return iCols; }
    void ArmStickyCtrl() { iStickyCtrl = ETrue; }
    TInt Rows() const { return iRows; }

    void PutCells(TInt aX, TInt aY, const TUint16 *aText, TInt aLen,
                  TUint aFg, TUint aBg, TInt aFlags)
    {
        if (aY < 0 || aY >= iRows)
            return;
        for (TInt i = 0; i < aLen && aX + i < iCols; i++) {
            if (aX + i < 0)
                continue;
            TInt n = aY * iCols + aX + i;
            iChars[n] = aText[i];
            iFg[n] = aFg;
            iBg[n] = aBg;
            iFlags[n] = (TUint8)aFlags;
        }
    }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
    {
        if (aType != EEventKey)
            return EKeyWasNotConsumed;
        if (iKeyEvents++ < 3)
            rssh_trace("key: event code 0x%x scan 0x%x mods 0x%x", aKeyEvent.iCode,
                       aKeyEvent.iScanCode, aKeyEvent.iModifiers);
        TUint code = aKeyEvent.iCode;
        TUint mods = aKeyEvent.iModifiers;
        int shift = (mods & EModifierShift) != 0;
        int ctrl = (mods & EModifierCtrl) != 0;
        int alt = (mods & EModifierAlt) != 0;
        int key = 0;

        switch (code) {
        case EKeyUpArrow:    key = RSSH_KEY_UP; break;
        case EKeyDownArrow:  key = RSSH_KEY_DOWN; break;
        case EKeyLeftArrow:  key = RSSH_KEY_LEFT; break;
        case EKeyRightArrow: key = RSSH_KEY_RIGHT; break;
        case EKeyHome:       key = RSSH_KEY_HOME; break;
        case EKeyEnd:        key = RSSH_KEY_END; break;
        case EKeyPageUp:     key = RSSH_KEY_PGUP; break;
        case EKeyPageDown:   key = RSSH_KEY_PGDN; break;
        case EKeyInsert:     key = RSSH_KEY_INSERT; break;
        case EKeyDelete:     key = RSSH_KEY_DELETE; break;
        case EKeyEnter:
        case EKeyDevice3:    key = RSSH_KEY_ENTER; break;     // selection key
        case EKeyBackspace:  key = RSSH_KEY_BACKSPACE; break;
        case EKeyTab:        key = RSSH_KEY_TAB; break;
        case EKeyEscape:     key = RSSH_KEY_ESCAPE; break;
        default:
            if (code >= EKeyF1 && code <= EKeyF12)
                key = RSSH_KEY_F1 + (code - EKeyF1);
            break;
        }
        if (key) {
            rssh_trace("key: special %d (code 0x%x scan 0x%x)", key, code,
                       aKeyEvent.iScanCode);
            rssh_session_send_key(key, shift, ctrl, alt);
            return EKeyWasConsumed;
        }

        if (iStickyCtrl) {                  // "Ctrl + next key" from the menu
            iStickyCtrl = EFalse;
            ctrl = 1;
        }
        if (ctrl && ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
                     || code == '[' || code == '\\' || code == ']'
                     || code == ' ')) {
            char c = (char)(code == ' ' ? 0 : (code & 0x1F));
            rssh_session_send_text(&c, 1);
            return EKeyWasConsumed;
        }
        if (code < 0x20) {
            rssh_trace("key: control 0x%x", code);
            char c = (char)code;
            rssh_session_send_text(&c, 1);
            return EKeyWasConsumed;
        }
        if (code < ENonCharacterKeyBase) {
            if (++iTypedCount <= 3 || iTypedCount % 10 == 0)
                rssh_trace("key: printable #%d", iTypedCount);   /* no text */
            TBuf16<1> in;
            in.Append(TChar(code));
            TBuf8<8> out;
            CnvUtfConverter::ConvertFromUnicodeToUtf8(out, in);
            rssh_session_send_text((const char *)out.Ptr(), out.Length());
            return EKeyWasConsumed;
        }
        return EKeyWasNotConsumed;
    }

private:
    CTermView() {}

    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        // A small font so ~80 columns fit in landscape. The system fonts
        // are proportional: cells are as wide as the widest lower-case letter
        // or digit (wider glyphs like 'W' may touch their neighbours), and
        // each glyph is centred in its cell.
        CFont *font = NULL;
        TFontSpec spec(KNullDesC, 13);
        if (iCoeEnv->ScreenDevice()->GetNearestFontToDesignHeightInPixels(font, spec) == KErrNone) {
            iFont = font;
            iOwnFont = ETrue;
        } else {
            iFont = iEikonEnv->DenseFont();
        }
        iCellW = 4;
        for (TUint c = 'a'; c <= 'z'; c++)
            if (iFont->CharWidthInPixels(c) > iCellW)
                iCellW = iFont->CharWidthInPixels(c);
        for (TUint c = '0'; c <= '9'; c++)
            if (iFont->CharWidthInPixels(c) > iCellW)
                iCellW = iFont->CharWidthInPixels(c);
        rssh_trace("font: cell %dx%d", iCellW, iFont->FontMaxHeight() + 1);
        iCellH = iFont->FontMaxHeight() + 1;
        iAscent = iFont->FontMaxAscent();
        SetRect(aRect);
        ActivateL();
    }

    void FreeGrid()
    {
        User::Free(iChars); iChars = NULL;
        User::Free(iFg); iFg = NULL;
        User::Free(iBg); iBg = NULL;
        User::Free(iFlags); iFlags = NULL;
    }

    void SizeChanged()
    {
        TInt cols = Rect().Width() / iCellW;
        TInt rows = Rect().Height() / iCellH;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (cols == iCols && rows == iRows && iChars)
            return;
        FreeGrid();
        iCols = cols;
        iRows = rows;
        TInt n = cols * rows;
        iChars = (TUint16 *)User::Alloc(n * sizeof(TUint16));
        iFg = (TUint *)User::Alloc(n * sizeof(TUint));
        iBg = (TUint *)User::Alloc(n * sizeof(TUint));
        iFlags = (TUint8 *)User::Alloc(n);
        if (!iChars || !iFg || !iBg || !iFlags) {
            FreeGrid();
            iCols = iRows = 0;
            return;
        }
        for (TInt i = 0; i < n; i++) {
            iChars[i] = ' ';
            iFg[i] = 0xBBBBBB;
            iBg[i] = 0x000000;
            iFlags[i] = 0;
        }
        rssh_session_resize(iCols, iRows);
        rssh_session_redraw();
    }

    void Draw(const TRect & /*aRect*/) const
    {
        CWindowGc &gc = SystemGc();
        gc.SetPenStyle(CGraphicsContext::ENullPen);
        gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        gc.SetBrushColor(KRgbBlack);
        gc.DrawRect(Rect());
        if (!iChars)
            return;

        TPoint origin = Rect().iTl;
        gc.UseFont(iFont);
        for (TInt y = 0; y < iRows; y++) {
            TInt top = origin.iY + y * iCellH;
            // backgrounds, in runs of equal colour
            TInt x = 0;
            while (x < iCols) {
                TInt n = y * iCols + x;
                TUint bg = iBg[n];
                TInt run = 1;
                while (x + run < iCols && iBg[n + run] == bg)
                    run++;
                if (bg != 0) {
                    gc.SetPenStyle(CGraphicsContext::ENullPen);
                    gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
                    gc.SetBrushColor(ToRgb(bg));
                    gc.DrawRect(TRect(TPoint(origin.iX + x * iCellW, top),
                                      TSize(run * iCellW, iCellH)));
                }
                x += run;
            }
            // glyphs, one cell at a time to keep a fixed grid with any font
            gc.SetBrushStyle(CGraphicsContext::ENullBrush);
            gc.SetPenStyle(CGraphicsContext::ESolidPen);
            for (x = 0; x < iCols; x++) {
                TInt n = y * iCols + x;
                TUint16 ch = iChars[n];
                TInt left = origin.iX + x * iCellW;
                gc.SetPenColor(ToRgb(iFg[n]));
                if (ch != ' ' && ch != 0) {
                    TPtrC glyph(&ch, 1);
                    TInt gx = left + (iCellW - iFont->CharWidthInPixels(ch)) / 2;
                    if (gx < left)
                        gx = left;
                    gc.DrawText(glyph, TPoint(gx, top + iAscent));
                    if (iFlags[n] & RSSH_DRAW_BOLD)
                        gc.DrawText(glyph, TPoint(gx + 1, top + iAscent));
                }
                if (iFlags[n] & RSSH_DRAW_UNDERLINE)
                    gc.DrawLine(TPoint(left, top + iAscent + 1),
                                TPoint(left + iCellW, top + iAscent + 1));
                if (iFlags[n] & RSSH_DRAW_PASSIVE_CURSOR)
                    gc.DrawRect(TRect(TPoint(left, top), TSize(iCellW, iCellH)));
            }
        }
        gc.DiscardFont();
    }

    const CFont *iFont;
    TInt iCellW, iCellH, iAscent;
    TInt iKeyEvents, iTypedCount;
    TBool iOwnFont;
    TBool iStickyCtrl;
    TInt iCols, iRows;
    TUint16 *iChars;
    TUint *iFg, *iBg;
    TUint8 *iFlags;
};

// ---------------------------------------------------------------------------
// About page: word-wrapped, scrollable text (arrows, paging, or drag)

class CAboutView : public CCoeControl
{
public:
    static CAboutView *NewL(const TRect &aRect)
    {
        CAboutView *self = new (ELeave) CAboutView;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CAboutView()
    {
        iLines.ResetAndDestroy();
        delete iText;
    }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
    {
        if (aType != EEventKey)
            return EKeyWasNotConsumed;
        switch (aKeyEvent.iCode) {
        case EKeyUpArrow:   ScrollBy(-1); return EKeyWasConsumed;
        case EKeyDownArrow: ScrollBy(1); return EKeyWasConsumed;
        case EKeyPageUp:    ScrollBy(-VisibleLines()); return EKeyWasConsumed;
        case EKeyPageDown:
        case ' ':           ScrollBy(VisibleLines()); return EKeyWasConsumed;
        default:            return EKeyWasNotConsumed;
        }
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
    CAboutView() {}

    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        EnableDragEvents();
        iFont = iEikonEnv->DenseFont();
        iLineH = iFont->FontMaxHeight() + 3;
        TPtrC8 utf8((const TUint8 *)KAboutTextUtf8);
        iText = HBufC::NewL(utf8.Length());
        TPtr ptr = iText->Des();
        CnvUtfConverter::ConvertToUnicodeFromUtf8(ptr, utf8);
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
        if (maxTop < 0)
            maxTop = 0;
        TInt top = iTop + aLines;
        if (top < 0) top = 0;
        if (top > maxTop) top = maxTop;
        if (top != iTop) {
            iTop = top;
            DrawDeferred();
        }
    }

    // Break the text into lines that fit the width (on spaces where possible).
    void WrapL()
    {
        iLines.ResetAndDestroy();
        TInt width = Rect().Width() - 2 * KMargin;
        if (width < 20)
            return;
        TPtrC rest(*iText);
        while (rest.Length()) {
            TInt nl = rest.Locate('\n');
            TPtrC para = nl >= 0 ? rest.Left(nl) : rest;
            rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
            if (!para.Length()) {
                iLines.AppendL(KNullDesC().AllocL());
                continue;
            }
            while (para.Length()) {
                TInt fit = iFont->TextCount(para, width);
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
            }
        }
    }

    void SizeChanged()
    {
        TRAP_IGNORE(WrapL());
        iTop = 0;
        ScrollBy(0);
    }

    void Draw(const TRect & /*aRect*/) const
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
        // simple scroll position indicator on the right edge
        if (iLines.Count() > VisibleLines()) {
            TInt h = Rect().Height();
            TInt barH = h * VisibleLines() / iLines.Count();
            TInt barY = Rect().iTl.iY + h * iTop / iLines.Count();
            gc.SetPenStyle(CGraphicsContext::ENullPen);
            gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
            gc.SetBrushColor(KRgbGray);
            gc.DrawRect(TRect(TPoint(Rect().iBr.iX - 4, barY),
                              TSize(4, barH > 4 ? barH : 4)));
        }
    }

    static const TInt KMargin = 6;
    const CFont *iFont;
    TInt iLineH;
    HBufC *iText;
    RPointerArray<HBufC> iLines;
    TInt iTop;
    TInt iDragY;
};

// ---------------------------------------------------------------------------
// connections list (start screen)

class CProfileList : public CCoeControl, public MEikListBoxObserver
{
public:
    static CProfileList *NewL(const TRect &aRect, CRsshAppUi &aAppUi)
    {
        CProfileList *self = new (ELeave) CProfileList(aAppUi);
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CProfileList()
    {
        delete iListBox;
        delete iItems;
    }

    // Row 0 is "Quick connect"; rows 1.. are profiles, in order added.
    void ResetL()
    {
        iItems->Reset();
        iItems->AppendL(_L("\tQuick connect\tconnect without saving"));
    }
    void AddRowL(const TDesC &aTitle, const TDesC &aSubtitle)
    {
        HBufC *row = HBufC::NewLC(aTitle.Length() + aSubtitle.Length() + 2);
        TPtr p = row->Des();
        p.Append('\t');
        p.Append(aTitle);
        p.Append('\t');
        p.Append(aSubtitle);
        iItems->AppendL(*row);
        CleanupStack::PopAndDestroy(row);
    }
    // After rebuilding the items (rows may have been added or removed):
    // reset the view and keep the selection in range, so the list box never
    // points past the end (deleting the last row used to panic here).
    void DoneL()
    {
        TInt current = iListBox->CurrentItemIndex();
        iListBox->Reset();
        if (current < 0)
            current = 0;
        if (current >= iItems->Count())
            current = iItems->Count() - 1;
        iListBox->SetCurrentItemIndex(current);
        iListBox->DrawDeferred();
    }
    TInt CurrentIndex() const { return iListBox->CurrentItemIndex(); }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
    {
        return iListBox->OfferKeyEventL(aKeyEvent, aType);
    }

    void HandleListBoxEventL(CEikListBox *aListBox, TListBoxEvent aEvent);

private:
    CProfileList(CRsshAppUi &aAppUi) : iAppUi(aAppUi) {}
    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        iListBox = new (ELeave) CAknDoubleStyleListBox;
        rssh_trace("list: listbox ConstructL");
        iListBox->ConstructL(this, EAknListBoxSelectionList);
        rssh_trace("list: listbox ok");
        iListBox->CreateScrollBarFrameL(ETrue);
        iListBox->ScrollBarFrame()->SetScrollBarVisibilityL(
            CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
        iListBox->SetListBoxObserver(this);
        iItems = new (ELeave) CDesCArrayFlat(4);
        iListBox->Model()->SetItemTextArray(iItems);
        iListBox->Model()->SetOwnershipType(ELbmDoesNotOwnItemArray);
        ResetL();
        SetRect(aRect);
        ActivateL();
    }
    void SizeChanged() { iListBox->SetRect(Rect()); }
    TInt CountComponentControls() const { return 1; }
    CCoeControl *ComponentControl(TInt) const { return iListBox; }

    CRsshAppUi &iAppUi;
    CAknDoubleStyleListBox *iListBox;
    CDesCArrayFlat *iItems;
};

// ---------------------------------------------------------------------------
// app UI

// "user@host[:port]" -> parts. EFalse if there is no host.
static TBool ParseTarget(const TDesC &aTarget, TDes8 &aHost, TDes8 &aUser, TInt &aPort)
{
    TPtrC rest(aTarget);
    TPtrC user(KNullDesC);
    TInt at = rest.LocateReverse('@');
    if (at >= 0) {
        user.Set(rest.Left(at));
        rest.Set(rest.Mid(at + 1));
    }
    aPort = 22;
    TInt colon = rest.LocateReverse(':');
    if (colon >= 0) {
        TLex lex(rest.Mid(colon + 1));
        if (lex.Val(aPort) != KErrNone || aPort <= 0 || aPort > 65535)
            aPort = 22;
        rest.Set(rest.Left(colon));
    }
    if (rest.Length() == 0)
        return EFalse;
    CnvUtfConverter::ConvertFromUnicodeToUtf8(aHost, rest);
    CnvUtfConverter::ConvertFromUnicodeToUtf8(aUser, user);
    return ETrue;
}

class CRsshAppUi : public CAknAppUi
{
public:
    void ConstructL()
    {
        BaseConstructL(EAknEnableSkin | EAppOrientationLandscape);
        RsshPlatformInitL();
        gAppUi = this;
        iView = CTermView::NewL(ClientRect());
        gView = iView;
        iView->MakeVisible(EFalse);
        rssh_trace("list: NewL");
        iList = CProfileList::NewL(ClientRect(), *this);
        rssh_trace("list: NewL ok");
        iAbout = CAboutView::NewL(ClientRect());
        iAbout->MakeVisible(EFalse);
        iTarget.Copy(_L("user@host"));
        ReadDefaultTarget();
        iNotes = new (ELeave) CAsyncCallBack(TCallBack(ShowNotes, this),
                                             CActive::EPriorityStandard);
        iAsk = new (ELeave) CAsyncCallBack(TCallBack(AskQuestions, this),
                                           CActive::EPriorityStandard);
        iEnded = new (ELeave) CAsyncCallBack(TCallBack(SessionEnded, this),
                                             CActive::EPriorityStandard);
        AddToStackL(iList);
        iMode = EList;
        RefreshProfilesL();
        rssh_trace("appui: constructed");
    }

    ~CRsshAppUi()
    {
        rssh_session_close();
        delete iNotes;
        delete iAsk;
        delete iEnded;
        delete iEndedMsg;
        iPending.ResetAndDestroy();
        for (TInt i = 0; i < iQuestions.Count(); i++) {
            delete iQuestions[i].iTitle;
            delete iQuestions[i].iText;
        }
        iQuestions.Close();
        iProfileNames.ResetAndDestroy();
        iProfileTargets.ResetAndDestroy();
        if (iView) {
            if (iMode == ETerminal)
                RemoveFromStack(iView);
            delete iView;
        }
        if (iList) {
            if (iMode == EList)
                RemoveFromStack(iList);
            delete iList;
        }
        if (iAbout) {
            if (iMode == EAbout)
                RemoveFromStack(iAbout);
            delete iAbout;
        }
        gView = NULL;
        gAppUi = NULL;
        RsshPlatformShutdown();
    }

    void SetTitleL(const TDesC &aTitle)
    {
        CAknTitlePane *title = static_cast<CAknTitlePane *>(
            StatusPane()->ControlL(TUid::Uid(EEikStatusPaneUidTitle)));
        title->SetTextL(aTitle);
    }

    // Queue a Yes/No question; the answer goes to aAnswer(aCtx, yes).
    void QueueQuestionL(const TDesC &aTitle, const TDesC &aText,
                        rssh_ui_answer_fn aAnswer, void *aCtx)
    {
        TQuestion q;
        q.iTitle = aTitle.AllocLC();
        q.iText = aText.AllocLC();
        q.iAnswer = aAnswer;
        q.iCtx = aCtx;
        iQuestions.AppendL(q);
        CleanupStack::Pop(2);
        iAsk->CallBack();
    }

    // Queue a note to show from the active scheduler, outside PuTTY calls.
    void QueueNoteL(const TDesC &aText)
    {
        HBufC *copy = aText.AllocLC();
        iPending.AppendL(copy);
        CleanupStack::Pop(copy);
        iNotes->CallBack();
    }

    // The connection has ended: tell the user (OK), then back to the list.
    void QueueSessionEndedL(const TDesC &aText)
    {
        if (iEndedMsg)
            return;                       /* already on its way */
        iEndedMsg = aText.AllocL();
        iEnded->CallBack();
    }

    // From the list: row 0 = quick connect, others = profiles.
    void OpenItemL(TInt aIndex)
    {
        if (aIndex == 0) {
            QuickConnectL();
        } else if (aIndex > 0 && aIndex <= iProfileNames.Count()) {
            HBufC8 *name = iProfileNames[aIndex - 1];
            HBufC *title = Utf8ToUnicodeLC((const char *)name->Des().PtrZ());
            ShowTerminalL(*title);
            CleanupStack::PopAndDestroy(title);
            rssh_session_start_profile((const char *)name->Des().PtrZ(),
                                       iView->Cols(), iView->Rows());
        }
    }

private:
    enum TMode { EList, ETerminal, EAbout };

    void AddProfileL(const char *aName, const char *aHost, int aPort, const char *aUser)
    {
        // +1 so PtrZ() has room for the terminator (it panics otherwise).
        TPtrC8 nameIn((const TUint8 *)aName);
        HBufC8 *name8 = HBufC8::NewLC(nameIn.Length() + 1);
        name8->Des().Copy(nameIn);
        HBufC *name = Utf8ToUnicodeLC(aName);
        HBufC *host = Utf8ToUnicodeLC(aHost);
        HBufC *user = Utf8ToUnicodeLC(aUser);
        HBufC *target = HBufC::NewLC(user->Length() + host->Length() + 12);
        TPtr t = target->Des();
        if (user->Length()) {
            t.Append(*user);
            t.Append('@');
        }
        t.Append(*host);
        if (aPort != 22) {
            t.Append(':');
            t.AppendNum(aPort);
        }
        iList->AddRowL(*name, *target);
        iProfileTargets.AppendL(target);
        CleanupStack::Pop(target);
        CleanupStack::PopAndDestroy(3, name);    /* user, host, name */
        iProfileNames.AppendL(name8);
        CleanupStack::Pop(name8);
    }

    void RefreshProfilesL()
    {
        iProfileNames.ResetAndDestroy();
        iProfileTargets.ResetAndDestroy();
        iList->ResetL();
        rssh_profile *list = NULL;
        TInt n = rssh_profiles_list(&list);
        TRAPD(err,
            for (TInt i = 0; i < n; i++)
                AddProfileL(list[i].name, list[i].host, list[i].port, list[i].user);
        );
        rssh_profiles_free(list, n);
        rssh_trace("profiles: %d listed, err=%d", n, err);
        User::LeaveIfError(err);
        iList->DoneL();
        rssh_trace("profiles: list updated");
    }

    CCoeControl *ModeControl(TMode aMode)
    {
        return aMode == ETerminal ? (CCoeControl *)iView :
               aMode == EAbout ? (CCoeControl *)iAbout : (CCoeControl *)iList;
    }

    void SwitchToL(TMode aMode)
    {
        if (iMode == aMode)
            return;
        CCoeControl *from = ModeControl(iMode), *to = ModeControl(aMode);
        RemoveFromStack(from);
        from->MakeVisible(EFalse);
        to->MakeVisible(ETrue);
        AddToStackL(to);
        iMode = aMode;
        to->DrawDeferred();
    }

    void ShowListL()
    {
        SwitchToL(EList);
        SetTitleL(_L("rSSH"));
        RefreshProfilesL();
    }

    void ShowAboutL()
    {
        SwitchToL(EAbout);
        SetTitleL(_L("About rSSH"));
    }

    void ShowTerminalL(const TDesC &aTitle)
    {
        SwitchToL(ETerminal);
        SetTitleL(aTitle);
    }

    // Default for the quick-connect prompt: first line of
    // C:\data\rssh-target.txt (handy where typing is awkward, e.g. the emulator).
    void ReadDefaultTarget()
    {
        RFile file;
        if (file.Open(iEikonEnv->FsSession(), _L("C:\\data\\rssh-target.txt"),
                      EFileRead | EFileShareReadersOnly) != KErrNone)
            return;
        TBuf8<128> line;
        if (file.Read(line) == KErrNone) {
            TInt end = 0;
            while (end < line.Length() && line[end] != '\r' && line[end] != '\n')
                end++;
            if (end > 0)
                iTarget.Copy(line.Left(end));
        }
        file.Close();
        rssh_trace("default target from file");
    }

    static TBool QueryTextL(const TDesC &aPrompt, TDes &aText)
    {
        CAknTextQueryDialog *dlg = CAknTextQueryDialog::NewL(aText);
        dlg->SetPromptL(aPrompt);
        return dlg->ExecuteLD(R_RSSH_TEXT_QUERY) != 0;
    }

    void QuickConnectL()
    {
        TBuf<128> target(iTarget);
        if (!QueryTextL(_L("Connect to user@host[:port]"), target))
            return;
        iTarget = target;
        TBuf8<128> host8, user8;
        TInt port;
        if (!ParseTarget(target, host8, user8, port))
            return;
        TPtrC title(target);
        ShowTerminalL(title);
        rssh_trace("quick connect");
        rssh_session_start((const char *)host8.PtrZ(), port,
                           (const char *)user8.PtrZ(),
                           iView->Cols(), iView->Rows());
    }

    // Add (aIndex < 1) or edit profile row aIndex.
    void EditProfileL(TInt aIndex)
    {
        TBool editing = aIndex >= 1 && aIndex <= iProfileNames.Count();
        TBuf<64> name;
        TBuf<128> target(_L("user@host"));
        if (editing) {
            HBufC *n = Utf8ToUnicodeLC((const char *)iProfileNames[aIndex - 1]->Des().PtrZ());
            name.Copy(n->Left(64));
            CleanupStack::PopAndDestroy(n);
            target.Copy(iProfileTargets[aIndex - 1]->Left(128));
        }
        if (!QueryTextL(_L("Connection name"), name) || name.Length() == 0)
            return;
        if (!QueryTextL(_L("user@host[:port]"), target))
            return;
        TBuf8<128> host8, user8, name8;
        TInt port;
        if (!ParseTarget(target, host8, user8, port))
            return;
        CnvUtfConverter::ConvertFromUnicodeToUtf8(name8, name);
        if (rssh_profile_save((const char *)name8.PtrZ(), (const char *)host8.PtrZ(),
                              port, (const char *)user8.PtrZ()) != 0)
            return;
        if (editing && iProfileNames[aIndex - 1]->Compare(name8) != 0)
            rssh_profile_delete((const char *)iProfileNames[aIndex - 1]->Des().PtrZ());
        RefreshProfilesL();
    }

    void DeleteProfileL(TInt aIndex)
    {
        if (aIndex < 1 || aIndex > iProfileNames.Count())
            return;
        HBufC *n = Utf8ToUnicodeLC((const char *)iProfileNames[aIndex - 1]->Des().PtrZ());
        HBufC *prompt = HBufC::NewLC(n->Length() + 16);
        prompt->Des().Format(_L("Delete \"%S\"?"), n);
        TBool yes = AskYesNoL(*prompt);
        CleanupStack::PopAndDestroy(2, n);
        rssh_trace("delete profile: answer %d", yes);
        if (!yes)
            return;
        rssh_profile_delete((const char *)iProfileNames[aIndex - 1]->Des().PtrZ());
        rssh_trace("delete profile: deleted");
        RefreshProfilesL();
    }

    static TInt AskQuestions(TAny *aSelf)
    {
        CRsshAppUi *self = static_cast<CRsshAppUi *>(aSelf);
        while (self->iQuestions.Count()) {
            TQuestion q = self->iQuestions[0];
            self->iQuestions.Remove(0);
            TBool yes = EFalse;
            TRAPD(err, yes = AskYesNoL(*q.iText));
            rssh_trace("question dialog err=%d yes=%d", err, yes);
            delete q.iTitle;
            delete q.iText;
            q.iAnswer(q.iCtx, yes ? 1 : 0);   /* always answer, even on error */
        }
        return 0;
    }

    static TInt ShowNotes(TAny *aSelf)
    {
        CRsshAppUi *self = static_cast<CRsshAppUi *>(aSelf);
        while (self->iPending.Count()) {
            HBufC *text = self->iPending[0];
            self->iPending.Remove(0);
            TRAP_IGNORE(ShowNoteL(*text, EFalse));
            delete text;
        }
        return 0;
    }

    static TInt SessionEnded(TAny *aSelf)
    {
        CRsshAppUi *self = static_cast<CRsshAppUi *>(aSelf);
        HBufC *msg = self->iEndedMsg;
        self->iEndedMsg = NULL;
        if (msg) {
            TRAP_IGNORE(AskOkL(*msg));
            delete msg;
        }
        rssh_session_close();
        TRAP_IGNORE(self->ShowListL());
        return 0;
    }

    // Right softkey: leave the terminal (disconnecting if still connected)
    // or, from the list, exit - asking first either way.
    void BackL()
    {
        if (iMode == EAbout) {
            ShowListL();
        } else if (iMode == ETerminal) {
            if (rssh_session_active() &&
                !AskYesNoL(_L("Disconnect from this server?")))
                return;
            rssh_session_close();
            ShowListL();
        } else if (AskYesNoL(_L("Exit rSSH?"))) {
            Exit();
        }
    }

    void SendCtrl(char aChar)
    {
        char c = aChar & 0x1F;
        rssh_session_send_text(&c, 1);
    }

    void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenuPane)
    {
        if (aResourceId != R_RSSH_MENU)
            return;
        TBool list = iMode == EList;
        TBool term = iMode == ETerminal;
        TBool onProfile = list && iList->CurrentIndex() >= 1;
        aMenuPane->SetItemDimmed(ERsshCmdAbout, !list);
        aMenuPane->SetItemDimmed(ERsshCmdConnect, !list);
        aMenuPane->SetItemDimmed(ERsshCmdAddProfile, !list);
        aMenuPane->SetItemDimmed(ERsshCmdEditProfile, !onProfile);
        aMenuPane->SetItemDimmed(ERsshCmdDeleteProfile, !onProfile);
        aMenuPane->SetItemDimmed(ERsshCmdDisconnect, !term);
        aMenuPane->SetItemDimmed(ERsshCmdStickyCtrl, !term);
        aMenuPane->SetItemDimmed(ERsshCmdSendEsc, !term);
        aMenuPane->SetItemDimmed(ERsshCmdSendTab, !term);
        aMenuPane->SetItemDimmed(ERsshCmdSendCtrlC, !term);
        aMenuPane->SetItemDimmed(ERsshCmdSendCtrlD, !term);
    }

    void HandleCommandL(TInt aCommand)
    {
        switch (aCommand) {
        case ERsshCmdConnect:       QuickConnectL(); break;
        case ERsshCmdAbout:         ShowAboutL(); break;
        case ERsshCmdAddProfile:    EditProfileL(0); break;
        case ERsshCmdEditProfile:   EditProfileL(iList->CurrentIndex()); break;
        case ERsshCmdDeleteProfile: DeleteProfileL(iList->CurrentIndex()); break;
        case ERsshCmdDisconnect:    rssh_session_close(); ShowListL(); break;
        case ERsshCmdStickyCtrl: iView->ArmStickyCtrl(); break;
        case ERsshCmdSendEsc:    rssh_session_send_key(RSSH_KEY_ESCAPE, 0, 0, 0); break;
        case ERsshCmdSendTab:    rssh_session_send_key(RSSH_KEY_TAB, 0, 0, 0); break;
        case ERsshCmdSendCtrlC:  SendCtrl('c'); break;
        case ERsshCmdSendCtrlD:  SendCtrl('d'); break;
        case EAknSoftkeyBack:
            BackL();
            break;
        case EAknSoftkeyExit:
        case EAknCmdExit:             // Options -> Exit
            if (!rssh_session_active() || AskYesNoL(_L("Disconnect and exit?")))
                Exit();
            break;
        case EEikCmdExit:             // from the system: never ask
            Exit();
            break;
        default:
            break;
        }
    }

    // EKA2L1 turns letter scan codes straight into upper-case character
    // codes without applying Shift/Caps state. Fix the case for every control
    // (terminal, queries, editors) before the event is dispatched. On a real
    // phone letters already arrive lower-case, so this changes nothing there.
    // EKA2L1 also never sets modifier flags, but it does send key-down/up
    // events for the Shift and Ctrl keys, so track those ourselves and add
    // the flags. (On a phone the flags are already set; OR-ing is harmless.)
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
        if (aType == KEikDynamicLayoutVariantSwitch) {   // rotation / slide
            if (iView)
                iView->SetRect(ClientRect());
            if (iList)
                iList->SetRect(ClientRect());
            if (iAbout)
                iAbout->SetRect(ClientRect());
        }
    }

    TMode iMode;
    TBool iShiftDown, iCtrlDown;
    CTermView *iView;
    CProfileList *iList;
    CAboutView *iAbout;
    CAsyncCallBack *iNotes;
    CAsyncCallBack *iAsk;
    CAsyncCallBack *iEnded;
    HBufC *iEndedMsg;
    RPointerArray<HBufC> iPending;
    struct TQuestion {
        HBufC *iTitle;
        HBufC *iText;
        rssh_ui_answer_fn iAnswer;
        void *iCtx;
    };
    RArray<TQuestion> iQuestions;
    RPointerArray<HBufC8> iProfileNames;     // UTF-8, as PuTTY stores them
    RPointerArray<HBufC> iProfileTargets;    // "user@host[:port]"
    TBuf<128> iTarget;
};

void CProfileList::HandleListBoxEventL(CEikListBox *aListBox, TListBoxEvent aEvent)
{
    if (aEvent == EEventEnterKeyPressed || aEvent == EEventItemSingleClicked ||
        aEvent == EEventItemDoubleClicked)
        iAppUi.OpenItemL(iListBox->CurrentItemIndex());
}

// ---------------------------------------------------------------------------
// rssh_ui.h

extern "C" void rssh_ui_draw(int x, int y, const unsigned short *text, int len,
                             unsigned int fg, unsigned int bg, int flags)
{
    if (gView)
        gView->PutCells(x, y, text, len, fg, bg, flags);
}

extern "C" void rssh_ui_flush(void)
{
    if (gView)
        gView->DrawDeferred();
}

static void SetTitleUtf8L(const char *aTitle)
{
    HBufC *t = Utf8ToUnicodeLC(aTitle);
    gAppUi->SetTitleL(*t);
    CleanupStack::PopAndDestroy(t);
}

static void QueueNoteUtf8L(const char *aMsg)
{
    HBufC *t = Utf8ToUnicodeLC(aMsg);
    gAppUi->QueueNoteL(*t);
    CleanupStack::PopAndDestroy(t);
}

static void QueueQuestionUtf8L(const char *aTitle, const char *aText,
                               rssh_ui_answer_fn aAnswer, void *aCtx)
{
    HBufC *t = Utf8ToUnicodeLC(aTitle);
    HBufC *m = Utf8ToUnicodeLC(aText);
    gAppUi->QueueQuestionL(*t, *m, aAnswer, aCtx);
    CleanupStack::PopAndDestroy(2, t);
}

static void FatalUtf8L(const char *aMsg)
{
    HBufC *m = Utf8ToUnicodeLC(aMsg);
    ShowNoteL(*m, ETrue);           /* waiting error note */
    CleanupStack::PopAndDestroy(m);
}

extern "C" void rssh_ui_set_title(const char *title)
{
    if (gAppUi)
        TRAP_IGNORE(SetTitleUtf8L(title));
}

extern "C" void rssh_ui_message(const char *msg)
{
    if (gAppUi)
        TRAP_IGNORE(QueueNoteUtf8L(msg));
}

static void QueueSessionEndedUtf8L(const char *aMsg)
{
    HBufC *t = Utf8ToUnicodeLC(aMsg);
    gAppUi->QueueSessionEndedL(*t);
    CleanupStack::PopAndDestroy(t);
}

extern "C" void rssh_ui_session_ended(const char *msg)
{
    rssh_trace("session ended: %s", msg);
    if (gAppUi)
        TRAP_IGNORE(QueueSessionEndedUtf8L(msg));
}

extern "C" void rssh_ui_confirm_async(const char *title, const char *text,
                                      rssh_ui_answer_fn answer, void *ctx)
{
    TInt err = KErrNotReady;
    if (gAppUi)
        TRAP(err, QueueQuestionUtf8L(title, text, answer, ctx));
    if (err != KErrNone)
        answer(ctx, 0);              /* could not ask: treat as No */
}

extern "C" void rssh_ui_fatal(const char *msg)
{
    TRAP_IGNORE(FatalUtf8L(msg));
    User::Exit(KErrGeneral);
    for (;;) {}
}

// ---------------------------------------------------------------------------
// application / document

class CRsshDocument : public CAknDocument
{
public:
    CRsshDocument(CEikApplication &aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi *CreateAppUiL() { return new (ELeave) CRsshAppUi; }
};

class CRsshApplication : public CAknApplication
{
private:
    TUid AppDllUid() const { return KUidRssh; }
    CApaDocument *CreateDocumentL() { return new (ELeave) CRsshDocument(*this); }
};

LOCAL_C CApaApplication *NewApplication() { return new CRsshApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
