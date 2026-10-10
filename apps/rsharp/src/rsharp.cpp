// rSharp: a C# scratchpad for Symbian (S60 3rd Edition FP1 and Symbian^3),
// in the spirit of LINQPad and RoslynPad: type C# expressions, statements
// and LINQ queries, run them, see the result.
//
// - Two screens: the code (a multi-line editor) and its output (read-only).
//   Run (right softkey, or the navigation key's centre) runs the code; Back
//   returns to it.
// - Errors: the line is shaded red, the cursor put on it, and the message
//   shown in a strip under the code (above the softkeys) until the code is
//   edited. Compile errors stay on the code screen; exceptions show the
//   output first.
// - The interpreter (../engine, portable C++) runs on a worker thread, so
//   the UI stays responsive; Stop asks it to stop.
// - The code is saved in the app's private folder when it runs and on exit.

#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <akntitle.h>
#include <aknlistquerydialog.h>
#include <aknquerydialog.h>
#include <aknutils.h>
#include <avkon.hrh>
#include <avkon.rsg>
#include <badesca.h>
#include <eikbtgpc.h>
#include <eikedwin.h>
#include <eikenv.h>
#include <eikmenup.h>
#include <eikspane.h>
#include <eikstart.h>
#include <coecntrl.h>
#include <f32file.h>
#include <frmtlay.h>
#include <frmtview.h>
#include <gdi.h>
#include <txtfrmat.h>
#include <txtglobl.h>
#include <txtetext.h>
#include <utf.h>

#include <rsharp.rsg>
#include "rsharp.hrh"
#include "rs.h"

const TUid KUidRsharp = { TInt32(0xE5A1E060) };
const TInt KMaxCode = 200000;               // characters
const TInt KWorkerStack = 0x40000;          // 256 KB: deep recursion in C#
const TInt KStackGuard = 0x8000;            // kept free below the interpreter

_LIT(KAutotest, "C:\\Data\\rsharp-autotest.txt");
_LIT(KAutotestOut, "C:\\Data\\rsharp-autotest-out.txt");

class CRsAppUi;
static CRsAppUi *gAppUi;

// ---------------------------------------------------------------------------
// examples (Options > Examples)

struct TExample
    {
    const char *iName;
    const char *iCode;
    };

static const TExample KExamples[] =
    {
    { "Numbers and types",
      "// The last line without ';' is the result.\n"
      "var a = 7 / 2;          // int division\n"
      "var b = 7 / 2.0;        // double\n"
      "Console.WriteLine($\"{a} {b} {7 % 3}\");\n"
      "Console.WriteLine(0.1 + 0.2);\n"
      "Console.WriteLine(int.MaxValue + a);   // wraps around\n"
      "Math.Sqrt(2) * Math.PI\n" },
    { "Bits",
      "uint x = 0b1011_0110;\n"
      "Console.WriteLine(x.ToString(\"X8\"));\n"
      "Console.WriteLine(Convert.ToString(x, 2));\n"
      "Console.WriteLine(BitOperations.PopCount(x));\n"
      "Console.WriteLine(x << 4 | x >> 28);\n"
      "~x & 0xFF\n" },
    { "LINQ methods",
      "Enumerable.Range(1, 20)\n"
      "    .Where(n => n % 3 == 0)\n"
      "    .Select(n => new { n, Square = n * n })\n" },
    { "Query syntax",
      "var words = new[] { \"apple\", \"banana\", \"avocado\", \"cherry\", \"blueberry\" };\n"
      "from w in words\n"
      "group w by w[0] into g\n"
      "orderby g.Key\n"
      "select new { Letter = g.Key, Count = g.Count(), Words = string.Join(\", \", g) }\n" },
    { "Primes",
      "bool IsPrime(int n) =>\n"
      "    n > 1 && Enumerable.Range(2, (int)Math.Sqrt(n) - 1).All(d => n % d != 0);\n"
      "Enumerable.Range(1, 100).Where(IsPrime)\n" },
    { "Fibonacci",
      "long Fib(int n)\n"
      "{\n"
      "    long a = 0, b = 1;\n"
      "    for (int i = 0; i < n; i++) (a, b) = (b, a + b);\n"
      "    return a;\n"
      "}\n"
      "Enumerable.Range(0, 15).Select(Fib)\n" },
    { "Strings",
      "var s = \"The quick brown fox jumps over the lazy dog\";\n"
      "var counts = s.ToLower().Where(char.IsLetter)\n"
      "    .GroupBy(c => c)\n"
      "    .OrderByDescending(g => g.Count())\n"
      "    .Take(5)\n"
      "    .Select(g => $\"{g.Key}: {g.Count()}\");\n"
      "string.Join(\"\\n\", counts)\n" },
    { "Collections",
      "var stock = new Dictionary<string, int> { [\"apples\"] = 3 };\n"
      "stock[\"pears\"] = 5;\n"
      "stock[\"apples\"] += 10;\n"
      "foreach (var (name, n) in stock.Select(kv => (kv.Key, kv.Value)))\n"
      "    Console.WriteLine($\"{name,-8}{n,4}\");\n" },
    { "Switch and patterns",
      "string Describe(object o) => o switch\n"
      "{\n"
      "    null => \"nothing\",\n"
      "    int n when n < 0 => \"a negative int\",\n"
      "    int n => $\"the int {n}\",\n"
      "    string s => $\"a string of {s.Length}\",\n"
      "    _ => \"something else\"\n"
      "};\n"
      "new object[] { 42, -1, \"hi\", 2.5, null }.Select(Describe)\n" },
    };
const TInt KExampleCount = sizeof(KExamples) / sizeof(KExamples[0]);

