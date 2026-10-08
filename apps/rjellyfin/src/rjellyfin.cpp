// rJellyfin: a Jellyfin client for Symbian^3 (experimental).
//
// Stage 1 and 2: sign in, browse the libraries (with artwork), and play
// music. Video comes later.
//
// - Screens are a stack of pages: Libraries > (a music library's Albums /
//   Album artists / Songs) > albums > tracks, or a library's folders. Back
//   goes up. Item pages start with a "now playing" row, as in rInternetRadio.
// - The API (JSON over HTTP or HTTPS) runs on a worker thread (CNetTask);
//   artwork is fetched one image at a time on another and decoded with the
//   phone's image decoders (CImageLoader).
// - Music: Jellyfin sends MP3 (as is, or converted on the fly from FLAC, AAC,
//   ...) through /Audio/<id>/universal, which apps/common/audio plays. The
//   queue is the tracks of the page a track was picked from; the next one
//   starts when the last has been heard.

#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <akntitle.h>
#include <aknlists.h>
#include <aknquerydialog.h>
#include <akniconutils.h>
#include <avkon.hrh>
#include <avkon.rsg>
#include <badesca.h>
#include <eikenv.h>
#include <eiklbo.h>
#include <eikmenup.h>
#include <eikspane.h>
#include <eikstart.h>
#include <eikfrlbd.h>
#include <e32math.h>
#include <coecntrl.h>
#include <f32file.h>
#include <fbs.h>
#include <gulicon.h>
#include <imageconversion.h>
#include <utf.h>
#include <remconcoreapitarget.h>
#include <remconcoreapitargetobserver.h>
#include <remconinterfaceselector.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rjellyfin.rsg>
#include "rjellyfin.hrh"
#include "radio_engine.h"
#include "audio_out.h"
#include "rsym_https.h"
#include "rsym_log.h"
#include "rsym_rlog.h"
#include "cJSON.h"

const TUid KUidJellyfin = { TInt32(0xE5A1E050) };
#define KVersion "0.1.0"

class CJfAppUi;
static CJfAppUi *gAppUi;

// ---------------------------------------------------------------------------
// helpers

static TBool ValidUtf8(const char *s, TInt aLen)
{
    for (TInt i = 0; i < aLen; ) {
        TUint8 c = s[i];
        TInt n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 :
                 (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (n && i + n >= aLen))
            return EFalse;
        for (TInt k = 1; k <= n; k++)
            if ((TUint8(s[i + k]) & 0xC0) != 0x80)
                return EFalse;
        i += n + 1;
    }
    return ETrue;
}

static HBufC *TextLC(const char *aText)
{
    TInt len = aText ? User::StringLength((const TUint8 *)aText) : 0;
    TPtrC8 in((const TUint8 *)aText, len);
    HBufC *out = HBufC::NewLC(len + 1);
    TPtr p = out->Des();
    if (!ValidUtf8(aText, len) || CnvUtfConverter::ConvertToUnicodeFromUtf8(p, in) != 0)
        p.Copy(in);
    return out;
}

static HBufC8 *ToUtf8LC(const TDesC &aText)
{
    HBufC8 *out = HBufC8::NewLC(aText.Length() * 3 + 1);
    TPtr8 p = out->Des();
    CnvUtfConverter::ConvertFromUnicodeToUtf8(p, aText);
    p.ZeroTerminate();
    return out;
}

static void Copy(char *aDst, int aSize, const char *aSrc)
{
    strncpy(aDst, aSrc ? aSrc : "", aSize - 1);
    aDst[aSize - 1] = 0;
}

static TBool AskYesNoL(const TDesC &aPrompt)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    return dlg->ExecuteLD(R_JF_YESNO_QUERY, aPrompt) != 0;
}

static void InfoL(const TDesC &aText)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    dlg->ExecuteLD(R_JF_OK_QUERY, aText);
}

static void InfoL(const char *aText)
{
    HBufC *t = TextLC(aText);
    InfoL(*t);
    CleanupStack::PopAndDestroy(t);
}

static TBool QueryTextL(const TDesC &aPrompt, TDes &aText, TInt aResource = R_JF_TEXT_QUERY)
{
    CAknTextQueryDialog *dlg = CAknTextQueryDialog::NewL(aText);
    dlg->SetPromptL(aPrompt);
    dlg->SetPredictiveTextInputPermitted(EFalse);
    return dlg->ExecuteLD(aResource) != 0;
}

// Ask for a UTF-8 string, editing aValue in place. EFalse if cancelled.
static TBool QueryUtf8L(const TDesC &aPrompt, char *aValue, int aSize,
                        TInt aResource = R_JF_TEXT_QUERY)
{
    HBufC *cur = TextLC(aValue);
    TBuf<256> text(cur->Left(256));
    CleanupStack::PopAndDestroy(cur);
    if (!QueryTextL(aPrompt, text, aResource))
        return EFalse;
    if (aResource == R_JF_TEXT_QUERY)
        text.TrimAll();
    HBufC8 *u = ToUtf8LC(text);
    Copy(aValue, aSize, (const char *)u->Ptr());
    CleanupStack::PopAndDestroy(u);
    return ETrue;
}

static HBufC *SymbianPathLC(const char *aPath)
{
    HBufC *p = TextLC(aPath);
    TPtr ptr = p->Des();
    for (TInt i = 0; i < ptr.Length(); i++)
        if (ptr[i] == '/')
            ptr[i] = '\\';
    return p;
}

static const char *JStr(cJSON *aObj, const char *aName)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(aObj, aName));
    return s ? s : "";
}

static double JNum(cJSON *aObj, const char *aName)
{
    cJSON *n = cJSON_GetObjectItem(aObj, aName);
    return cJSON_IsNumber(n) ? n->valuedouble : 0;
}

// ---------------------------------------------------------------------------
// items and pages

struct TItem
    {
    char iId[40];
    char iName[160];
    char iType[24];         // Jellyfin's Type, or "#albums" etc. (our own rows)
    char iSub[160];         // second line
    char iImageId[40];      // item whose Primary image to show; "" for none
    char iCollection[24];   // CollectionType of a library
    char iArtist[96];
    TBool iFolder;
    TInt iSeconds;          // running time
    TInt iIcon;             // index in the icon array (0: the app's icon)
    };

enum TPageKind { EPageSetup, EPageLibraries, EPageMusic, EPageItems, EPageSettings };

class CPage : public CBase
{
public:
    ~CPage() { iItems.ResetAndDestroy(); }
    TPageKind iKind;
    TBuf<100> iTitle;
    char iPath[700];        // the API query (without paging) of an items page
    RPointerArray<TItem> iItems;
    TInt iTotal;            // TotalRecordCount
    TInt iCurrent;          // highlighted row, kept when coming back
    TBool iLoaded;
};

// ---------------------------------------------------------------------------
// the list

