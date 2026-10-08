// rInternetRadio: internet radio for Symbian^3.
//
// - One list, in three modes: Favourites (the start screen), search results
//   from the radio-browser.info directory, and Settings. Its first row is
//   always "now playing"; selecting it stops (or plays again).
// - radio_engine.cpp fetches and decodes the stream on a worker thread;
//   CAudioOut plays its PCM through CMdaAudioOutputStream on this thread.
// - Directory searches run on a worker thread too (CNetTask, as in rDrive);
//   the JSON is parsed here with cJSON.
// - Volume: left/right keys, the phone's volume keys (RemCon) or Options.

#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <akntitle.h>
#include <aknlists.h>
#include <aknquerydialog.h>
#include <avkon.hrh>
#include <avkon.rsg>
#include <badesca.h>
#include <eikenv.h>
#include <eiklbo.h>
#include <eikmenup.h>
#include <eikspane.h>
#include <eikstart.h>
#include <coecntrl.h>
#include <f32file.h>
#include <utf.h>
#include <remconcoreapitarget.h>
#include <remconcoreapitargetobserver.h>
#include <remconinterfaceselector.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rinternetradio.rsg>
#include "rinternetradio.hrh"
#include "radio_engine.h"
#include "audio_out.h"
#include "rsym_https.h"
#include "rsym_log.h"
#include "rsym_rlog.h"
#include "cJSON.h"

const TUid KUidRadio = { TInt32(0xE5A1E040) };

class CRadioAppUi;
static CRadioAppUi *gAppUi;

// ---------------------------------------------------------------------------
// helpers

static TBool ValidUtf8(const char *s, TInt aLen)
{
    for (TInt i = 0; i < aLen; ) {
        TUint8 c = s[i];
        TInt n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 :
                 (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= aLen + (n ? 0 : 1))
            return EFalse;
        for (TInt k = 1; k <= n; k++)
            if ((TUint8(s[i + k]) & 0xC0) != 0x80)
                return EFalse;
        i += n + 1;
    }
    return ETrue;
}

// UTF-8, or Latin-1 when it is not valid UTF-8 (common in ICY titles).
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

static TBool AskYesNoL(const TDesC &aPrompt)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    return dlg->ExecuteLD(R_RADIO_YESNO_QUERY, aPrompt) != 0;
}

static void InfoL(const TDesC &aText)
{
    CAknQueryDialog *dlg = CAknQueryDialog::NewL();
    dlg->ExecuteLD(R_RADIO_OK_QUERY, aText);
}

static TBool QueryTextL(const TDesC &aPrompt, TDes &aText)
{
    CAknTextQueryDialog *dlg = CAknTextQueryDialog::NewL(aText);
    dlg->SetPromptL(aPrompt);
    dlg->SetPredictiveTextInputPermitted(EFalse);
    return dlg->ExecuteLD(R_RADIO_TEXT_QUERY) != 0;
}

// "C:/private/x/file" -> "C:\private\x\file"
static HBufC *SymbianPathLC(const char *aPath)
{
    HBufC *p = TextLC(aPath);
    TPtr ptr = p->Des();
    for (TInt i = 0; i < ptr.Length(); i++)
        if (ptr[i] == '/')
            ptr[i] = '\\';
    return p;
}

static void Copy(char *aDst, int aSize, const char *aSrc)
{
    strncpy(aDst, aSrc ? aSrc : "", aSize - 1);
    aDst[aSize - 1] = 0;
}

// ---------------------------------------------------------------------------
// a station

struct TStation
    {
    char iName[96];
    char iUrl[512];
    char iInfo[96];         // "MP3 128 kbps, Germany"
    };

// ---------------------------------------------------------------------------
// the list (all three screens)