static const char KHelp[] =
    "rSharp runs a C# subset on the phone.\n"
    "\n"
    "The last line without ';' is the result, shown as LINQPad would. "
    "Console.WriteLine and x.Dump() write to the output too.\n"
    "\n"
    "Works: int, uint, long, ulong, short, byte, char, bool, float, double, "
    "string; C#'s numeric rules (wrap-around, checked, integer division, "
    "shifts); var; if, for, foreach, while, do, switch (with patterns), "
    "break, continue, return; local functions; lambdas and closures; "
    "Func<...>; tuples; anonymous objects; arrays, List, Dictionary, "
    "HashSet, StringBuilder; string methods; $\"...\" with formats; "
    "Math, Convert, BitOperations, int.Parse, ...; LINQ methods (lazy, as "
    "in .NET) and query syntax (from, where, let, join, orderby, group, "
    "into); switch expressions; is patterns; ^1 and a..b.\n"
    "\n"
    "Not supported: classes, structs, interfaces, try/catch, decimal, "
    "async, generics of your own, reflection, files and the network.\n"
    "\n"
    "Keys: Run is the right softkey (or the navigation key's centre). On the "
    "output, up/down scroll; Back returns to the code.\n";

// ---------------------------------------------------------------------------
// helpers

static TBool AskYesNoL(const TDesC &aPrompt)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    return dlg->ExecuteLD(R_RS_YESNO_QUERY, aPrompt) != 0;
}

static void InfoL(const TDesC &aText)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    dlg->ExecuteLD(R_RS_OK_QUERY, aText);
}

static HBufC *FromUtf8LC(const char *aText)
{
    TPtrC8 in((const TUint8 *)aText, User::StringLength((const TUint8 *)aText));
    HBufC *out = HBufC::NewLC(in.Length() + 1);
    TPtr p = out->Des();
    CnvUtfConverter::ConvertToUnicodeFromUtf8(p, in);
    return out;
}

// Edwins keep paragraphs apart with U+2029; the interpreter wants '\n'.
static void ToEdwinNewlines(TDes &aText)
{
    for (TInt i = 0; i < aText.Length(); i++)
        if (aText[i] == '\n')
            aText[i] = CEditableText::EParagraphDelimiter;
}

static void FromEdwinNewlines(TDes &aText)
{
    for (TInt i = 0; i < aText.Length(); i++)
        if (aText[i] == CEditableText::EParagraphDelimiter || aText[i] == CEditableText::ELineBreak)
            aText[i] = '\n';
}

// ---------------------------------------------------------------------------
// the interpreter on a worker thread

class MRunObserver
{
public:
    // aErrorLen: the error message is the last aErrorLen characters of aOutput
    virtual void RunDoneL(TInt aStatus, const TDesC &aOutput, TInt aErrorPos, TInt aErrorLen) = 0;
};

static void *RsAlloc(size_t aSize) { return User::Alloc(aSize); }
static void RsRelease(void *aPtr) { User::Free(aPtr); }

class CRunTask : public CActive
{
public:
    CRunTask(MRunObserver &aObserver) : CActive(EPriorityStandard), iObserver(aObserver)
    {
        CActiveScheduler::Add(this);
    }
    ~CRunTask()
    {
        Cancel();
        delete iSrc;
    }
    TBool Busy() const { return IsActive(); }

    TInt Start(const TDesC &aCode, TBool aIntViews)
    {
        if (IsActive())
            return KErrInUse;
        delete iSrc;
        iSrc = aCode.Alloc();
        if (!iSrc)
            return KErrNoMemory;
        Mem::FillZ(&iJob, sizeof iJob);
        iJob.iSrc = iSrc->Ptr();
        iJob.iLen = iSrc->Length();
        iJob.iOpt.alloc = RsAlloc;
        iJob.iOpt.release = RsRelease;
        iJob.iOpt.mem_limit = 20 << 20;
        iJob.iOpt.out_limit = 100000;
        iJob.iOpt.int_views = aIntViews;
        iJob.iOpt.cancel = &iJob.iCancel;
        iJob.iStatus = &iStatus;
        iJob.iUiThread = RThread().Id();
        TInt stack = KWorkerStack;
        TInt err = iThread.Create(KNullDesC, ThreadMain, stack, &User::Allocator(), &iJob);
        if (err != KErrNone) {
            stack = 0x14000;
            err = iThread.Create(KNullDesC, ThreadMain, stack, &User::Allocator(), &iJob);
        }
        if (err != KErrNone)
            return err;
        iJob.iOpt.stack_limit = stack - KStackGuard;
        iThread.SetPriority(EPriorityLess);
        iStatus = KRequestPending;
        SetActive();
        iThread.Resume();
        return KErrNone;
    }

    void Stop() { iJob.iCancel = 1; }
    TBool Stopping() const { return iJob.iCancel != 0; }

private:
    struct TJob
        {
        const TUint16 *iSrc;
        TInt iLen;
        rs_options iOpt;
        volatile int iCancel;
        TInt iResult;
        const rs_char *iOut;
        TInt iOutLen;
        TInt iErrorPos;
        TInt iErrorLen;
        TRequestStatus *iStatus;
        TThreadId iUiThread;
        };

    static TInt ThreadMain(TAny *aPtr)
    {
        TJob *job = static_cast<TJob *>(aPtr);
        CTrapCleanup *cleanup = CTrapCleanup::New();
        int len = 0;
        job->iResult = rs_run((const rs_char *)job->iSrc, job->iLen, &job->iOpt, &job->iOut, &len);
        job->iOutLen = len;
        job->iErrorPos = rs_error_pos();
        job->iErrorLen = rs_error_length();
        RThread ui;
        if (ui.Open(job->iUiThread) == KErrNone) {
            TRequestStatus *s = job->iStatus;    // RequestComplete clears the pointer it gets
            ui.RequestComplete(s, KErrNone);
            ui.Close();
        }
        delete cleanup;
        return 0;
    }