class CJfList : public CCoeControl, public MEikListBoxObserver
{
public:
    static CJfList *NewL(const TRect &aRect)
    {
        CJfList *self = new (ELeave) CJfList;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CJfList()
    {
        delete iListBox;        // owns the icon array
        delete iItems;
    }

    void ResetL() { iItems->Reset(); }
    void AddRowL(TInt aIcon, const TDesC &aTitle, const TDesC &aValue)
    {
        HBufC *row = RowLC(aIcon, aTitle, aValue);
        iItems->AppendL(*row);
        CleanupStack::PopAndDestroy(row);
    }
    void AddRowL(TInt aIcon, const char *aTitle, const char *aValue)
    {
        HBufC *t = TextLC(aTitle);
        HBufC *v = TextLC(aValue);
        AddRowL(aIcon, *t, *v);
        CleanupStack::PopAndDestroy(2, t);
    }
    void AddRowL(TInt aIcon, const TDesC &aTitle, const char *aValue)
    {
        HBufC *v = TextLC(aValue);
        AddRowL(aIcon, aTitle, *v);
        CleanupStack::PopAndDestroy(v);
    }
    void SetRowL(TInt aIndex, TInt aIcon, const TDesC &aTitle, const TDesC &aValue)
    {
        if (aIndex >= iItems->Count())
            return;
        HBufC *row = RowLC(aIcon, aTitle, aValue);
        iItems->Delete(aIndex);
        iItems->InsertL(aIndex, *row);
        CleanupStack::PopAndDestroy(row);
        iListBox->DrawItem(aIndex);
    }
    // Change the icon of a row ("<icon>\t...").
    void SetRowIconL(TInt aIndex, TInt aIcon)
    {
        if (aIndex >= iItems->Count())
            return;
        TPtrC old = (*iItems)[aIndex];
        TInt tab = old.Locate('\t');
        if (tab < 0)
            return;
        HBufC *row = HBufC::NewLC(old.Length() + 8);
        row->Des().AppendNum(aIcon);
        row->Des().Append(old.Mid(tab));
        iItems->Delete(aIndex);
        iItems->InsertL(aIndex, *row);
        CleanupStack::PopAndDestroy(row);
        iListBox->DrawItem(aIndex);
    }
    void DoneL(TInt aCurrent)
    {
        iListBox->HandleItemAdditionL();
        if (aCurrent >= iItems->Count())
            aCurrent = iItems->Count() - 1;
        if (aCurrent < 0)
            aCurrent = 0;
        iListBox->SetCurrentItemIndex(aCurrent);
        iListBox->DrawDeferred();
    }
    TInt CurrentIndex() const { return iListBox->CurrentItemIndex(); }
    TInt Count() const { return iItems->Count(); }

    // Icons: 0 is the app's own icon (the placeholder); artwork is added.
    CArrayPtr<CGulIcon> *Icons() { return iListBox->ItemDrawer()->FormattedCellData()->IconArray(); }
    TSize IconSize()
    {
        TSize s = iListBox->ItemDrawer()->FormattedCellData()->SubCellSize(0);
        return s.iWidth > 8 && s.iHeight > 8 ? s : TSize(60, 60);
    }
    void ResetIconsL()
    {
        CArrayPtr<CGulIcon> *icons = Icons();
        while (icons->Count() > 1) {
            delete icons->At(icons->Count() - 1);
            icons->Delete(icons->Count() - 1);
        }
    }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType);
    void HandleListBoxEventL(CEikListBox *aListBox, TListBoxEvent aEvent);

private:
    static HBufC *RowLC(TInt aIcon, const TDesC &aTitle, const TDesC &aValue)
    {
        HBufC *row = HBufC::NewLC(aTitle.Length() + aValue.Length() + 12);
        TPtr p = row->Des();
        p.AppendNum(aIcon);
        p.Append('\t');
        for (TInt i = 0; i < aTitle.Length(); i++)
            p.Append(aTitle[i] == '\t' ? ' ' : aTitle[i]);
        p.Append('\t');
        for (TInt i = 0; i < aValue.Length(); i++)
            p.Append(aValue[i] == '\t' ? ' ' : aValue[i]);
        return row;
    }

    void ConstructL(const TRect &aRect)
    {
        CreateWindowL();
        iListBox = new (ELeave) CAknDoubleLargeStyleListBox;
        iListBox->ConstructL(this, EAknListBoxSelectionList);
        iListBox->CreateScrollBarFrameL(ETrue);
        iListBox->ScrollBarFrame()->SetScrollBarVisibilityL(
            CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
        iListBox->SetListBoxObserver(this);
        iItems = new (ELeave) CDesCArrayFlat(32);
        iListBox->Model()->SetItemTextArray(iItems);
        iListBox->Model()->SetOwnershipType(ELbmDoesNotOwnItemArray);
        CArrayPtr<CGulIcon> *icons = new (ELeave) CArrayPtrFlat<CGulIcon>(32);
        iListBox->ItemDrawer()->FormattedCellData()->SetIconArray(icons);
        SetRect(aRect);
        // the placeholder: the app's icon, at the list's icon size
        TFileName mif;
        TFileName exe = RProcess().FileName();
        mif.Append(exe.Length() ? TChar(exe[0]) : TChar('C'));
        mif.Append(_L(":\\resource\\apps\\rjellyfin_aif.mif"));
        CFbsBitmap *bmp = NULL, *mask = NULL;
        TRAPD(err, AknIconUtils::CreateIconL(bmp, mask, mif, 16384, 16385));
        if (err == KErrNone) {
            AknIconUtils::SetSize(bmp, IconSize());
            AknIconUtils::SetSize(mask, IconSize());
            icons->AppendL(CGulIcon::NewL(bmp, mask));
        } else {
            rsym_log("icon: placeholder failed (%d)", err);
            CFbsBitmap *blank = new (ELeave) CFbsBitmap;
            blank->Create(TSize(1, 1), EColor16M);
            icons->AppendL(CGulIcon::NewL(blank));
        }
        ActivateL();
    }
    void SizeChanged() { iListBox->SetRect(Rect()); }
    TInt CountComponentControls() const { return 1; }
    CCoeControl *ComponentControl(TInt) const { return iListBox; }

    CAknDoubleLargeStyleListBox *iListBox;
    CDesCArrayFlat *iItems;
};

// ---------------------------------------------------------------------------
// one HTTP(S) request on a worker thread (as in rDrive)

class MNetObserver
{
public:
    virtual void NetDoneL(TInt aTag, const rsym_http_response &aResp, TInt aResult) = 0;
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

    // aReq's strings must stay valid until NetDoneL; aTag comes back there.
    TInt Start(const rsym_http_request &aReq, TInt aTag)
    {
        if (IsActive())
            return KErrInUse;
        rsym_http_response_free(&iJob.iResp);
        Mem::FillZ(&iJob.iResp, sizeof iJob.iResp);
        iJob.iReq = aReq;
        iJob.iStatus = &iStatus;
        iJob.iUiThread = RThread().Id();
        iTag = aTag;
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
        TRequestStatus *iStatus;
        TThreadId iUiThread;
        };

    static TInt ThreadMain(TAny *aPtr)
    {
        TJob *job = static_cast<TJob *>(aPtr);
        CTrapCleanup *cleanup = CTrapCleanup::New();
        RadioNetInit();
        job->iResult = rsym_https_request(&job->iReq, &job->iResp);
        RThread ui;
        if (ui.Open(job->iUiThread) == KErrNone) {
            TRequestStatus *s = job->iStatus;
            ui.RequestComplete(s, KErrNone);
            ui.Close();
        }
        delete cleanup;
        return 0;
    }
    void RunL()
    {
        iThread.Close();
        iObserver.NetDoneL(iTag, iJob.iResp, iJob.iResult);
    }
    TInt RunError(TInt aError)
    {
        rsym_log("net: RunL left %d", aError);
        return KErrNone;
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
    TInt iTag;
};

// ---------------------------------------------------------------------------
// artwork: decodes one JPEG/PNG into a bitmap with the phone's decoders

class MImageObserver
{
public:
    virtual void ImageDecodedL(CFbsBitmap *aBitmap, TInt aError) = 0;   // takes aBitmap
};

class CImageLoader : public CActive
{
public:
    CImageLoader(MImageObserver &aObserver) : CActive(EPriorityLow), iObserver(aObserver)
    {
        CActiveScheduler::Add(this);
    }
    ~CImageLoader() { Cancel(); Reset(); }

    TBool Busy() const { return IsActive(); }