class CRadioList : public CCoeControl, public MEikListBoxObserver
{
public:
    static CRadioList *NewL(const TRect &aRect)
    {
        CRadioList *self = new (ELeave) CRadioList;
        CleanupStack::PushL(self);
        self->ConstructL(aRect);
        CleanupStack::Pop(self);
        return self;
    }
    ~CRadioList()
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
        // tabs separate the columns
        for (TInt i = 0; i < aTitle.Length(); i++)
            p.Append(aTitle[i] == '\t' ? ' ' : aTitle[i]);
        p.Append('\t');
        for (TInt i = 0; i < aValue.Length(); i++)
            p.Append(aValue[i] == '\t' ? ' ' : aValue[i]);
        iItems->AppendL(*row);
        CleanupStack::PopAndDestroy(row);
    }
    void AddRowL(const TDesC &aTitle, const char *aValue)
    {
        HBufC *v = TextLC(aValue);
        AddRowL(aTitle, *v);
        CleanupStack::PopAndDestroy(v);
    }
    void AddRowL(const char *aTitle, const char *aValue)
    {
        HBufC *t = TextLC(aTitle);
        AddRowL(*t, aValue);
        CleanupStack::PopAndDestroy(t);
    }
    // Replace one row (the now-playing row) without resetting the view.
    void SetRowL(TInt aIndex, const TDesC &aTitle, const TDesC &aValue)
    {
        if (aIndex >= iItems->Count())
            return;
        TInt count = iItems->Count();
        AddRowL(aTitle, aValue);                // appended last ...
        TPtrC row = (*iItems)[count];
        HBufC *copy = row.AllocLC();
        iItems->Delete(count);
        iItems->Delete(aIndex);
        iItems->InsertL(aIndex, *copy);         // ... and moved into place
        CleanupStack::PopAndDestroy(copy);
        iListBox->DrawItem(aIndex);
    }
    void DoneL(TInt aCurrent = -1)
    {
        TInt current = aCurrent >= 0 ? aCurrent : iListBox->CurrentItemIndex();
        iListBox->HandleItemAdditionL();
        if (current < 0)
            current = 0;
        if (current >= iItems->Count())
            current = iItems->Count() - 1;
        iListBox->SetCurrentItemIndex(current);
        iListBox->DrawDeferred();
    }
    TInt CurrentIndex() const { return iListBox->CurrentItemIndex(); }

    TKeyResponse OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType);
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
// one directory request on a worker thread (as in rDrive)