    void RunL()
    {
        iThread.Close();
        TPtrC out(KNullDesC);
        if (iJob.iOut)
            out.Set((const TUint16 *)iJob.iOut, iJob.iOutLen);
        TRAPD(err, iObserver.RunDoneL(iJob.iResult, out, iJob.iErrorPos, iJob.iErrorLen));
        rs_free_output();
        User::LeaveIfError(err);
    }

    void DoCancel()
    {
        // only when the app ends: the process ends the thread with it
        iThread.Kill(KErrCancel);
        iThread.Close();
        TRequestStatus *s = &iStatus;
        User::RequestComplete(s, KErrCancel);
    }

    MRunObserver &iObserver;
    RThread iThread;
    TJob iJob;
    HBufC *iSrc;
};

// ---------------------------------------------------------------------------
// the screen: the code editor, or the output

const TInt KStripPad = 3;                    // pixels around the error strip's text
const TInt KStripLines = 3;                  // at most; then clipped with "..."
#define KErrorLineColor TRgb(0xff, 0xd0, 0xd0)
#define KStripColor TRgb(0xc0, 0x20, 0x20)

// Shades one paragraph (the error's line) of an editor: drawn behind the
// text through FORM's custom drawing, everything else passed on to the
// editor's own custom drawer (Avkon's). A plain editor can't have a format
// per paragraph, and a rich text editor crashes EKA2L1 on S60 3rd.
class TLineShade : public MFormCustomDraw
{
public:
    TLineShade() : iBase(NULL), iLayout(NULL), iStart(-1), iEnd(-1) {}

    void DrawBackground(const TParam &aParam, const TRgb &aBackground, TRect &aDrawn) const
    {
        if (iBase) iBase->DrawBackground(aParam, aBackground, aDrawn);
        else MFormCustomDraw::DrawBackground(aParam, aBackground, aDrawn);
    }
    void DrawLineGraphics(const TParam &aParam, const TLineInfo &aLineInfo) const
    {
        if (iBase) iBase->DrawLineGraphics(aParam, aLineInfo);
        else MFormCustomDraw::DrawLineGraphics(aParam, aLineInfo);
        if (iStart < 0 || !iLayout)
            return;
        // the paragraph's top and bottom, where they are formatted (layout
        // coordinates: the drawing's, less the layout's top left)
        TTmPosInfo2 pos;
        TTmLineInfo line;
        TInt top = KMinTInt, bottom = KMaxTInt;
        TBool hasTop = iLayout->FindDocPos(TTmDocPosSpec(iStart, TTmDocPosSpec::ELeading), pos, &line);
        if (hasTop) top = line.iOuterRect.iTl.iY;
        TBool hasBottom = iLayout->FindDocPos(TTmDocPosSpec(iEnd, TTmDocPosSpec::ETrailing), pos, &line);
        if (hasBottom) bottom = line.iOuterRect.iBr.iY;
        if (!hasTop && !hasBottom)
            return;
        TInt y = (aLineInfo.iOuterRect.iTl.iY + aLineInfo.iOuterRect.iBr.iY) / 2 - aParam.iTextLayoutTopLeft.iY;
        if (y < top || y >= bottom)
            return;
        CGraphicsContext &gc = aParam.iGc;
        gc.SetPenStyle(CGraphicsContext::ENullPen);
        gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        gc.SetBrushColor(KErrorLineColor);
        gc.DrawRect(aLineInfo.iOuterRect);
        gc.SetBrushStyle(CGraphicsContext::ENullBrush);
        gc.SetPenStyle(CGraphicsContext::ESolidPen);
    }
    void DrawText(const TParam &aParam, const TLineInfo &aLineInfo, const TCharFormat &aFormat,
                  const TDesC &aText, const TPoint &aTextOrigin, TInt aExtraPixels) const
    {
        if (iBase) iBase->DrawText(aParam, aLineInfo, aFormat, aText, aTextOrigin, aExtraPixels);
        else MFormCustomDraw::DrawText(aParam, aLineInfo, aFormat, aText, aTextOrigin, aExtraPixels);
    }
#ifdef SYMBIAN_CRYPTOSPI
    // Symbian^3 draws text through this one
    void DrawText(const TParam &aParam, const TLineInfo &aLineInfo, const TCharFormat &aFormat,
                  const TDesC &aText, const TInt aStart, const TInt aEnd,
                  const TPoint &aTextOrigin, TInt aExtraPixels) const
    {
        if (iBase) iBase->DrawText(aParam, aLineInfo, aFormat, aText, aStart, aEnd, aTextOrigin, aExtraPixels);
        else MFormCustomDraw::DrawText(aParam, aLineInfo, aFormat, aText, aStart, aEnd, aTextOrigin, aExtraPixels);
    }
#endif
    TRgb SystemColor(TUint aColorIndex, TRgb aDefaultColor) const
    {
        return iBase ? iBase->SystemColor(aColorIndex, aDefaultColor)
                     : MFormCustomDraw::SystemColor(aColorIndex, aDefaultColor);
    }

    const MFormCustomDraw *iBase;
    const CTextLayout *iLayout;
    TInt iStart, iEnd;          // the shaded paragraph (iEnd: its last character); -1: none
};