    void StartL(RFs &aFs, const TDesC8 &aData)
    {
        Cancel();
        Reset();
        iData = aData.AllocL();             // kept until decoded
        iDecoder = CImageDecoder::DataNewL(aFs, *iData);
        iBitmap = new (ELeave) CFbsBitmap;
        User::LeaveIfError(iBitmap->Create(iDecoder->FrameInfo(0).iOverallSizeInPixels,
                                           EColor16M));
        iDecoder->Convert(&iStatus, *iBitmap);
        SetActive();
    }

private:
    void Reset()
    {
        delete iDecoder;
        iDecoder = NULL;
        delete iBitmap;
        iBitmap = NULL;
        delete iData;
        iData = NULL;
    }
    void RunL()
    {
        CFbsBitmap *bmp = iStatus.Int() == KErrNone ? iBitmap : NULL;
        if (bmp)
            iBitmap = NULL;
        TInt err = iStatus.Int();
        Reset();
        iObserver.ImageDecodedL(bmp, err);
    }
    TInt RunError(TInt aError)
    {
        rsym_log("image: RunL left %d", aError);
        return KErrNone;
    }
    void DoCancel()
    {
        if (iDecoder)
            iDecoder->Cancel();
    }
    MImageObserver &iObserver;
    CImageDecoder *iDecoder;
    CFbsBitmap *iBitmap;
    HBufC8 *iData;
};

// ---------------------------------------------------------------------------
// app UI

enum TNetTag { ETagLogin = 1, ETagPage, ETagMore, ETagImage };

class CJfAppUi : public CAknAppUi, public MNetObserver, public MRadioNews,
                 public MAudioOutObserver, public MImageObserver,
                 public MRemConCoreApiTargetObserver
{
public:
    void ConstructL()
    {
        gAppUi = this;
        InitPrivateDirL();
        LoadSettings();         // before BaseConstructL: the orientation
        BaseConstructL(EAknEnableSkin |
                       (iOrient == 'L' ? EAppOrientationLandscape :
                        iOrient == 'P' ? EAppOrientationPortrait : EAppOrientationAutomatic));
        LoadCa();
        User::LeaveIfError(RadioInit());
        rsym_https_allow_untrusted(iInsecure ? iHost : NULL);
        iAudio = new (ELeave) CAudioOut(this);
        iAudio->ConstructL();
        iNotify = new (ELeave) CRadioNotify(*this);
        iNet = new (ELeave) CNetTask(*this);
        iImageNet = new (ELeave) CNetTask(*this);
        iImage = new (ELeave) CImageLoader(*this);
        iTick = CPeriodic::NewL(CActive::EPriorityLow);
        iTick->Start(1000000, 1000000, TCallBack(Tick, this));
        iList = CJfList::NewL(ClientRect());
        AddToStackL(iList);
        TRAPD(err,
            iRemCon = CRemConInterfaceSelector::NewL();
            iRemConTarget = CRemConCoreApiTarget::NewL(*iRemCon, *this);
            iRemCon->OpenTargetL();
        );
        rsym_log("remcon: %d", err);
        ReadAutotest();
        if (iToken[0] && !iAutoPassword[0])
            PushLibrariesL();
        else
            PushPageL(EPageSetup, _L("Sign in to Jellyfin"));
        if (iAutoPassword[0])
            SignInL();
    }

    ~CJfAppUi()
    {
        RadioStop();
        delete iTick;
        delete iNotify;
        delete iAudio;
        delete iImage;
        delete iNet;
        delete iImageNet;
        delete iRemCon;
        if (iList) {
            RemoveFromStack(iList);
            delete iList;
        }
        iPages.ResetAndDestroy();
        iQueue.ResetAndDestroy();
        iImageIds.Reset();
        free(iCa);
        gAppUi = NULL;
        rsym_log("exit");
        rsym_rlog_flush(1000);
        // EKA2L1 hangs in the framework teardown after this; our clean-up is
        // done, so end the process (as rSSH does).
        User::Exit(KErrNone);
    }

    void SelectL(TInt aRow);
    void VolumeL(TInt aDelta);

private:
    // ---- start-up and settings ----

    void InitPrivateDirL()
    {
        RFs &fs = iEikonEnv->FsSession();
        fs.CreatePrivatePath(EDriveC);
        TFileName priv;
        User::LeaveIfError(fs.PrivatePath(priv));
        TPtr8 dir(reinterpret_cast<TUint8 *>(iPrivDir), 0, sizeof(iPrivDir) - 1);
        dir.Copy(_L8("C:"));
        for (TInt i = 0; i < priv.Length() - 1; i++)
            dir.Append(priv[i] == '\\' ? '/' : TUint8(priv[i]));
        dir.ZeroTerminate();
        rsym_log_init(iPrivDir, "rjellyfin");
        TFileName exe = RProcess().FileName();
        iInstallDrive = exe.Length() > 0 ? char(exe[0]) : 'C';
    }

    void LoadCa()
    {
        char path[64];
        snprintf(path, sizeof path, "%c:/private/e5a1e050/ca-bundle", iInstallDrive);
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            rsym_log("ca: cannot open %s", path);
            return;
        }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        iCa = (char *)malloc(n + 1);
        if (iCa && fread(iCa, 1, n, fp) == (size_t)n) {
            iCa[n] = 0;
            RadioSetCa(iCa);
        }
        fclose(fp);
    }

    void CfgPath(char *aBuf, int aLen)
    {
        snprintf(aBuf, aLen, "%s/jellyfin.cfg", iPrivDir);
    }

    void LoadSettings()
    {
        iVolume = 7;
        iOrient = 'A';
        char path[96];
        CfgPath(path, sizeof path);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char line[600];
            while (fgets(line, sizeof line, fp)) {
                line[strcspn(line, "\r\n")] = 0;
                char *eq = strchr(line, '=');
                if (!eq)
                    continue;
                *eq++ = 0;
                if (!strcmp(line, "server")) Copy(iServer, sizeof iServer, eq);
                else if (!strcmp(line, "user")) Copy(iUser, sizeof iUser, eq);
                else if (!strcmp(line, "userid")) Copy(iUserId, sizeof iUserId, eq);
                else if (!strcmp(line, "token")) Copy(iToken, sizeof iToken, eq);
                else if (!strcmp(line, "deviceid")) Copy(iDeviceId, sizeof iDeviceId, eq);
                else if (!strcmp(line, "insecure")) iInsecure = atoi(eq) != 0;
                else if (!strcmp(line, "volume")) iVolume = atoi(eq);
                else if (!strcmp(line, "orientation") && strchr("LPA", eq[0])) iOrient = eq[0];
            }
            fclose(fp);
        }
        if (iVolume < 0 || iVolume > 10)
            iVolume = 7;
        if (!iDeviceId[0]) {
            // identifies this phone to the server (its device list)
            TInt64 seed = User::FastCounter() ^ User::TickCount();
            snprintf(iDeviceId, sizeof iDeviceId, "rjellyfin-%08x%08x",
                     (unsigned)Math::Rand(seed), (unsigned)Math::Rand(seed));
        }
        ParseServer();
    }

    void SaveSettings()
    {
        char path[96];
        CfgPath(path, sizeof path);
        FILE *fp = fopen(path, "w");
        if (!fp)
            return;
        // (the password is never stored: only the token the server gave)
        fprintf(fp, "server=%s\nuser=%s\nuserid=%s\ntoken=%s\ndeviceid=%s\ninsecure=%d\n"
                    "volume=%d\norientation=%c\n",
                iServer, iUser, iUserId, iToken, iDeviceId, iInsecure, iVolume, iOrient);
        fclose(fp);
    }

    // "http[s]://host[:port][/base]" -> iTls, iHost, iPort, iBase
    TBool ParseServer()
    {
        const char *p = iServer;
        iTls = EFalse;
        if (!strncasecmp(p, "https://", 8)) {
            iTls = ETrue;
            p += 8;
        } else if (!strncasecmp(p, "http://", 7)) {
            p += 7;
        }
        const char *slash = strchr(p, '/');
        const char *end = slash ? slash : p + strlen(p);
        const char *colon = (const char *)memchr(p, ':', end - p);
        int hlen = (int)((colon ? colon : end) - p);
        if (hlen <= 0 || hlen >= (int)sizeof iHost) {
            iHost[0] = 0;
            return EFalse;
        }
        memcpy(iHost, p, hlen);
        iHost[hlen] = 0;
        iPort = colon ? atoi(colon + 1) : 0;
        Copy(iBase, sizeof iBase, slash ? slash : "");
        int bl = strlen(iBase);
        while (bl > 0 && iBase[bl - 1] == '/')
            iBase[--bl] = 0;
        return ETrue;
    }

    // Test hook (emulator diagnostics): C:\Data\rjellyfin-autotest.txt with
    // server=, user=, password= (and insecure=1) lines signs in at start;
    // play=1 then opens the first music library's first album and plays it.
    void ReadAutotest()
    {
        FILE *fp = fopen("C:\\Data\\rjellyfin-autotest.txt", "r");
        if (!fp)
            return;
        char line[300];
        while (fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq++ = 0;
            if (!strcmp(line, "server")) Copy(iServer, sizeof iServer, eq);
            else if (!strcmp(line, "user")) Copy(iUser, sizeof iUser, eq);
            else if (!strcmp(line, "password")) Copy(iAutoPassword, sizeof iAutoPassword, eq);
            else if (!strcmp(line, "insecure")) iInsecure = atoi(eq) != 0;
            else if (!strcmp(line, "play")) iAutoStep = atoi(eq) ? 1 : 0;
        }
        fclose(fp);
        ParseServer();
        rsym_log("autotest: %s as %s%s", iServer, iUser, iAutoStep ? ", then play" : "");
    }

    // ---- requests ----