class MNetObserver
{
public:
    virtual void NetDoneL(const rsym_http_response &aResp, TInt aResult) = 0;
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
        Mem::FillZ(&iJob.iResp, sizeof iJob.iResp);
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
            ui.RequestComplete(job->iStatus, KErrNone);
            ui.Close();
        }
        delete cleanup;
        return 0;
    }
    void RunL()
    {
        iThread.Close();
        iObserver.NetDoneL(iJob.iResp, iJob.iResult);
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

class CRadioAppUi : public CAknAppUi, public MRadioNews, public MNetObserver,
                    public MRemConCoreApiTargetObserver
{
public:
    void ConstructL()
    {
        gAppUi = this;
        InitPrivateDirL();
        LoadSettings();         // before BaseConstructL: the orientation
        BaseConstructL(EAknEnableSkin |
                       (iOrient == EOrientLandscape ? EAppOrientationLandscape :
                        iOrient == EOrientPortrait ? EAppOrientationPortrait :
                        EAppOrientationAutomatic));
        LoadCa();
        User::LeaveIfError(RadioInit());
        LoadFavouritesL();
        iAudio = new (ELeave) CAudioOut;
        iAudio->ConstructL();
        iNotify = new (ELeave) CRadioNotify(*this);
        iNet = new (ELeave) CNetTask(*this);
        iList = CRadioList::NewL(ClientRect());
        AddToStackL(iList);
        // The phone's volume keys (and headset buttons). Not in every
        // environment (EKA2L1), so failure is fine.
        TRAPD(err,
            iRemCon = CRemConInterfaceSelector::NewL();
            iRemConTarget = CRemConCoreApiTarget::NewL(*iRemCon, *this);
            iRemCon->OpenTargetL();
        );
        rsym_log("remcon: %d", err);
        ShowFavouritesL();

        // Test hook (emulator diagnostics): C:\Data\rinternetradio-autotest.txt
        // with a stream URL on its first line plays it at start; with
        // "search:<name>", searches the directory for it.
        FILE *fp = fopen("C:\\Data\\rinternetradio-autotest.txt", "r");
        if (fp) {
            char url[512];
            if (fgets(url, sizeof url, fp)) {
                url[strcspn(url, "\r\n")] = 0;
                if (!strncmp(url, "search:", 7)) {
                    rsym_log("autotest: search %s", url + 7);
                    TPtrC8 name((const TUint8 *)url + 7, strlen(url + 7));
                    iLastSearch.Copy(name.Left(iLastSearch.MaxLength()));
                    StartSearchL(iLastSearch);
                } else if (url[0]) {
                    rsym_log("autotest: play %s", url);
                    TStation s;
                    Copy(s.iName, sizeof s.iName, "Autotest");
                    Copy(s.iUrl, sizeof s.iUrl, url);
                    s.iInfo[0] = 0;
                    PlayL(s);
                }
            }
            fclose(fp);
        }
    }

    ~CRadioAppUi()
    {
        RadioStop();
        delete iNotify;
        delete iAudio;
        delete iNet;
        delete iRemCon;             // owns iRemConTarget
        if (iList) {
            RemoveFromStack(iList);
            delete iList;
        }
        iFavourites.ResetAndDestroy();
        iResults.ResetAndDestroy();
        gAppUi = NULL;
        rsym_log("exit");
        rsym_rlog_flush(1000);
        // EKA2L1 hangs in the framework teardown after this; our clean-up is
        // done, so end the process (as rSSH does). The stream worker may
        // still be running; ending the process ends it too.
        User::Exit(KErrNone);
    }

    // From the list: a row was selected.
    void SelectL(TInt aIndex)
    {
        if (aIndex == 0) {
            NowPlayingSelectedL();
            return;
        }
        switch (iMode) {
        case EFavourites:
            if (aIndex == 1)
                SearchL();
            else if (aIndex == 2)
                TopL();
            else if (aIndex - KFavFirst < iFavourites.Count())
                PlayL(*iFavourites[aIndex - KFavFirst]);
            break;
        case EResults:
            if (aIndex - 1 < iResults.Count())
                PlayL(*iResults[aIndex - 1]);
            break;
        case ESettings:
            SettingsItemL(aIndex);
            break;
        }
    }

    void VolumeL(TInt aDelta)
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
        if (iMode == ESettings)
            RefreshL();
    }

private:
    enum TMode { EFavourites, EResults, ESettings };
    enum TOrient { EOrientLandscape = 'L', EOrientPortrait = 'P', EOrientAuto = 'A' };
    enum { KFavFirst = 3 };         // rows: now playing, search, top, favourites...
    enum { ESetVolume = 1, ESetOrient, ESetDebug, ESetLogPath, ESetClear,
           ESetRemote, ESetRemoteHost, ESetRemotePort };

    // ---- start-up ----

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
        rsym_log_init(iPrivDir, "rinternetradio");
        // the installed files (ca-bundle) are on the drive of the program
        TFileName exe = RProcess().FileName();
        iInstallDrive = exe.Length() > 0 ? char(exe[0]) : 'C';
    }

    // Mozilla's root certificates (NetSurf's copy), for https:// streams.
    void LoadCa()
    {
        char path[64];
        snprintf(path, sizeof path, "%c:/private/e5a1e040/ca-bundle", iInstallDrive);
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
            rsym_log("ca: %ld bytes", n);
        }
        fclose(fp);
    }

    void PrivPath(char *aBuf, int aLen, const char *aName)
    {
        snprintf(aBuf, aLen, "%s/%s", iPrivDir, aName);
    }

    void LoadSettings()
    {
        iVolume = 7;
        iOrient = EOrientAuto;
        char path[96];
        PrivPath(path, sizeof path, "radio.cfg");
        FILE *fp = fopen(path, "r");
        if (!fp)
            return;
        char line[600];
        while (fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "volume=", 7)) {
                iVolume = atoi(line + 7);
                if (iVolume < 0 || iVolume > 10)
                    iVolume = 7;
            } else if (!strncmp(line, "orientation=", 12)) {
                char c = line[12];
                if (c == EOrientLandscape || c == EOrientPortrait || c == EOrientAuto)
                    iOrient = static_cast<TOrient>(c);
            } else if (!strncmp(line, "last-name=", 10)) {
                Copy(iLast.iName, sizeof iLast.iName, line + 10);
            } else if (!strncmp(line, "last-url=", 9)) {
                Copy(iLast.iUrl, sizeof iLast.iUrl, line + 9);
            }
        }
        fclose(fp);
    }

    void SaveSettings()
    {
        char path[96];
        PrivPath(path, sizeof path, "radio.cfg");
        FILE *fp = fopen(path, "w");
        if (!fp)
            return;
        fprintf(fp, "volume=%d\norientation=%c\nlast-name=%s\nlast-url=%s\n",
                iVolume, (char)iOrient, iLast.iName, iLast.iUrl);
        fclose(fp);
    }

    // favourites.txt: "name<TAB>url<TAB>info" per line.
    void LoadFavouritesL()
    {
        char path[96];
        PrivPath(path, sizeof path, "favourites.txt");
        FILE *fp = fopen(path, "r");
        if (!fp) {
            // first run: a few long-standing SomaFM channels (MP3, 128 kbps)
            AddPresetL("SomaFM: Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3",
                       "MP3 128 kbps, ambient/downtempo");
            AddPresetL("SomaFM: Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3",
                       "MP3 128 kbps, ambient");
            AddPresetL("SomaFM: Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3",
                       "MP3 128 kbps, lounge");
            AddPresetL("SomaFM: Indie Pop Rocks!", "http://ice1.somafm.com/indiepop-128-mp3",
                       "MP3 128 kbps, indie pop");
            SaveFavourites();
            return;
        }
        char line[800];
        while (fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = 0;
            char *t1 = strchr(line, '\t');
            if (!t1)
                continue;
            *t1++ = 0;
            char *t2 = strchr(t1, '\t');
            if (t2)
                *t2++ = 0;
            AddPresetL(line, t1, t2 ? t2 : "");
        }
        fclose(fp);
    }

    void AddPresetL(const char *aName, const char *aUrl, const char *aInfo)
    {
        TStation *s = new (ELeave) TStation;
        Copy(s->iName, sizeof s->iName, aName);
        Copy(s->iUrl, sizeof s->iUrl, aUrl);
        Copy(s->iInfo, sizeof s->iInfo, aInfo);
        TInt err = iFavourites.Append(s);
        if (err != KErrNone) {
            delete s;
            User::Leave(err);
        }
    }

    void SaveFavourites()
    {
        char path[96];
        PrivPath(path, sizeof path, "favourites.txt");
        FILE *fp = fopen(path, "w");
        if (!fp)
            return;
        for (TInt i = 0; i < iFavourites.Count(); i++)
            fprintf(fp, "%s\t%s\t%s\n", iFavourites[i]->iName, iFavourites[i]->iUrl,
                    iFavourites[i]->iInfo);
        fclose(fp);
    }

    TBool IsFavourite(const char *aUrl)
    {
        for (TInt i = 0; i < iFavourites.Count(); i++)
            if (!strcmp(iFavourites[i]->iUrl, aUrl))
                return ETrue;
        return EFalse;
    }

    // ---- screens ----

    void SetTitleL(const TDesC &aTitle)
    {
        CAknTitlePane *title = static_cast<CAknTitlePane *>(
            StatusPane()->ControlL(TUid::Uid(EEikStatusPaneUidTitle)));
        title->SetTextL(aTitle);
    }

    void ShowFavouritesL()
    {
        iMode = EFavourites;
        SetTitleL(_L("rInternetRadio"));
        RefreshL(0);
    }

    void ShowResultsL(const TDesC &aTitle)
    {
        iMode = EResults;
        SetTitleL(aTitle);
        RefreshL(1);
    }

    void ShowSettingsL()
    {
        iMode = ESettings;
        SetTitleL(_L("Settings"));
        RefreshL(1);
    }

    void NowPlayingTextL(TDes &aTitle, TDes &aValue)
    {
        TRadioInfo info;
        RadioInfo(info);
        HBufC *name = TextLC(iPlaying.iName[0] ? iPlaying.iName : info.iName);
        aTitle.Zero();
        aValue.Zero();
        switch (info.iState) {
        case ERadioStopped:
            aTitle.Copy(_L("Stopped"));
            if (iLast.iUrl[0]) {
                HBufC *last = TextLC(iLast.iName);
                aValue.Append(_L("Select to play "));
                aValue.Append(last->Left(aValue.MaxLength() - aValue.Length()));
                CleanupStack::PopAndDestroy(last);
            } else {
                aValue.Copy(_L("Select a station to play"));
            }
            break;
        case ERadioConnecting:
            aTitle.Copy(_L("Connecting: "));
            aTitle.Append(name->Left(aTitle.MaxLength() - aTitle.Length()));
            aValue.Copy(_L("Select to stop"));
            break;
        case ERadioBuffering: {
            TInt pct = info.iBufferedMs * 100 / 1000;
            aTitle.Format(_L("Buffering %d%%: "), pct > 100 ? 100 : pct);
            aTitle.Append(name->Left(aTitle.MaxLength() - aTitle.Length()));
            if (info.iUnderruns)
                aValue.Format(_L("The stream fell behind %d times"), info.iUnderruns);
            else
                aValue.Copy(_L("Select to stop"));
            break;
        }
        case ERadioPlaying: {
            aTitle.Copy(_L("Playing: "));       // (the phone's font has no play symbol)
            aTitle.Append(name->Left(aTitle.MaxLength() - aTitle.Length()));
            if (info.iTitle[0]) {
                HBufC *t = TextLC(info.iTitle);
                aValue.Append(t->Left(aValue.MaxLength() - 24));
                CleanupStack::PopAndDestroy(t);
                aValue.Append(_L(" \x2022 "));
            }
            if (info.iKbps)
                aValue.AppendFormat(_L("%d kbps \x2022 "), info.iKbps);
            aValue.AppendFormat(_L("vol %d"), iVolume);
            break;
        }
        case ERadioFinished:
            aTitle.Copy(_L("The station ended its stream"));
            aValue.Copy(_L("Select to play it again"));
            break;
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
        TBuf<128> title;
        TBuf<200> value;
        NowPlayingTextL(title, value);
        iList->SetRowL(0, title, value);
    }

    void RefreshL(TInt aCurrent = -1)
    {
        TBuf<128> title;
        TBuf<200> value;
        NowPlayingTextL(title, value);
        iList->ResetL();
        iList->AddRowL(title, value);
        switch (iMode) {
        case EFavourites:
            iList->AddRowL(_L("Search stations"), _L("by name, in the radio-browser.info directory"));
            iList->AddRowL(_L("Top stations"), _L("the directory's most played MP3 stations"));
            for (TInt i = 0; i < iFavourites.Count(); i++)
                iList->AddRowL(iFavourites[i]->iName, iFavourites[i]->iInfo);
            break;
        case EResults:
            for (TInt i = 0; i < iResults.Count(); i++)
                iList->AddRowL(iResults[i]->iName, iResults[i]->iInfo);
            if (!iResults.Count())
                iList->AddRowL(_L("No stations found"), _L("try another name"));
            break;
        case ESettings: {
            TBuf<16> v;
            v.Format(_L("%d of 10"), iVolume);
            iList->AddRowL(_L("Volume"), v);
            iList->AddRowL(_L("Screen orientation"),
                           iOrient == EOrientLandscape ? _L("Landscape") :
                           iOrient == EOrientPortrait ? _L("Portrait") : _L("Automatic (default)"));
            iList->AddRowL(_L("Debug log"), rsym_log_enabled() ? _L("On") : _L("Off (default)"));
            HBufC *path = SymbianPathLC(rsym_log_path());
            iList->AddRowL(_L("Debug log location"), *path);
            CleanupStack::PopAndDestroy(path);
            iList->AddRowL(_L("Clear debug log"), _L("delete the log file"));
            char status[96];
            rsym_rlog_status(status, sizeof status);
            iList->AddRowL(_L("Remote debug log"), status);
            iList->AddRowL(_L("Remote debug host"),
                           rsym_rlog_host()[0] ? rsym_rlog_host() : "not set (IP address of the log server)");
            v.Zero();
            v.AppendNum(rsym_rlog_port());
            iList->AddRowL(_L("Remote debug port"), v);
            break;
        }
        }
        iList->DoneL(aCurrent);
    }

    // ---- playing ----

    void PlayL(const TStation &aStation)
    {
        iPlaying = aStation;
        iLast = aStation;
        SaveSettings();
        iAudio->Stop();
        rsym_log("play: %s <%s>", aStation.iName, aStation.iUrl);
        RadioPlay(aStation.iUrl);
        RefreshNowPlayingL();
    }

    void StopL()
    {
        iAudio->Stop();
        RadioStop();
        RefreshNowPlayingL();
    }

    void NowPlayingSelectedL()
    {
        TRadioInfo info;
        RadioInfo(info);
        if (info.iState == ERadioStopped || info.iState == ERadioFailed ||
            info.iState == ERadioFinished) {
            if (iLast.iUrl[0])
                PlayL(iLast);
        } else {
            StopL();
        }
    }

    // MRadioNews: the engine's state, title or buffer changed.
    void RadioNewsL()
    {
        TRadioInfo info;
        RadioInfo(info);
        if (info.iState != iShownState)
            rsym_log("news: state %d, %d ms buffered", info.iState, info.iBufferedMs);
        if (info.iState == ERadioPlaying) {
            if (!iAudio->Running())
                iAudio->StartL(iVolume);
            iAudio->Feed();
        } else if (info.iState == ERadioStopped || info.iState == ERadioFailed) {
            iAudio->Stop();     // (when finished, the output plays out what it has)
        }
        if (info.iState != iShownState || strcmp(info.iTitle, iShownTitle) ||
            info.iState == ERadioBuffering) {
            if (info.iState == ERadioFailed && iShownState != ERadioFailed)
                rsym_log("play: failed: %s", info.iError);
            iShownState = info.iState;
            Copy(iShownTitle, sizeof iShownTitle, info.iTitle);
            RefreshNowPlayingL();
        }
    }

    // ---- the directory ----

    void SearchL()
    {
        TBuf<64> text(iLastSearch);
        if (!QueryTextL(_L("Search stations by name"), text))
            return;
        text.TrimAll();
        if (!text.Length())
            return;
        iLastSearch = text;
        StartSearchL(text);
    }

    void StartSearchL(const TDesC &aText)
    {
        HBufC8 *utf8 = ToUtf8LC(aText);
        char enc[200];
        int n = 0;
        for (const TUint8 *p = utf8->Ptr(); *p && n < (int)sizeof enc - 4; p++) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
                enc[n++] = *p;
            else
                n += sprintf(enc + n, "%%%02X", *p);
        }
        enc[n] = 0;
        CleanupStack::PopAndDestroy(utf8);
        snprintf(iPath, sizeof iPath,
                 "/json/stations/search?name=%s&codec=MP3&hidebroken=true"
                 "&order=clickcount&reverse=true&limit=50", enc);
        iResultsTitle.Copy(_L("Search: "));
        iResultsTitle.Append(aText.Left(iResultsTitle.MaxLength() - iResultsTitle.Length()));
        StartDirectoryL();
    }

    void TopL()
    {
        snprintf(iPath, sizeof iPath,
                 "/json/stations/search?codec=MP3&hidebroken=true"
                 "&order=clickcount&reverse=true&limit=50");
        iResultsTitle.Copy(_L("Top stations"));
        StartDirectoryL();
    }

    void StartDirectoryL()
    {
        if (iNet->Busy())
            return;
        rsym_http_request req;
        Mem::FillZ(&req, sizeof req);
        req.method = "GET";
        req.host = "all.api.radio-browser.info";
        req.path = iPath;
        req.plain = 1;
        req.user_agent = "rInternetRadio/0.1 (Symbian)";
        req.timeout_ms = 30000;
        rsym_log("directory: %s", iPath);
        TInt err = iNet->Start(req);
        if (err != KErrNone) {
            TBuf<64> msg;
            msg.Format(_L("Could not start the search (%d)."), err);
            InfoL(msg);
            return;
        }
        SetTitleL(_L("Searching..."));
    }

    void NetDoneL(const rsym_http_response &aResp, TInt aResult)
    {
        if (aResult != 0 || aResp.status != 200 || !aResp.body) {
            rsym_log("directory: failed (%d, HTTP %d): %s", aResult, aResp.status, aResp.error);
            if (iMode == EFavourites)
                SetTitleL(_L("rInternetRadio"));
            else if (iMode == ESettings)
                SetTitleL(_L("Settings"));
            else
                SetTitleL(iResultsTitle);
            HBufC *e = TextLC(aResult != 0 ? aResp.error : "The directory did not answer.");
            HBufC *msg = HBufC::NewLC(e->Length() + 64);
            msg->Des().Format(_L("Station search failed:\n%S"), e);
            InfoL(*msg);
            CleanupStack::PopAndDestroy(2, e);
            return;
        }
        iResults.ResetAndDestroy();
        cJSON *root = cJSON_ParseWithLength((const char *)aResp.body, aResp.body_len);
        int n = cJSON_IsArray(root) ? cJSON_GetArraySize(root) : 0;
        for (int i = 0; i < n; i++) {
            cJSON *st = cJSON_GetArrayItem(root, i);
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(st, "name"));
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(st, "url_resolved"));
            if (!url || !url[0])
                url = cJSON_GetStringValue(cJSON_GetObjectItem(st, "url"));
            if (!name || !url || !url[0])
                continue;
            const char *codec = cJSON_GetStringValue(cJSON_GetObjectItem(st, "codec"));
            const char *country = cJSON_GetStringValue(cJSON_GetObjectItem(st, "country"));
            cJSON *br = cJSON_GetObjectItem(st, "bitrate");
            TStation *s = new TStation;
            if (!s)
                break;
            Copy(s->iName, sizeof s->iName, name);
            Copy(s->iUrl, sizeof s->iUrl, url);
            int kbps = cJSON_IsNumber(br) ? br->valueint : 0;
            if (kbps > 0)
                snprintf(s->iInfo, sizeof s->iInfo, "%s %d kbps%s%s", codec ? codec : "",
                         kbps, country && country[0] ? ", " : "", country ? country : "");
            else
                snprintf(s->iInfo, sizeof s->iInfo, "%s%s%s", codec ? codec : "",
                         country && country[0] ? ", " : "", country ? country : "");
            if (iResults.Append(s) != KErrNone) {
                delete s;
                break;
            }
        }
        cJSON_Delete(root);
        rsym_log("directory: %d stations", iResults.Count());
        ShowResultsL(iResultsTitle);
    }

    // ---- favourites ----

    // The highlighted station (in favourites or results), or what plays.
    const TStation *SelectedStation()
    {
        TInt i = iList->CurrentIndex();
        if (iMode == EFavourites && i >= KFavFirst && i - KFavFirst < iFavourites.Count())
            return iFavourites[i - KFavFirst];
        if (iMode == EResults && i >= 1 && i - 1 < iResults.Count())
            return iResults[i - 1];
        return iPlaying.iUrl[0] ? &iPlaying : NULL;
    }

    void AddFavouriteL()
    {
        const TStation *s = SelectedStation();
        if (!s || IsFavourite(s->iUrl))
            return;
        AddPresetL(s->iName, s->iUrl, s->iInfo);
        SaveFavourites();
        InfoL(_L("Added to favourites."));
        if (iMode == EFavourites)
            RefreshL();
    }

    void RemoveFavouriteL()
    {
        TInt i = iList->CurrentIndex() - KFavFirst;
        if (iMode != EFavourites || i < 0 || i >= iFavourites.Count())
            return;
        HBufC *name = TextLC(iFavourites[i]->iName);
        HBufC *q = HBufC::NewLC(name->Length() + 40);
        q->Des().Format(_L("Remove %S from favourites?"), name);
        TBool yes = AskYesNoL(*q);
        CleanupStack::PopAndDestroy(2, name);
        if (!yes)
            return;
        delete iFavourites[i];
        iFavourites.Remove(i);
        SaveFavourites();
        RefreshL();
    }

    void AddUrlL()
    {
        TBuf<512> url(_L("http://"));
        if (!QueryTextL(_L("Stream or playlist address (MP3)"), url))
            return;
        url.TrimAll();
        if (url.Length() < 8)
            return;
        TBuf<96> name;
        if (!QueryTextL(_L("Name for this station"), name))
            return;
        name.TrimAll();
        HBufC8 *u8 = ToUtf8LC(url);
        HBufC8 *n8 = ToUtf8LC(name.Length() ? TPtrC(name) : TPtrC(url));
        AddPresetL((const char *)n8->Ptr(), (const char *)u8->Ptr(), "added by address");
        CleanupStack::PopAndDestroy(2, u8);
        SaveFavourites();
        ShowFavouritesL();
        PlayL(*iFavourites[iFavourites.Count() - 1]);
    }

    // ---- settings ----

    TBool EditRemoteHostL()
    {
        HBufC *cur = TextLC(rsym_rlog_host());
        TBuf<64> text(cur->Left(64));
        CleanupStack::PopAndDestroy(cur);
        if (!QueryTextL(_L("Remote debug host (log server IP address)"), text))
            return EFalse;
        text.TrimAll();
        HBufC8 *host = ToUtf8LC(text);
        rsym_rlog_configure(rsym_rlog_enabled(), (const char *)host->Ptr(), 0);
        CleanupStack::PopAndDestroy(host);
        return text.Length() > 0;
    }

    void SettingsItemL(TInt aIndex)
    {
        switch (aIndex) {
        case ESetVolume:
            VolumeL(iVolume >= 10 ? -10 : 1);
            break;
        case ESetOrient:
            // as NetSurf: landscape -> portrait -> automatic, remembered
            iOrient = iOrient == EOrientLandscape ? EOrientPortrait :
                      iOrient == EOrientPortrait ? EOrientAuto : EOrientLandscape;
            SetOrientationL(iOrient == EOrientLandscape ? EAppUiOrientationLandscape :
                            iOrient == EOrientPortrait ? EAppUiOrientationPortrait :
                            EAppUiOrientationAutomatic);
            SaveSettings();
            rsym_log("orientation: %c", (char)iOrient);
            break;
        case ESetDebug:
            rsym_log_set(!rsym_log_enabled());
            break;
        case ESetLogPath: {
            HBufC *path = SymbianPathLC(rsym_log_path());
            HBufC *msg = HBufC::NewLC(path->Length() + 80);
            msg->Des().Format(_L("Debug log:\n%S"), path);
            InfoL(*msg);
            CleanupStack::PopAndDestroy(2, path);
            break;
        }
        case ESetClear:
            if (AskYesNoL(_L("Delete the debug log?")))
                rsym_log_clear();
            break;
        case ESetRemote: {
            TBool on = !rsym_rlog_enabled();
            if (on && !rsym_rlog_host()[0] && !EditRemoteHostL())
                break;
            rsym_rlog_configure(on, NULL, 0);
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
        RefreshL();
    }

    // ---- menu and keys ----

    void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenu)
    {
        if (aResourceId != R_RADIO_MENU)
            return;
        TRadioInfo info;
        RadioInfo(info);
        TBool active = info.iState != ERadioStopped && info.iState != ERadioFailed &&
                       info.iState != ERadioFinished;
        TInt i = iList->CurrentIndex();
        TBool onStation = (iMode == EFavourites && i >= KFavFirst) ||
                          (iMode == EResults && i >= 1 && i - 1 < iResults.Count());
        const TStation *s = SelectedStation();
        aMenu->SetItemDimmed(ERadioCmdPlay, !onStation);
        aMenu->SetItemDimmed(ERadioCmdStop, !active);
        aMenu->SetItemDimmed(ERadioCmdAddFavourite,
                             iMode == ESettings || !s || IsFavourite(s->iUrl));
        aMenu->SetItemDimmed(ERadioCmdRemoveFavourite, !(iMode == EFavourites && i >= KFavFirst));
        aMenu->SetItemDimmed(ERadioCmdSettings, iMode == ESettings);
    }

    void HandleCommandL(TInt aCommand)
    {
        switch (aCommand) {
        case ERadioCmdPlay:
            if (const TStation *s = SelectedStation())
                PlayL(*s);
            break;
        case ERadioCmdStop:           StopL(); break;
        case ERadioCmdSearch:         SearchL(); break;
        case ERadioCmdTop:            TopL(); break;
        case ERadioCmdAddFavourite:   AddFavouriteL(); break;
        case ERadioCmdRemoveFavourite: RemoveFavouriteL(); break;
        case ERadioCmdAddUrl:         AddUrlL(); break;
        case ERadioCmdVolumeUp:       VolumeL(1); break;
        case ERadioCmdVolumeDown:     VolumeL(-1); break;
        case ERadioCmdSettings:       ShowSettingsL(); break;
        case ERadioCmdAbout:
            InfoL(_L("rInternetRadio 0.1 (experimental)\n"
                     "Developer: RuhanSA079\ngithub.com/RuhanSA079/SymbianApps\n"
                     "MP3 decoding: minimp3 by lieff (CC0)\n"
                     "Station directory: radio-browser.info\n"
                     "Preset stations: SomaFM"));
            break;
        case EAknSoftkeyBack:
            if (iMode != EFavourites)
                ShowFavouritesL();
            else if (AskYesNoL(_L("Exit rInternetRadio?")))
                Exit();
            break;
        case ERadioCmdExit:
        case EAknSoftkeyExit:
        case EAknCmdExit:
        case EEikCmdExit:
            Exit();
            break;
        default:
            break;
        }
    }

    // MRemConCoreApiTargetObserver: the phone's volume keys, headset buttons
    void MrccatoCommand(TRemConCoreApiOperationId aOperationId,
                        TRemConCoreApiButtonAction aButtonAct)
    {
        if (aButtonAct == ERemConCoreApiButtonRelease)
            return;
        switch (aOperationId) {
        case ERemConCoreApiVolumeUp:   TRAP_IGNORE(VolumeL(1)); break;
        case ERemConCoreApiVolumeDown: TRAP_IGNORE(VolumeL(-1)); break;
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

    CRadioList *iList;
    CAudioOut *iAudio;
    CRadioNotify *iNotify;
    CNetTask *iNet;
    CRemConInterfaceSelector *iRemCon;
    CRemConCoreApiTarget *iRemConTarget;
    TMode iMode;
    RPointerArray<TStation> iFavourites;
    RPointerArray<TStation> iResults;
    TStation iPlaying, iLast;
    TRadioState iShownState;
    char iShownTitle[160];
    TBuf<64> iLastSearch;
    TBuf<80> iResultsTitle;
    char iPath[400];
    TInt iVolume;
    TBool iShiftDown;
    TOrient iOrient;
    char *iCa;
    char iPrivDir[64];
    char iInstallDrive;
};

// ---------------------------------------------------------------------------

TKeyResponse CRadioList::OfferKeyEventL(const TKeyEvent &aKey, TEventCode aType)
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

void CRadioList::HandleListBoxEventL(CEikListBox *, TListBoxEvent aEvent)
{
    if ((aEvent == EEventEnterKeyPressed || aEvent == EEventItemSingleClicked ||
         aEvent == EEventItemDoubleClicked) && gAppUi)
        gAppUi->SelectL(iListBox->CurrentItemIndex());
}

class CRadioDocument : public CAknDocument
{
public:
    CRadioDocument(CEikApplication &aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi *CreateAppUiL() { return new (ELeave) CRadioAppUi; }
};

class CRadioApplication : public CAknApplication
{
private:
    TUid AppDllUid() const { return KUidRadio; }
    CApaDocument *CreateDocumentL() { return new (ELeave) CRadioDocument(*this); }
};

LOCAL_C CApaApplication *NewApplication() { return new CRadioApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