class CRsView : public CCoeControl, public MEikEdwinObserver
{
public:
    static CRsView *NewL(const TRect &aRect)
    {
        CRsView *self = new (ELeave) CRsView;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CRsView()
    {
        delete iClearLater;
        delete iCode;
        delete iOut;
        delete iErrText;
        delete iErrShown;
        delete iErrLines;
        if (iSmallFont)
            iCoeEnv->ScreenDevice()->ReleaseFont(iSmallFont);
    }

    CEikEdwin *Code() { return iCode; }
    TBool ShowingOutput() const { return iOut != NULL; }

    void ShowOutputL(const TDesC &aText)
    {
        if (!iOut) {
            iOut = NewOutputEdwinL();
            iCode->SetFocus(EFalse);
            iCode->MakeVisible(EFalse);
        }
        HBufC *text = aText.AllocLC();
        TPtr p = text->Des();
        ToEdwinNewlines(p);
        iOut->SetTextL(text);
        CleanupStack::PopAndDestroy(text);
        iOut->SetCursorPosL(0, EFalse);
        iOut->SetRect(Rect());
        iOut->MakeVisible(ETrue);
        iOut->SetFocus(ETrue);
        iOut->ActivateL();
        DrawDeferred();
    }

    void ShowCodeL(TInt aCursor)
    {
        delete iOut;
        iOut = NULL;
        Layout();
        iCode->MakeVisible(ETrue);
        iCode->SetFocus(ETrue);
        if (aCursor >= 0 && aCursor <= iCode->TextLength())
            iCode->SetCursorPosL(aCursor, EFalse);
        DrawDeferred();
    }

    // Shades the line at aPos (if >= 0) and shows aMessage under the code,
    // until the code is edited or ClearErrorL().
    void SetErrorL(TInt aPos, const TDesC &aMessage)
    {
        ClearErrorL();
        iErrText = aMessage.AllocL();
        CTextLayout *layout = iCode->TextLayout();
        if (aPos >= 0 && layout) {
            // (again each time: Avkon may have set its own drawer since)
            if (layout->CustomDraw() != &iShade) {
                iShade.iBase = layout->CustomDraw();
                layout->SetCustomDraw(&iShade);
            }
            iShade.iLayout = layout;
            CPlainText *text = iCode->Text();
            TInt pos = Min(aPos, text->DocumentLength());
            TInt para = text->ParagraphNumberForPos(pos);
            TInt len = 0;
            iShade.iStart = text->CharPosOfParagraph(len, para);
            iShade.iEnd = iShade.iStart + Max(len - 1, 0);     // not the paragraph end
        }
        Layout();
        DrawDeferred();
    }

    void ClearErrorL()
    {
        iClearLater->Cancel();
        if (iShade.iStart >= 0) {
            iShade.iStart = iShade.iEnd = -1;
            iCode->DrawDeferred();
        }
        if (iErrText) {
            delete iErrText;
            iErrText = NULL;
            Layout();
            DrawDeferred();
        }
    }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType);

private:
    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        iClearLater = new (ELeave) CAsyncCallBack(TCallBack(ClearLater, this), CActive::EPriorityStandard);
        iErrLines = new (ELeave) CArrayFixFlat<TPtrC>(KStripLines);
        // the code's font: the secondary font, smaller (but not below 12 px)
        const CFont *secondary = AknLayoutUtils::FontFromId(EAknLogicalFontSecondaryFont);
        if (secondary) {
            TFontSpec spec = secondary->FontSpecInTwips();
            spec.iHeight = Max(spec.iHeight * 3 / 5, iCoeEnv->ScreenDevice()->VerticalPixelsToTwips(12));
            iCoeEnv->ScreenDevice()->GetNearestFontInTwips(iSmallFont, spec);
        }
        iCode = NewCodeEdwinL();
        SetRect(aRect);
        ActivateL();
        iCode->SetFocus(ETrue);
    }

    CEikEdwin *NewCodeEdwinL()
    {
        CEikEdwin *e = new (ELeave) CEikEdwin;
        CleanupStack::PushL(e);
        e->SetContainerWindowL(*this);
        e->ConstructL(EEikEdwinNoAutoSelection | EEikEdwinJustAutoCurEnd, 0, KMaxCode, 0);
        // letters as typed, no predictive text
        e->SetAknEditorFlags(EAknEditorFlagNoT9 | EAknEditorFlagLatinInputModesOnly);
        e->SetAknEditorCase(EAknEditorLowerCase);
        e->SetAknEditorPermittedCaseModes(EAknEditorAllCaseModes);
        e->SetAknEditorInputMode(EAknEditorTextInputMode);
        e->SetAknEditorAllowedInputModes(EAknEditorTextInputMode | EAknEditorNumericInputMode);
        if (iSmallFont) {
            TFontSpec spec = iSmallFont->FontSpecInTwips();
            SetFontL(e, spec);
            // the editor's line spacing is for its usual font: set it for this one
            CParaFormat *pf = CParaFormat::NewLC();
            pf->iLineSpacingControl = CParaFormat::ELineSpacingExactlyInTwips;
            pf->iLineSpacingInTwips = spec.iHeight * 6 / 5;
            TParaFormatMask mask;
            mask.SetAttrib(EAttLineSpacing);
            mask.SetAttrib(EAttLineSpacingControl);
            CParaFormatLayer *layer = CParaFormatLayer::NewL(pf, mask);
            CleanupStack::PopAndDestroy(pf);
            e->SetParaFormatLayer(layer);           // takes it
        }
        e->AddEdwinObserverL(this);
        FinishEdwinL(e);
        CleanupStack::Pop(e);
        return e;
    }

    CEikEdwin *NewOutputEdwinL()
    {
        CEikEdwin *e = new (ELeave) CEikEdwin;
        CleanupStack::PushL(e);
        e->SetContainerWindowL(*this);
        e->ConstructL(EEikEdwinNoAutoSelection | EEikEdwinJustAutoCurEnd |
                      EEikEdwinReadOnly | EEikEdwinAvkonDisableCursor, 0, 0, 0);
        const CFont *font = AknLayoutUtils::FontFromId(EAknLogicalFontSecondaryFont);
        if (font)
            SetFontL(e, font->FontSpecInTwips());
        FinishEdwinL(e);
        CleanupStack::Pop(e);
        return e;
    }

    static void SetFontL(CEikEdwin *aEdwin, const TFontSpec &aSpec)
    {
        TCharFormat cf;
        TCharFormatMask mask;
        cf.iFontSpec = aSpec;
        mask.SetAttrib(EAttFontTypeface);
        mask.SetAttrib(EAttFontHeight);
        CCharFormatLayer *layer = CCharFormatLayer::NewL(cf, mask);
        aEdwin->SetCharFormatLayer(layer);          // takes it
    }

    void FinishEdwinL(CEikEdwin *aEdwin)
    {
#ifdef SYMBIAN_CRYPTOSPI
        // a scroll bar on Symbian^3 only: on S60 3rd (9.2) one stalled
        // rSSH's UI thread in EKA2L1
        aEdwin->CreateScrollBarFrameL()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff,
                                                                 CEikScrollBarFrame::EAuto);