    void AuthHeader(char *aBuf, int aLen, TBool aWithToken)
    {
        snprintf(aBuf, aLen,
                 "Authorization: MediaBrowser Client=\"rJellyfin\", Device=\"Nokia Symbian\", "
                 "DeviceId=\"%s\", Version=\"" KVersion "\"%s%s%s\r\n"
                 "Accept: application/json\r\n",
                 iDeviceId, aWithToken ? ", Token=\"" : "", aWithToken ? iToken : "",
                 aWithToken ? "\"" : "");
    }

    void FillRequest(rsym_http_request &aReq, const char *aMethod, const char *aPath,
                     const char *aHeaders)
    {
        Mem::FillZ(&aReq, sizeof aReq);
        aReq.method = aMethod;
        aReq.host = iHost;
        aReq.port = iPort;
        aReq.path = aPath;
        aReq.plain = !iTls;
        aReq.headers = aHeaders;
        aReq.user_agent = "rJellyfin/" KVersion " (Symbian)";
        aReq.timeout_ms = 30000;
    }

    void SignInL()
    {
        if (!ParseServer()) {
            InfoL(_L("Set the server address first, for example http://192.168.1.10:8096"));
            return;
        }
        if (iNet->Busy())
            return;
        char *password = iAutoPassword;
        TBuf<64> pw;
        if (!password[0]) {
            if (!QueryTextL(_L("Password"), pw, R_JF_PASSWORD_QUERY))
                return;
            HBufC8 *u = ToUtf8LC(pw);
            Copy(iTypedPassword, sizeof iTypedPassword, (const char *)u->Ptr());
            CleanupStack::PopAndDestroy(u);
            password = iTypedPassword;
        }
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "Username", iUser);
        cJSON_AddStringToObject(body, "Pw", password);
        char *json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        Copy(iBody, sizeof iBody, json);
        cJSON_free(json);
        memset(iTypedPassword, 0, sizeof iTypedPassword);
        memset(iAutoPassword, 0, sizeof iAutoPassword);
        rsym_https_allow_untrusted(iInsecure ? iHost : NULL);
        AuthHeader(iHeaders, sizeof iHeaders - 40, EFalse);
        strcat(iHeaders, "Content-Type: application/json\r\n");
        snprintf(iPath, sizeof iPath, "%s/Users/AuthenticateByName", iBase);
        rsym_http_request req;
        FillRequest(req, "POST", iPath, iHeaders);
        req.body = iBody;
        req.body_len = strlen(iBody);
        rsym_log("login: %s as %s", iServer, iUser);
        StartL(req, ETagLogin);
        SetTitleL(_L("Signing in..."));
    }

    void StartL(const rsym_http_request &aReq, TInt aTag)
    {
        TInt err = iNet->Start(aReq, aTag);
        if (err != KErrNone) {
            TBuf<64> msg;
            msg.Format(_L("Could not start the request (%d)."), err);
            InfoL(msg);
        }
    }

    void LoadPageL(CPage &aPage, TInt aStart)
    {
        if (iNet->Busy())
            return;
        snprintf(iPagePath, sizeof iPagePath, "%s%sStartIndex=%d&Limit=100", aPage.iPath,
                 strchr(aPage.iPath, '?') ? "&" : "?", aStart);
        AuthHeader(iHeaders, sizeof iHeaders, ETrue);
        rsym_http_request req;
        FillRequest(req, "GET", iPagePath, iHeaders);
        rsym_log("get: %s", iPagePath);
        StartL(req, aStart ? ETagMore : ETagPage);
        SetTitleL(_L("Loading..."));
    }

    void NetDoneL(TInt aTag, const rsym_http_response &aResp, TInt aResult)
    {
        if (aTag == ETagImage) {
            ImageFetchedL(aResp, aResult);
            return;
        }
        CPage *page = Top();
        if (page)
            SetTitleL(page->iTitle);
        if (aResult != 0) {
            HBufC *e = TextLC(aResp.error);
            HBufC *msg = HBufC::NewLC(e->Length() + 80);
            msg->Des().Format(_L("Could not reach the server:\n%S"), e);
            InfoL(*msg);
            CleanupStack::PopAndDestroy(2, e);
            return;
        }
        if (aResp.status == 401) {
            rsym_log("server: 401");
            iToken[0] = 0;
            SaveSettings();
            InfoL(_L("The server did not accept the sign-in. Sign in again."));
            ShowSetupL();
            return;
        }
        if (aResp.status != 200 || !aResp.body) {
            char msg[100];
            snprintf(msg, sizeof msg, "The server answered HTTP %d.", aResp.status);
            rsym_log("server: %s", msg);
            InfoL(msg);
            return;
        }
        cJSON *root = cJSON_ParseWithLength((const char *)aResp.body, aResp.body_len);
        if (!root) {
            InfoL(_L("The server's answer could not be read."));
            return;
        }
        if (aTag == ETagLogin) {
            cJSON *user = cJSON_GetObjectItem(root, "User");
            Copy(iToken, sizeof iToken, JStr(root, "AccessToken"));
            Copy(iUserId, sizeof iUserId, JStr(user, "Id"));
            cJSON_Delete(root);
            rsym_log("login: ok (user id %s)", iUserId);
            SaveSettings();
            iPages.ResetAndDestroy();
            PushLibrariesL();
            return;
        }
        if (page && (page->iKind == EPageItems || page->iKind == EPageLibraries)) {
            ParseItems(*page, root);
            page->iLoaded = ETrue;
            rsym_log("page: %d of %d items", page->iItems.Count(), page->iTotal);
        }
        cJSON_Delete(root);
        RefreshL();
        StartImages();
        AutoStepL();
    }

    void ParseItems(CPage &aPage, cJSON *aRoot)
    {
        cJSON *items = cJSON_GetObjectItem(aRoot, "Items");
        int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
        aPage.iTotal = (TInt)JNum(aRoot, "TotalRecordCount");
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(items, i);
            TItem *t = new TItem;
            if (!t)
                break;
            Mem::FillZ(t, sizeof *t);
            Copy(t->iId, sizeof t->iId, JStr(it, "Id"));
            Copy(t->iName, sizeof t->iName, JStr(it, "Name"));
            Copy(t->iType, sizeof t->iType, JStr(it, "Type"));
            Copy(t->iCollection, sizeof t->iCollection, JStr(it, "CollectionType"));
            t->iFolder = cJSON_IsTrue(cJSON_GetObjectItem(it, "IsFolder"));
            t->iSeconds = (TInt)(JNum(it, "RunTimeTicks") / 10000000.0);
            const char *artist = JStr(it, "AlbumArtist");
            if (!artist[0]) {
                cJSON *artists = cJSON_GetObjectItem(it, "Artists");
                if (cJSON_GetArraySize(artists) > 0)
                    artist = cJSON_GetStringValue(cJSON_GetArrayItem(artists, 0));
            }
            Copy(t->iArtist, sizeof t->iArtist, artist);
            // artwork: the item's own, or (tracks) its album's
            if (cJSON_GetObjectItem(cJSON_GetObjectItem(it, "ImageTags"), "Primary"))
                Copy(t->iImageId, sizeof t->iImageId, t->iId);
            else if (JStr(it, "AlbumPrimaryImageTag")[0])
                Copy(t->iImageId, sizeof t->iImageId, JStr(it, "AlbumId"));
            Describe(*t, it);
            t->iIcon = IconFor(t->iImageId);
            if (aPage.iItems.Append(t) != KErrNone) {
                delete t;
                break;
            }
        }
    }

    // the second line of a row
    void Describe(TItem &aItem, cJSON *aJson)
    {
        int year = (int)JNum(aJson, "ProductionYear");
        int index = (int)JNum(aJson, "IndexNumber");
        char time[16] = "";
        if (aItem.iSeconds > 0)
            snprintf(time, sizeof time, "%d:%02d", aItem.iSeconds / 60, aItem.iSeconds % 60);
        const char *type = aItem.iType;
        if (!strcmp(type, "Audio")) {
            if (index > 0)
                snprintf(aItem.iSub, sizeof aItem.iSub, "%d. %s  %s", index, time, aItem.iArtist);
            else
                snprintf(aItem.iSub, sizeof aItem.iSub, "%s  %s", time, aItem.iArtist);
        } else if (!strcmp(type, "MusicAlbum")) {
            if (year > 0)
                snprintf(aItem.iSub, sizeof aItem.iSub, "%s, %d", aItem.iArtist, year);
            else
                Copy(aItem.iSub, sizeof aItem.iSub, aItem.iArtist);
        } else if (!strcmp(type, "MusicArtist")) {
            Copy(aItem.iSub, sizeof aItem.iSub, "Artist");
        } else if (!strcmp(type, "CollectionFolder") || !strcmp(type, "UserView")) {
            const char *c = aItem.iCollection;
            Copy(aItem.iSub, sizeof aItem.iSub,
                 !strcmp(c, "music") ? "Music library" : !strcmp(c, "movies") ? "Movies (video: not yet)" :
                 !strcmp(c, "tvshows") ? "TV shows (video: not yet)" : !strcmp(c, "playlists") ? "Playlists" :
                 "Library");
        } else if (!strcmp(type, "Episode")) {
            snprintf(aItem.iSub, sizeof aItem.iSub, "Episode %d  %s (video: not yet)", index, time);
        } else if (!strcmp(type, "Movie") || !strcmp(type, "Video") || !strcmp(type, "MusicVideo")) {
            snprintf(aItem.iSub, sizeof aItem.iSub, "%d  %s (video: not yet)", year, time);
        } else if (aItem.iFolder) {
            int children = (int)JNum(aJson, "ChildCount");
            if (children > 0)
                snprintf(aItem.iSub, sizeof aItem.iSub, "%s, %d items", type, children);
            else
                Copy(aItem.iSub, sizeof aItem.iSub, type);
        } else {
            Copy(aItem.iSub, sizeof aItem.iSub, type);
        }
    }

    // ---- pages ----

    CPage *Top() { return iPages.Count() ? iPages[iPages.Count() - 1] : NULL; }

    CPage *PushPageL(TPageKind aKind, const TDesC &aTitle)
    {
        if (CPage *top = Top())
            top->iCurrent = iList->CurrentIndex();
        iImageNext = 0;         // artwork for the new page from its start
        CPage *page = new (ELeave) CPage;
        page->iKind = aKind;
        page->iTitle.Copy(aTitle.Left(page->iTitle.MaxLength()));
        CleanupStack::PushL(page);
        iPages.AppendL(page);
        CleanupStack::Pop(page);
        SetTitleL(page->iTitle);
        RefreshL(aKind == EPageSettings || aKind == EPageSetup ? 0 : 1);
        return page;
    }

    void PushItemsL(const char *aTitle, TPageKind aKind = EPageItems)
    {
        HBufC *t = TextLC(aTitle);
        CPage *page = PushPageL(aKind, *t);
        CleanupStack::PopAndDestroy(t);
        Copy(page->iPath, sizeof page->iPath, iPath);
        LoadPageL(*page, 0);
    }

    void PushLibrariesL()
    {
        snprintf(iPath, sizeof iPath, "%s/Users/%s/Views", iBase, iUserId);
        PushItemsL("Libraries", EPageLibraries);
    }

    void PopPageL()
    {
        if (iPages.Count() <= 1)
            return;
        delete iPages[iPages.Count() - 1];
        iPages.Remove(iPages.Count() - 1);
        iImageNext = 0;
        CPage *page = Top();
        SetTitleL(page->iTitle);
        RefreshL(page->iCurrent);
        StartImages();
    }

    void ShowSetupL()
    {
        iPages.ResetAndDestroy();
        PushPageL(EPageSetup, _L("Sign in to Jellyfin"));
    }

    void SetTitleL(const TDesC &aTitle)
    {
        CAknTitlePane *title = static_cast<CAknTitlePane *>(
            StatusPane()->ControlL(TUid::Uid(EEikStatusPaneUidTitle)));
        title->SetTextL(aTitle);
    }

    TBool HasNowPlaying(const CPage &aPage)
    {
        return aPage.iKind == EPageLibraries || aPage.iKind == EPageMusic ||
               aPage.iKind == EPageItems;
    }

    void RefreshL(TInt aCurrent = -1)
    {
        CPage *page = Top();
        if (!page)
            return;
        if (aCurrent < 0)
            aCurrent = iList->CurrentIndex();
        iList->ResetL();
        if (HasNowPlaying(*page)) {
            TBuf<128> t;
            TBuf<200> v;
            NowPlayingTextL(t, v);
            iList->AddRowL(NowPlayingIcon(), t, v);
        }
        switch (page->iKind) {
        case EPageSetup:
            iList->AddRowL(0, _L("Server address"), iServer[0] ? iServer : "not set, e.g. http://192.168.1.10:8096");
            iList->AddRowL(0, _L("User name"), iUser[0] ? iUser : "not set");
            iList->AddRowL(0, _L("Accept any certificate"),
                           iInsecure ? "On (for a self-signed https:// server)" : "Off (default)");
            iList->AddRowL(0, _L("Sign in"), "asks for the password (it is not stored)");
            break;
        case EPageSettings: {
            char who[200];
            snprintf(who, sizeof who, "%s at %s (select to sign out)", iUser, iServer);
            iList->AddRowL(0, _L("Signed in"), who);
            TBuf<16> v;
            v.Format(_L("%d of 10"), iVolume);
            iList->AddRowL(0, _L("Volume"), v);
            iList->AddRowL(0, _L("Screen orientation"),
                           iOrient == 'L' ? "Landscape" : iOrient == 'P' ? "Portrait" : "Automatic (default)");
            iList->AddRowL(0, _L("Accept any certificate"),
                           iInsecure ? "On (for a self-signed https:// server)" : "Off (default)");
            iList->AddRowL(0, _L("Debug log"), rsym_log_enabled() ? "On" : "Off (default)");
            HBufC *path = SymbianPathLC(rsym_log_path());
            iList->AddRowL(0, _L("Debug log location"), *path);
            CleanupStack::PopAndDestroy(path);
            iList->AddRowL(0, _L("Clear debug log"), "delete the log file");
            char status[96];
            rsym_rlog_status(status, sizeof status);
            iList->AddRowL(0, _L("Remote debug log"), status);
            iList->AddRowL(0, _L("Remote debug host"),
                           rsym_rlog_host()[0] ? rsym_rlog_host() : "not set (IP address of the log server)");
            v.Zero();
            v.AppendNum(rsym_rlog_port());
            iList->AddRowL(0, _L("Remote debug port"), v);
            break;
        }
        case EPageMusic:
        case EPageLibraries:
        case EPageItems:
            for (TInt i = 0; i < page->iItems.Count(); i++) {
                TItem *t = page->iItems[i];
                iList->AddRowL(t->iIcon, t->iName, t->iSub);
            }
            if (page->iLoaded && page->iItems.Count() < page->iTotal) {
                char more[64];
                snprintf(more, sizeof more, "%d of %d shown", page->iItems.Count(), page->iTotal);
                iList->AddRowL(0, "Load more", more);
            } else if (page->iLoaded && !page->iItems.Count()) {
                iList->AddRowL(0, "Nothing here", "");
            }
            break;
        }
        iList->DoneL(aCurrent);
    }

    // ---- selecting ----

    void OpenItemL(TItem &aItem)
    {
        const char *type = aItem.iType;
        CPage *page = Top();
        char lib[40];
        Copy(lib, sizeof lib, aItem.iId);
        if (!strcmp(type, "Audio")) {
            PlayFromPageL(*page, aItem);
        } else if ((!strcmp(type, "CollectionFolder") || !strcmp(type, "UserView")) &&
                   !strcmp(aItem.iCollection, "music")) {
            // a music library: our own menu
            HBufC *t = TextLC(aItem.iName);
            CPage *music = PushPageL(EPageMusic, *t);
            CleanupStack::PopAndDestroy(t);
            AddMenuItem(*music, "Albums", "#albums", lib);
            AddMenuItem(*music, "Album artists", "#artists", lib);
            AddMenuItem(*music, "Songs", "#songs", lib);
            music->iLoaded = ETrue;
            music->iTotal = music->iItems.Count();
            RefreshL(1);
            AutoStepL();
        } else if (!strcmp(type, "#albums")) {
            snprintf(iPath, sizeof iPath, "%s/Items?userId=%s&ParentId=%s&IncludeItemTypes=MusicAlbum"
                     "&Recursive=true&SortBy=SortName%s", iBase, iUserId, aItem.iId, KFields);
            PushItemsL("Albums");
        } else if (!strcmp(type, "#artists")) {
            snprintf(iPath, sizeof iPath, "%s/Artists/AlbumArtists?userId=%s&ParentId=%s"
                     "&SortBy=SortName%s", iBase, iUserId, aItem.iId, KFields);
            PushItemsL("Album artists");
        } else if (!strcmp(type, "#songs")) {
            snprintf(iPath, sizeof iPath, "%s/Items?userId=%s&ParentId=%s&IncludeItemTypes=Audio"
                     "&Recursive=true&SortBy=SortName%s", iBase, iUserId, aItem.iId, KFields);
            PushItemsL("Songs");
        } else if (!strcmp(type, "MusicArtist")) {
            snprintf(iPath, sizeof iPath, "%s/Items?userId=%s&ArtistIds=%s&IncludeItemTypes=MusicAlbum"
                     "&Recursive=true&SortBy=ProductionYear,SortName%s", iBase, iUserId, aItem.iId, KFields);
            PushItemsL(aItem.iName);
        } else if (!strcmp(type, "MusicAlbum")) {
            snprintf(iPath, sizeof iPath, "%s/Items?userId=%s&ParentId=%s"
                     "&SortBy=ParentIndexNumber,IndexNumber,SortName%s", iBase, iUserId, aItem.iId, KFields);
            PushItemsL(aItem.iName);
        } else if (!strcmp(type, "Playlist")) {
            snprintf(iPath, sizeof iPath, "%s/Playlists/%s/Items?userId=%s%s", iBase, aItem.iId,
                     iUserId, KFields);
            PushItemsL(aItem.iName);
        } else if (aItem.iFolder) {
            snprintf(iPath, sizeof iPath, "%s/Items?userId=%s&ParentId=%s&SortBy=IsFolder,SortName%s",
                     iBase, iUserId, aItem.iId, KFields);
            PushItemsL(aItem.iName);
        } else {
            InfoL(_L("rJellyfin plays music for now; video comes later."));
        }
    }

    void AddMenuItem(CPage &aPage, const char *aName, const char *aType, const char *aLib)
    {
        TItem *t = new TItem;
        if (!t)
            return;
        Mem::FillZ(t, sizeof *t);
        Copy(t->iName, sizeof t->iName, aName);
        Copy(t->iType, sizeof t->iType, aType);
        Copy(t->iId, sizeof t->iId, aLib);
        t->iFolder = ETrue;
        if (aPage.iItems.Append(t) != KErrNone)
            delete t;
    }

    // ---- artwork ----

    // Icon index for an image id: 0 (placeholder) until it has arrived.
    TInt IconFor(const char *aImageId)
    {
        if (!aImageId[0])
            return 0;
        for (TInt i = 0; i < iImageIds.Count(); i++)
            if (!strcmp(iImageIds[i].iId, aImageId))
                return iImageIds[i].iIcon;
        return 0;
    }

    // Fetch the next missing image of the current page (one at a time).
    void StartImages()
    {
        if (iImageNet->Busy() || iImage->Busy())
            return;
        CPage *page = Top();
        if (!page || !HasNowPlaying(*page))
            return;
        for (; iImageNext < page->iItems.Count(); iImageNext++) {
            TItem *t = page->iItems[iImageNext];
            if (!t->iImageId[0] || IconFor(t->iImageId) || Failed(t->iImageId))
                continue;
            Copy(iImageWanted, sizeof iImageWanted, t->iImageId);
            TSize s = iList->IconSize();
            snprintf(iImagePath, sizeof iImagePath,
                     "%s/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=80",
                     iBase, t->iImageId, s.iWidth, s.iHeight);
            AuthHeader(iImageHeaders, sizeof iImageHeaders, ETrue);
            rsym_http_request req;
            FillRequest(req, "GET", iImagePath, iImageHeaders);
            if (iImageNet->Start(req, ETagImage) != KErrNone)
                return;
            return;
        }
    }

    TBool Failed(const char *aId)
    {
        for (TInt i = 0; i < iImageFailed.Count(); i++)
            if (!strcmp(iImageFailed[i].iId, aId))
                return ETrue;
        return EFalse;
    }

    void ImageFetchedL(const rsym_http_response &aResp, TInt aResult)
    {
        if (aResult != 0 || aResp.status != 200 || !aResp.body || aResp.body_len <= 0) {
            MarkFailed(iImageWanted);
            iImageNext++;
            StartImages();
            return;
        }
        TPtrC8 data(aResp.body, aResp.body_len);
        TRAPD(err, iImage->StartL(iEikonEnv->FsSession(), data));
        if (err != KErrNone) {
            rsym_log("image: decoder %d (%d bytes)", err, aResp.body_len);
            MarkFailed(iImageWanted);
            iImageNext++;
            StartImages();
        }
    }

    void MarkFailed(const char *aId)
    {
        TImageId f;
        Copy(f.iId, sizeof f.iId, aId);
        f.iIcon = 0;
        iImageFailed.Append(f);
    }

    void ImageDecodedL(CFbsBitmap *aBitmap, TInt aError)
    {
        if (!aBitmap) {
            rsym_log("image: decode failed %d", aError);
            MarkFailed(iImageWanted);
        } else {
            CArrayPtr<CGulIcon> *icons = iList->Icons();
            if (icons->Count() > 300) {
                // keep memory bounded: forget the artwork, it comes again
                iList->ResetIconsL();
                iImageIds.Reset();
                for (TInt p = 0; p < iPages.Count(); p++)
                    for (TInt i = 0; i < iPages[p]->iItems.Count(); i++)
                        iPages[p]->iItems[i]->iIcon = 0;
            }
            CGulIcon *icon = CGulIcon::NewL(aBitmap);
            icons->AppendL(icon);
            TImageId id;
            Copy(id.iId, sizeof id.iId, iImageWanted);
            id.iIcon = icons->Count() - 1;
            iImageIds.Append(id);
            // every row with this image (an album's tracks share one)
            CPage *page = Top();
            TInt off = page && HasNowPlaying(*page) ? 1 : 0;
            for (TInt i = 0; page && i < page->iItems.Count(); i++)
                if (!strcmp(page->iItems[i]->iImageId, iImageWanted)) {
                    page->iItems[i]->iIcon = id.iIcon;
                    iList->SetRowIconL(i + off, id.iIcon);
                }
            if (off && iQueue.Count() && !strcmp(iQueue[iQueuePos]->iImageId, iImageWanted))
                iList->SetRowIconL(0, id.iIcon);
        }
        iImageNext++;
        StartImages();
    }

    // ---- playing ----

    void PlayFromPageL(CPage &aPage, const TItem &aItem)
    {
        iQueue.ResetAndDestroy();
        TInt pos = 0;
        for (TInt i = 0; i < aPage.iItems.Count(); i++) {
            TItem *t = aPage.iItems[i];
            if (strcmp(t->iType, "Audio"))
                continue;
            if (t == &aItem)
                pos = iQueue.Count();
            TItem *copy = new (ELeave) TItem(*t);
            CleanupStack::PushL(copy);
            iQueue.AppendL(copy);
            CleanupStack::Pop(copy);
        }
        PlayQueueL(pos);
    }

    void PlayQueueL(TInt aPos)
    {
        if (aPos < 0 || aPos >= iQueue.Count()) {
            StopL();
            return;
        }
        iQueuePos = aPos;
        iAdvanced = EFalse;
        TItem *t = iQueue[aPos];
        // MP3 as it is, anything else converted to MP3 by the server
        snprintf(iStreamUrl, sizeof iStreamUrl,
                 "%s://%s%s%s%s/Audio/%s/universal?UserId=%s&DeviceId=%s&api_key=%s"
                 "&Container=mp3&AudioCodec=mp3&TranscodingContainer=mp3&TranscodingProtocol=http"
                 "&MaxStreamingBitrate=192000",
                 iTls ? "https" : "http", iHost, iPort ? ":" : "", iPort ? Num(iPort) : "",
                 iBase, t->iId, iUserId, iDeviceId, iToken);
        iAudio->Stop();
        rsym_log("play: %s (%d of %d)", t->iName, aPos + 1, iQueue.Count());
        RadioPlay(iStreamUrl);
        RefreshNowPlayingL();
    }

    const char *Num(TInt aN)
    {
        snprintf(iNum, sizeof iNum, "%d", aN);
        return iNum;
    }

    void StopL()
    {
        iAudio->Stop();
        RadioStop();
        RefreshNowPlayingL();
    }

    void NextL(TInt aDelta)
    {
        if (!iQueue.Count())
            return;
        TInt pos = iQueuePos + aDelta;
        if (pos < 0)
            pos = 0;
        if (pos >= iQueue.Count()) {
            rsym_log("play: end of the queue");
            StopL();
            return;
        }
        PlayQueueL(pos);
    }

    void NowPlayingSelectedL()
    {
        TRadioInfo info;
        RadioInfo(info);
        if (info.iState == ERadioStopped || info.iState == ERadioFailed ||
            info.iState == ERadioFinished) {
            if (iQueue.Count())
                PlayQueueL(iQueuePos);
        } else {
            StopL();
        }
    }

    void NowPlayingTextL(TDes &aTitle, TDes &aValue)
    {
        TRadioInfo info;
        RadioInfo(info);
        TItem *t = iQueue.Count() ? iQueue[iQueuePos] : NULL;
        HBufC *name = TextLC(t ? t->iName : "");
        aTitle.Zero();
        aValue.Zero();
        switch (info.iState) {
        case ERadioStopped:
        case ERadioFinished:
            aTitle.Copy(_L("Not playing"));
            aValue.Copy(t ? _L("Select to play the last track again") : _L("Pick an album or a song"));
            break;
        case ERadioConnecting:
        case ERadioBuffering:
            aTitle.Copy(_L("Loading: "));
            aTitle.Append(name->Left(aTitle.MaxLength() - aTitle.Length()));
            aValue.Copy(_L("Select to stop"));
            break;
        case ERadioPlaying: {
            aTitle.Copy(_L("Playing: "));     // (the phone's font has no play symbol)
            aTitle.Append(name->Left(aTitle.MaxLength() - aTitle.Length()));
            TInt s = info.iPlayedMs / 1000;
            aValue.AppendFormat(_L("%d:%02d"), s / 60, s % 60);
            if (t && t->iSeconds > 0)
                aValue.AppendFormat(_L(" / %d:%02d"), t->iSeconds / 60, t->iSeconds % 60);
            if (t && t->iArtist[0]) {
                HBufC *a = TextLC(t->iArtist);
                aValue.Append(_L(" \x2022 "));
                aValue.Append(a->Left(60));
                CleanupStack::PopAndDestroy(a);
            }
            aValue.AppendFormat(_L(" \x2022 vol %d"), iVolume);
            if (iQueue.Count() > 1)
                aValue.AppendFormat(_L(" \x2022 %d/%d"), iQueuePos + 1, iQueue.Count());
            break;
        }
        case ERadioFailed: {
            aTitle.Copy(_L("Could not play: "));
            aTitle.Append(name->Left(aTitle.MaxLength() - aTitle.Length()));
            HBufC *e = TextLC(info.iError);
            aValue.Append(e->Left(aValue.MaxLength()));
            CleanupStack::PopAndDestroy(e);
            break;
        }
        }
        CleanupStack::PopAndDestroy(name);
    }

    void RefreshNowPlayingL()
    {
        CPage *page = Top();
        if (!page || !HasNowPlaying(*page))
            return;
        TBuf<128> t;
        TBuf<200> v;
        NowPlayingTextL(t, v);
        iList->SetRowL(0, NowPlayingIcon(), t, v);
    }

    // the playing track's artwork, once it has arrived
    TInt NowPlayingIcon()
    {
        return iQueue.Count() ? IconFor(iQueue[iQueuePos]->iImageId) : 0;
    }

    static TInt Tick(TAny *aSelf)
    {
        CJfAppUi *self = static_cast<CJfAppUi *>(aSelf);
        TRadioInfo info;
        RadioInfo(info);
        if (info.iState == ERadioPlaying)
            TRAP_IGNORE(self->RefreshNowPlayingL());
        return 0;
    }

    // MRadioNews
    void RadioNewsL()
    {
        TRadioInfo info;
        RadioInfo(info);
        if (info.iState != iShownState)
            rsym_log("news: state %d", info.iState);
        if (info.iState == ERadioPlaying) {
            if (!iAudio->Running())
                iAudio->StartL(iVolume);
            iAudio->Feed();
        } else if (info.iState == ERadioStopped || info.iState == ERadioFailed) {
            iAudio->Stop();
        } else if (info.iState == ERadioFinished && !iAudio->Running()) {
            AdvanceL();         // nothing left in the output: next track now
        }
        if (info.iState != iShownState) {
            iShownState = info.iState;
            RefreshNowPlayingL();
        }
    }

    // MAudioOutObserver: the output has played everything it had
    void AudioDrainedL()
    {
        TRadioInfo info;
        RadioInfo(info);
        if (info.iState == ERadioFinished)
            AdvanceL();
    }

    void AdvanceL()
    {
        if (iAdvanced)
            return;
        iAdvanced = ETrue;
        rsym_log("play: track ended");
        NextL(1);
    }

    // ---- settings ----

    void SetupItemL(TInt aIndex)
    {
        switch (aIndex) {
        case 0:
            if (QueryUtf8L(_L("Server address, e.g. http://192.168.1.10:8096"), iServer, sizeof iServer)) {
                if (iServer[0] && !strstr(iServer, "://")) {
                    char s[200];
                    snprintf(s, sizeof s, "http://%s", iServer);
                    Copy(iServer, sizeof iServer, s);
                }
                ParseServer();
                SaveSettings();
            }
            break;
        case 1:
            if (QueryUtf8L(_L("User name"), iUser, sizeof iUser))
                SaveSettings();
            break;
        case 2:
            iInsecure = !iInsecure;
            SaveSettings();
            break;
        case 3:
            SignInL();
            break;
        }
        RefreshL();
    }

    TBool EditRemoteHostL()
    {
        char host[64];
        Copy(host, sizeof host, rsym_rlog_host());
        if (!QueryUtf8L(_L("Remote debug host (log server IP address)"), host, sizeof host))
            return EFalse;
        rsym_rlog_configure(rsym_rlog_enabled(), host, 0);
        return host[0] != 0;
    }

    void SettingsItemL(TInt aIndex)
    {
        switch (aIndex) {
        case 0:
            if (AskYesNoL(_L("Sign out of this server?"))) {
                StopL();
                iQueue.ResetAndDestroy();
                iToken[0] = 0;
                iUserId[0] = 0;
                SaveSettings();
                ShowSetupL();
                return;
            }
            break;
        case 1:
            VolumeL(iVolume >= 10 ? -10 : 1);
            break;
        case 2:
            iOrient = iOrient == 'L' ? 'P' : iOrient == 'P' ? 'A' : 'L';
            SetOrientationL(iOrient == 'L' ? EAppUiOrientationLandscape :
                            iOrient == 'P' ? EAppUiOrientationPortrait : EAppUiOrientationAutomatic);
            SaveSettings();
            break;
        case 3:
            iInsecure = !iInsecure;
            rsym_https_allow_untrusted(iInsecure ? iHost : NULL);
            SaveSettings();
            break;
        case 4:
            rsym_log_set(!rsym_log_enabled());
            break;
        case 5: {
            HBufC *path = SymbianPathLC(rsym_log_path());
            HBufC *msg = HBufC::NewLC(path->Length() + 40);
            msg->Des().Format(_L("Debug log:\n%S"), path);
            InfoL(*msg);
            CleanupStack::PopAndDestroy(2, path);
            break;
        }
        case 6:
            if (AskYesNoL(_L("Delete the debug log?")))
                rsym_log_clear();
            break;
        case 7: {
            TBool on = !rsym_rlog_enabled();
            if (on && !rsym_rlog_host()[0] && !EditRemoteHostL())
                break;
            rsym_rlog_configure(on, NULL, 0);
            break;
        }
        case 8:
            EditRemoteHostL();
            break;
        case 9: {
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
        }
        RefreshL();
    }

    // ---- autotest ----

    // play=1: first music library > Albums > first album > first track
    void AutoStepL()
    {
        if (!iAutoStep)
            return;
        CPage *page = Top();
        if (!page)
            return;
        if (iAutoStep == 1 && page->iKind == EPageLibraries) {
            for (TInt i = 0; i < page->iItems.Count(); i++)
                if (!strcmp(page->iItems[i]->iCollection, "music")) {
                    iAutoStep = 2;
                    OpenItemL(*page->iItems[i]);
                    return;
                }
            rsym_log("autotest: no music library");
            iAutoStep = 0;
        } else if (iAutoStep == 2 && page->iKind == EPageMusic && page->iItems.Count()) {
            iAutoStep = 3;
            OpenItemL(*page->iItems[0]);                // Albums
        } else if (iAutoStep == 3 && page->iKind == EPageItems && page->iItems.Count()) {
            iAutoStep = 4;
            OpenItemL(*page->iItems[0]);                // the first album
        } else if (iAutoStep == 4 && page->iKind == EPageItems && page->iItems.Count()) {
            iAutoStep = 0;
            rsym_log("autotest: play the album");
            OpenItemL(*page->iItems[0]);                // its first track
        }
    }

    // ---- menu and keys ----

    TItem *SelectedItem()
    {
        CPage *page = Top();
        if (!page || !HasNowPlaying(*page))
            return NULL;
        TInt i = iList->CurrentIndex() - 1;
        return i >= 0 && i < page->iItems.Count() ? page->iItems[i] : NULL;
    }

    void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenu)
    {
        if (aResourceId != R_JF_MENU)
            return;
        TRadioInfo info;
        RadioInfo(info);
        TBool active = info.iState == ERadioConnecting || info.iState == ERadioBuffering ||
                       info.iState == ERadioPlaying;
        TItem *t = SelectedItem();
        TBool audio = t && !strcmp(t->iType, "Audio");
        CPage *page = Top();
        TBool signedIn = iToken[0] != 0;
        aMenu->SetItemDimmed(EJfCmdOpen, !t || audio);
        aMenu->SetItemDimmed(EJfCmdPlay, !audio);
        aMenu->SetItemDimmed(EJfCmdStop, !active);
        aMenu->SetItemDimmed(EJfCmdNext, !iQueue.Count() || iQueuePos + 1 >= iQueue.Count());
        aMenu->SetItemDimmed(EJfCmdPrevious, !iQueue.Count() || iQueuePos == 0);
        aMenu->SetItemDimmed(EJfCmdHome, !signedIn || (page && page->iKind == EPageLibraries));
        aMenu->SetItemDimmed(EJfCmdSettings, !signedIn || (page && page->iKind == EPageSettings));
    }

    void HandleCommandL(TInt aCommand)
    {
        switch (aCommand) {
        case EJfCmdOpen:
        case EJfCmdPlay:
            if (TItem *t = SelectedItem())
                OpenItemL(*t);
            break;
        case EJfCmdStop:     StopL(); break;
        case EJfCmdNext:     NextL(1); break;
        case EJfCmdPrevious: NextL(-1); break;
        case EJfCmdVolumeUp:   VolumeL(1); break;
        case EJfCmdVolumeDown: VolumeL(-1); break;
        case EJfCmdHome:
            while (iPages.Count() > 1)
                PopPageL();
            break;
        case EJfCmdSettings:
            PushPageL(EPageSettings, _L("Settings"));
            break;
        case EJfCmdAbout:
            InfoL(_L("rJellyfin " KVersion " (experimental)\n"
                     "A Jellyfin client: music for now\n"
                     "Developer: RuhanSA079\ngithub.com/RuhanSA079/SymbianApps\n"
                     "MP3 decoding: minimp3 by lieff (CC0)\n"
                     "Not affiliated with the Jellyfin project"));
            break;
        case EAknSoftkeyBack:
            if (iPages.Count() > 1)
                PopPageL();
            else if (AskYesNoL(_L("Exit rJellyfin?")))
                Exit();
            break;
        case EJfCmdExit:
        case EAknSoftkeyExit:
        case EAknCmdExit:
        case EEikCmdExit:
            Exit();
            break;
        default:
            break;
        }
    }

    void MrccatoCommand(TRemConCoreApiOperationId aOperationId,
                        TRemConCoreApiButtonAction aButtonAct)
    {
        if (aButtonAct == ERemConCoreApiButtonRelease)
            return;
        switch (aOperationId) {
        case ERemConCoreApiVolumeUp:   TRAP_IGNORE(VolumeL(1)); break;
        case ERemConCoreApiVolumeDown: TRAP_IGNORE(VolumeL(-1)); break;
        case ERemConCoreApiForward:    TRAP_IGNORE(NextL(1)); break;
        case ERemConCoreApiBackward:   TRAP_IGNORE(NextL(-1)); break;
        case ERemConCoreApiPausePlayFunction:
        case ERemConCoreApiStop:       TRAP_IGNORE(NowPlayingSelectedL()); break;
        default: break;
        }
    }

    void HandleResourceChangeL(TInt aType)
    {
        CAknAppUi::HandleResourceChangeL(aType);
        if (aType == KEikDynamicLayoutVariantSwitch && iList)
            iList->SetRect(ClientRect());
    }

    // EKA2L1 workaround, as in the other apps: letters arrive upper-case
    // without Shift. Harmless on a phone.
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

    friend class CJfList;

    struct TImageId
        {
        char iId[40];
        TInt iIcon;
        };

    static const char KFields[];

    CJfList *iList;
    CAudioOut *iAudio;
    CRadioNotify *iNotify;
    CNetTask *iNet, *iImageNet;
    CImageLoader *iImage;
    CPeriodic *iTick;
    CRemConInterfaceSelector *iRemCon;
    CRemConCoreApiTarget *iRemConTarget;
    RPointerArray<CPage> iPages;
    RPointerArray<TItem> iQueue;
    TInt iQueuePos;
    TBool iAdvanced;
    TRadioState iShownState;
    RArray<TImageId> iImageIds, iImageFailed;
    TInt iImageNext;
    char iImageWanted[40];
    char iImagePath[300];
    char iImageHeaders[400];
    // server and sign-in
    char iServer[200], iHost[128], iBase[128];
    TBool iTls;
    TInt iPort;
    char iUser[96], iUserId[40], iToken[64], iDeviceId[40];
    char iAutoPassword[96], iTypedPassword[96];
    TBool iInsecure;
    TInt iAutoStep;
    // request buffers (alive until the request is done)
    char iPath[700], iPagePath[760], iHeaders[400], iBody[300];
    char iStreamUrl[700];
    char iNum[12];
    TInt iVolume;
    char iOrient;
    TBool iShiftDown;
    char *iCa;
    char iPrivDir[64];
    char iInstallDrive;
};