#endif
        aEdwin->SetRect(Rect());
    }

    // the code changed: the error no longer applies. Not from inside the
    // editor's own event (it may be in the middle of an edit).
    void HandleEdwinEventL(CEikEdwin *, TEdwinEvent aEventType)
    {
        if (aEventType == EEventTextUpdate && (iShade.iStart >= 0 || iErrText) && !iClearLater->IsActive())
            iClearLater->CallBack();
    }
    static TInt ClearLater(TAny *aSelf)
    {
        TRAP_IGNORE(static_cast<CRsView *>(aSelf)->ClearErrorL());
        return 0;
    }

    const CFont *StripFont() const
    {
        return iSmallFont ? iSmallFont : AknLayoutUtils::FontFromId(EAknLogicalFontSecondaryFont);
    }
    TInt StripLineHeight() const { return StripFont()->HeightInPixels() + StripFont()->DescentInPixels() + 1; }

    // the code above, the error strip (if any) at the bottom
    void Layout()
    {
        iStripHeight = 0;
        iErrLines->Reset();
        delete iErrShown;
        iErrShown = NULL;
        if (iErrText) {
            TRAPD(err, WrapErrorL());
            if (err == KErrNone && iErrLines->Count())
                iStripHeight = iErrLines->Count() * StripLineHeight() + 2 * KStripPad;
        }
        TRect code(Rect());
        code.iBr.iY -= iStripHeight;
        if (iCode) iCode->SetRect(code);
        if (iOut) iOut->SetRect(Rect());
    }

    void WrapErrorL()
    {
        iErrShown = HBufC::NewL(iErrText->Length() + 4);    // room for the "..."
        *iErrShown = *iErrText;
        CArrayFixFlat<TInt> *widths = new (ELeave) CArrayFixFlat<TInt>(KStripLines);
        CleanupStack::PushL(widths);
        TInt w = Max(Rect().Width() - 2 * KStripPad, 16);
        for (TInt i = 0; i < KStripLines; i++)
            widths->AppendL(w);
        TPtr p = iErrShown->Des();
        AknTextUtils::WrapToArrayAndClipL(p, *widths, *StripFont(), *iErrLines);
        CleanupStack::PopAndDestroy(widths);
    }

    void SizeChanged() { Layout(); }
    TInt CountComponentControls() const { return 1; }
    CCoeControl *ComponentControl(TInt) const { return iOut ? iOut : iCode; }
    void Draw(const TRect &aRect) const
    {
        CWindowGc &gc = SystemGc();
        gc.SetBrushColor(KRgbWhite);
        gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        gc.SetPenStyle(CGraphicsContext::ENullPen);
        gc.DrawRect(aRect);
        if (iOut || !iStripHeight)
            return;
        // the error strip, just above the softkeys
        TRect strip(Rect());
        strip.iTl.iY = strip.iBr.iY - iStripHeight;
        gc.SetBrushColor(KStripColor);
        gc.DrawRect(strip);
        const CFont *font = StripFont();
        gc.UseFont(font);
        gc.SetPenStyle(CGraphicsContext::ESolidPen);
        gc.SetPenColor(KRgbWhite);
        gc.SetBrushStyle(CGraphicsContext::ENullBrush);
        TInt y = strip.iTl.iY + KStripPad + font->AscentInPixels();
        for (TInt i = 0; i < iErrLines->Count(); i++) {
            gc.DrawText((*iErrLines)[i], TPoint(strip.iTl.iX + KStripPad, y));
            y += StripLineHeight();
        }
        gc.DiscardFont();
    }
    void FocusChanged(TDrawNow aDrawNow)
    {
        CEikEdwin *e = iOut ? iOut : iCode;
        if (e) e->SetFocus(IsFocused(), aDrawNow);
    }

    CEikEdwin *iCode;
    CEikEdwin *iOut;            // while the output shows
    CFont *iSmallFont;          // the code's (and the strip's)
    CAsyncCallBack *iClearLater;
    TLineShade iShade;          // the error line's shading
    HBufC *iErrText;            // the error under the code, or NULL
    HBufC *iErrShown;           // iErrText as wrapped (and clipped)
    CArrayFixFlat<TPtrC> *iErrLines;    // into iErrShown
    TInt iStripHeight;
};

// ---------------------------------------------------------------------------
// app UI

class CRsAppUi : public CAknAppUi, public MRunObserver
{
public:
    void ConstructL()
    {
        gAppUi = this;
        InitPrivateDirL();
        LoadSettings();             // before BaseConstructL: the orientation
        BaseConstructL(EAknEnableSkin |
                       (iOrient == EOrientLandscape ? EAppOrientationLandscape :
                        iOrient == EOrientPortrait ? EAppOrientationPortrait :
                        EAppOrientationAutomatic));
        iRun = new (ELeave) CRunTask(*this);
        iView = CRsView::NewL(ClientRect());
        AddToStackL(iView);
        LoadCodeL();
        SetTitleL(_L("rSharp"));

        // Test hook (emulator diagnostics): C:\Data\rsharp-autotest.txt is
        // run at start; its output goes to C:\Data\rsharp-autotest-out.txt.
        RFile f;
        if (f.Open(iEikonEnv->FsSession(), KAutotest, EFileRead) == KErrNone) {
            CleanupClosePushL(f);
            TInt size = 0;
            f.Size(size);
            HBufC8 *raw = HBufC8::NewLC(size + 1);
            TPtr8 rp = raw->Des();
            f.Read(rp, size);
            rp.ZeroTerminate();
            SetCodeL((const char *)rp.Ptr());
            CleanupStack::PopAndDestroy(2, &f);
            iAutotest = ETrue;
            RunL();
        }
    }

    ~CRsAppUi()
    {
        TRAP_IGNORE(SaveCodeL());
        delete iRun;                // ends a run that is still going
        if (iView) {
            RemoveFromStack(iView);
            delete iView;
        }
        delete iLastOutput;
        delete iErrorMsg;
        gAppUi = NULL;
        // EKA2L1 hangs in the framework teardown after this; our clean-up is
        // done, so end the process (as rSSH does).
        User::Exit(KErrNone);
    }

    void HandleCommandL(TInt aCommand)
    {
        switch (aCommand) {
        case ERsCmdRun:
            RunL();
            break;
        case ERsCmdStop:
            if (iRun->Busy()) {
                iRun->Stop();
                iView->ShowOutputL(_L("Stopping..."));
            }
            break;
        case ERsCmdBack:
            ShowCodeL(-1);
            break;
        case ERsCmdShowOutput:
            if (iLastOutput)
                ShowOutputL(*iLastOutput, _L("Output"));
            break;
        case ERsCmdExamples:
            ExamplesL();
            break;
        case ERsCmdClear:
            if (iView->Code()->TextLength() == 0 || AskYesNoL(_L("Clear the code?"))) {
                ForgetErrorL();
                iView->Code()->SetTextL(&KNullDesC);
                iView->Code()->HandleTextChangedL();
                ShowCodeL(0);
            }
            break;
        case ERsCmdViews:
            iViews = !iViews;
            SaveSettings();
            InfoL(iViews ? _L("Integer results show their hex and binary digits.")
                         : _L("Results show only their value."));
            break;
        case ERsCmdOrientation:
            iOrient = iOrient == EOrientLandscape ? EOrientPortrait :
                      iOrient == EOrientPortrait ? EOrientAuto : EOrientLandscape;
            SetOrientationL(iOrient == EOrientLandscape ? EAppUiOrientationLandscape :
                            iOrient == EOrientPortrait ? EAppUiOrientationPortrait :
                            EAppUiOrientationAutomatic);
            SaveSettings();
            break;
        case ERsCmdHelp: {
            HBufC *help = FromUtf8LC(KHelp);
            ShowOutputL(*help, _L("What works"));
            CleanupStack::PopAndDestroy(help);
            break;
        }
        case ERsCmdAbout:
            InfoL(_L("rSharp 0.1\nA C# scratchpad: expressions, statements and LINQ, "
                     "run by its own interpreter on the phone.\n\nPart of SymbianApps."));
            break;
        case EAknSoftkeyExit:
        case EEikCmdExit:
        case ERsCmdExit:
            Exit();
            break;
        default:
            break;
        }
    }

    // From the view: the navigation key's centre.
    void SelectKeyL()
    {
        if (iRun->Busy())
            return;
        if (iView->ShowingOutput())
            ShowCodeL(-1);
        else
            RunL();
    }

    void RunDoneL(TInt aStatus, const TDesC &aOutput, TInt aErrorPos, TInt aErrorLen)
    {
        delete iLastOutput;
        iLastOutput = NULL;
        iLastOutput = aOutput.Length() ? aOutput.AllocL() :
                      aStatus == RS_CANCELLED ? _L("Stopped.").AllocL() : _L("(no output)").AllocL();
        iErrorPos = aErrorPos;
        delete iErrorMsg;
        iErrorMsg = NULL;
        if (aErrorLen > 0 && aErrorLen <= aOutput.Length())
            iErrorMsg = OneLineL(aOutput.Right(aErrorLen));
        if (aStatus == RS_COMPILE_ERROR) {
            // nothing ran: straight back to the code, the error under it
            ShowCodeL(-1);
        } else {
            const TDesC &title = aStatus == RS_RUNTIME_ERROR ? _L("Exception") :
                                 aStatus == RS_CANCELLED ? _L("Stopped") : _L("Output");
            ShowOutputL(*iLastOutput, title);
        }
        if (iAutotest) {
            iAutotest = EFalse;
            HBufC8 *utf8 = HBufC8::NewLC(iLastOutput->Length() * 3 + 16);
            TPtr8 up = utf8->Des();
            up.Format(_L8("status %d\n"), aStatus);
            TPtr8 rest((TUint8 *)up.Ptr() + up.Length(), 0, up.MaxLength() - up.Length());
            CnvUtfConverter::ConvertFromUnicodeToUtf8(rest, *iLastOutput);
            up.SetLength(up.Length() + rest.Length());
            RFile f;
            if (f.Replace(iEikonEnv->FsSession(), KAutotestOut, EFileWrite) == KErrNone) {
                f.Write(up);
                f.Close();
            }
            CleanupStack::PopAndDestroy(utf8);
        }
    }

private:
    enum TOrient { EOrientLandscape = 'L', EOrientPortrait = 'P', EOrientAuto = 'A' };