const char CJfAppUi::KFields[] =
    "&Fields=PrimaryImageAspectRatio,ChildCount&EnableImageTypes=Primary"
    "&ImageTypeLimit=1&EnableUserData=false";

void CJfAppUi::SelectL(TInt aRow)
{
    CPage *page = Top();
    if (!page)
        return;
    page->iCurrent = aRow;
    if (page->iKind == EPageSetup) {
        SetupItemL(aRow);
        return;
    }
    if (page->iKind == EPageSettings) {
        SettingsItemL(aRow);
        return;
    }
    if (aRow == 0) {
        NowPlayingSelectedL();
        return;
    }
    TInt i = aRow - 1;
    if (i < page->iItems.Count())
        OpenItemL(*page->iItems[i]);
    else if (page->iItems.Count() < page->iTotal)
        LoadPageL(*page, page->iItems.Count());        // "Load more"
}

void CJfAppUi::VolumeL(TInt aDelta)
{
    TInt v = iVolume + aDelta;
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    if (v == iVolume)
        return;
    iVolume = v;
    iAudio->SetVolume(iVolume);
    SaveSettings();
    RefreshNowPlayingL();
    if (Top() && Top()->iKind == EPageSettings)
        RefreshL();
}

// ---------------------------------------------------------------------------

TKeyResponse CJfList::OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType)
{
    if (aType == EEventKey && gAppUi) {
        if (aKey.iCode == EKeyRightArrow) {
            gAppUi->VolumeL(1);
            return EKeyWasConsumed;
        }
        if (aKey.iCode == EKeyLeftArrow) {
            gAppUi->VolumeL(-1);
            return EKeyWasConsumed;
        }
    }
    return iListBox->OfferKeyEventL(aKey, aType);
}

void CJfList::HandleListBoxEventL(CEikListBox *, TListBoxEvent aEvent)
{
    if ((aEvent == EEventEnterKeyPressed || aEvent == EEventItemSingleClicked ||
         aEvent == EEventItemDoubleClicked) && gAppUi)
        gAppUi->SelectL(iListBox->CurrentItemIndex());
}

class CJfDocument : public CAknDocument
{
public:
    CJfDocument(CEikApplication &aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi *CreateAppUiL() { return new (ELeave) CJfAppUi; }
};

class CJfApplication : public CAknApplication
{
private:
    TUid AppDllUid() const { return KUidJellyfin; }
    CApaDocument *CreateDocumentL() { return new (ELeave) CJfDocument(*this); }
};

LOCAL_C CApaApplication *NewApplication() { return new CJfApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