    void RunL()
    {
        if (iRun->Busy())
            return;
        if (iView->ShowingOutput())
            ShowCodeL(-1);
        iView->ClearErrorL();
        SaveCodeL();
        HBufC *code = iView->Code()->GetTextInHBufL();
        if (!code)
            code = HBufC::NewL(0);
        CleanupStack::PushL(code);
        TPtr p = code->Des();
        FromEdwinNewlines(p);
        TInt err = iRun->Start(*code, iViews);
        CleanupStack::PopAndDestroy(code);
        if (err != KErrNone) {
            TBuf<80> msg;
            msg.Format(_L("Can't start the interpreter (%d)"), err);
            InfoL(msg);
            return;
        }
        iErrorPos = -1;
        delete iErrorMsg;
        iErrorMsg = NULL;
        iView->ShowOutputL(_L("Running..."));
        SetTitleL(_L("Running"));
        SetCbaL(R_RS_CBA_RUNNING);
    }

    void ShowOutputL(const TDesC &aText, const TDesC &aTitle)
    {
        iView->ShowOutputL(aText);
        SetTitleL(aTitle);
        SetCbaL(R_RS_CBA_OUTPUT);
    }

    // Back to the code; after an error (once), with the error shown on it.
    void ShowCodeL(TInt aCursor)
    {
        if (iErrorMsg) {
            iView->SetErrorL(iErrorPos, *iErrorMsg);
            delete iErrorMsg;
            iErrorMsg = NULL;
        }
        iView->ShowCodeL(aCursor >= 0 ? aCursor : iErrorPos);
        iErrorPos = -1;
        SetTitleL(_L("rSharp"));
        SetCbaL(R_RS_CBA_CODE);
    }

    void SetCbaL(TInt aResource)
    {
        CEikButtonGroupContainer *cba = Cba();
        if (!cba) return;
        cba->SetCommandSetL(aResource);
        cba->DrawDeferred();
    }

    void SetTitleL(const TDesC &aTitle)
    {
        CEikStatusPane *sp = StatusPane();
        if (!sp) return;
        TRAP_IGNORE(
            CAknTitlePane *title = static_cast<CAknTitlePane *>(sp->ControlL(TUid::Uid(EEikStatusPaneUidTitle)));
            title->SetTextL(aTitle);
        );
    }

    void ExamplesL()
    {
        if (iRun->Busy())
            return;
        TInt chosen = 0;
        CDesCArrayFlat *names = new (ELeave) CDesCArrayFlat(KExampleCount);
        CleanupStack::PushL(names);
        for (TInt i = 0; i < KExampleCount; i++) {
            HBufC *n = FromUtf8LC(KExamples[i].iName);
            names->AppendL(*n);
            CleanupStack::PopAndDestroy(n);
        }
        CAknListQueryDialog *dlg = new (ELeave) CAknListQueryDialog(&chosen);
        dlg->PrepareLC(R_RS_LIST_QUERY);
        dlg->SetItemTextArray(names);
        dlg->SetOwnershipType(ELbmDoesNotOwnItemArray);
        TBool ok = dlg->RunLD();
        if (ok && chosen >= 0 && chosen < KExampleCount &&
            (iView->Code()->TextLength() == 0 || AskYesNoL(_L("Replace the code with this example?")))) {
            SetCodeL(KExamples[chosen].iCode);
            ShowCodeL(0);
        }
        CleanupStack::PopAndDestroy(names);
    }

    // An error message on one line: "System.X: message at line 3".
    static HBufC *OneLineL(const TDesC &aText)
    {
        HBufC *one = HBufC::NewL(aText.Length());
        TPtr p = one->Des();
        for (TInt i = 0; i < aText.Length(); i++) {
            TChar c = aText[i];
            if (c == '\n' || c == ' ') {
                if (p.Length() && p[p.Length() - 1] != ' ')
                    p.Append(' ');
            } else {
                p.Append(c);
            }
        }
        p.TrimRight();
        return one;
    }

    // new code: no error applies to it
    void ForgetErrorL()
    {
        delete iErrorMsg;
        iErrorMsg = NULL;
        iErrorPos = -1;
        iView->ClearErrorL();
    }

    void SetCodeL(const char *aUtf8)
    {
        ForgetErrorL();
        HBufC *text = FromUtf8LC(aUtf8);
        TPtr p = text->Des();
        ToEdwinNewlines(p);
        iView->Code()->SetTextL(text);
        iView->Code()->HandleTextChangedL();
        CleanupStack::PopAndDestroy(text);
    }

    // ---- menu and keys ----

    void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenu)
    {
        if (aResourceId != R_RS_MENU)
            return;
        TBool out = iView->ShowingOutput();
        aMenu->SetItemDimmed(ERsCmdBack, !out);
        aMenu->SetItemDimmed(ERsCmdShowOutput, out || !iLastOutput);
        aMenu->SetItemDimmed(ERsCmdClear, out);
    }

    void HandleResourceChangeL(TInt aType)
    {
        CAknAppUi::HandleResourceChangeL(aType);
        if (aType == KEikDynamicLayoutVariantSwitch && iView)
            iView->SetRect(ClientRect());
    }

    // EKA2L1 workaround, as in rSSH and NetSurf: it sends letters upper-case
    // without Shift and never sets modifier flags (but does send Shift
    // key-down/up events). This sees every key first, dialogs included.
    // Harmless on a phone.
    void HandleWsEventL(const TWsEvent &aEvent, CCoeControl *aDestination)
    {
        TInt type = aEvent.Type();
        if (type == EEventKeyDown || type == EEventKeyUp) {
            TInt scan = aEvent.Key()->iScanCode;
            if (scan == EStdKeyLeftShift || scan == EStdKeyRightShift)
                iShiftDown = type == EEventKeyDown;
        } else if (type == EEventKey) {
            TKeyEvent *key = aEvent.Key();
            if (iShiftDown)
                key->iModifiers |= EModifierShift | EModifierLeftShift;
            if (key->iCode >= 'A' && key->iCode <= 'Z' &&
                !(key->iModifiers & (EModifierShift | EModifierCapsLock)))
                key->iCode += 'a' - 'A';
        }
        CAknAppUi::HandleWsEventL(aEvent, aDestination);
    }

    // ---- files: the code and the settings, in the private folder ----

    void InitPrivateDirL()
    {
        RFs &fs = iEikonEnv->FsSession();
        fs.CreatePrivatePath(EDriveC);
        TFileName priv;
        User::LeaveIfError(fs.PrivatePath(priv));       // \private\<SID>\ (no drive)
        iDir.Copy(_L("C:"));
        iDir.Append(priv);
    }

    void LoadSettings()
    {
        iViews = ETrue;
        iOrient = EOrientAuto;
        TFileName path(iDir);
        path.Append(_L("settings.txt"));
        RFile f;
        if (f.Open(iEikonEnv->FsSession(), path, EFileRead) != KErrNone)
            return;
        TBuf8<16> buf;
        f.Read(buf);
        f.Close();
        if (buf.Length() >= 1) iViews = buf[0] == 'V';
        if (buf.Length() >= 2 && (buf[1] == 'L' || buf[1] == 'P' || buf[1] == 'A'))
            iOrient = (TOrient)buf[1];
    }

    void SaveSettings()
    {
        TFileName path(iDir);
        path.Append(_L("settings.txt"));
        RFile f;
        if (f.Replace(iEikonEnv->FsSession(), path, EFileWrite) != KErrNone)
            return;
        TBuf8<4> buf;
        buf.Append(iViews ? 'V' : 'v');
        buf.Append((TUint8)iOrient);
        f.Write(buf);
        f.Close();
    }

    void LoadCodeL()
    {
        TFileName path(iDir);
        path.Append(_L("code.cs"));
        RFile f;
        RFs &fs = iEikonEnv->FsSession();
        if (f.Open(fs, path, EFileRead) != KErrNone) {
            SetCodeL(KExamples[0].iCode);           // the first run
            return;
        }
        CleanupClosePushL(f);
        TInt size = 0;
        f.Size(size);
        if (size > KMaxCode * 3) size = KMaxCode * 3;
        HBufC8 *raw = HBufC8::NewLC(size + 1);
        TPtr8 rp = raw->Des();
        f.Read(rp, size);
        rp.ZeroTerminate();
        SetCodeL((const char *)rp.Ptr());
        CleanupStack::PopAndDestroy(2, &f);
    }

    void SaveCodeL()
    {
        if (!iView) return;
        HBufC *code = iView->Code()->GetTextInHBufL();
        if (!code) code = HBufC::NewL(0);
        CleanupStack::PushL(code);
        TPtr p = code->Des();
        FromEdwinNewlines(p);
        HBufC8 *utf8 = HBufC8::NewLC(code->Length() * 3 + 1);
        TPtr8 up = utf8->Des();
        CnvUtfConverter::ConvertFromUnicodeToUtf8(up, *code);
        TFileName path(iDir);
        path.Append(_L("code.cs"));
        RFile f;
        if (f.Replace(iEikonEnv->FsSession(), path, EFileWrite) == KErrNone) {
            f.Write(up);
            f.Close();
        }
        CleanupStack::PopAndDestroy(2, code);
    }

    CRsView *iView;
    CRunTask *iRun;
    HBufC *iLastOutput;
    TInt iErrorPos;
    HBufC *iErrorMsg;           // the last run's error, until the code shows it
    TBool iViews;
    TOrient iOrient;
    TBool iShiftDown;
    TBool iAutotest;
    TFileName iDir;
};

// ---------------------------------------------------------------------------

TKeyResponse CRsView::OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType)
{
    if (aType == EEventKey && (aKey.iCode == EKeyOK || aKey.iCode == EKeyDevice3) && gAppUi) {
        gAppUi->SelectKeyL();
        return EKeyWasConsumed;
    }
    if (iOut) {
        if (aType != EEventKey)
            return EKeyWasNotConsumed;
        switch (aKey.iCode) {
        case EKeyUpArrow: iOut->MoveDisplayL(TCursorPosition::EFLineUp); return EKeyWasConsumed;
        case EKeyDownArrow: iOut->MoveDisplayL(TCursorPosition::EFLineDown); return EKeyWasConsumed;
        case EKeyLeftArrow: iOut->MoveDisplayL(TCursorPosition::EFPageUp); return EKeyWasConsumed;
        case EKeyRightArrow: iOut->MoveDisplayL(TCursorPosition::EFPageDown); return EKeyWasConsumed;
        default: return EKeyWasNotConsumed;
        }
    }
    return iCode->OfferKeyEventL(aKey, aType);
}

class CRsDocument : public CAknDocument
{
public:
    CRsDocument(CEikApplication &aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi *CreateAppUiL() { return new (ELeave) CRsAppUi; }
};

class CRsApplication : public CAknApplication
{
private:
    TUid AppDllUid() const { return KUidRsharp; }
    CApaDocument *CreateDocumentL() { return new (ELeave) CRsDocument(*this); }
};

LOCAL_C CApaApplication *NewApplication() { return new CRsApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
