// "Hello from Claude" for Symbian^3, built on Linux with GnuPoc + GCCE.
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <avkon.hrh>
#include <eikenv.h>
#include <eikstart.h>
#include <coecntrl.h>

const TUid KUidHelloClaude = { TInt32(0xE5A1E002) };

class CHelloView : public CCoeControl
    {
public:
    void ConstructL(const TRect& aRect)
        {
        CreateWindowL();
        SetRect(aRect);
        ActivateL();
        }
private:
    void Draw(const TRect& /*aRect*/) const
        {
        CWindowGc& gc = SystemGc();
        gc.Clear(Rect());
        const CFont* font = iEikonEnv->TitleFont();
        gc.UseFont(font);
        TInt baseline = Rect().Height() / 2 + font->AscentInPixels() / 2;
        gc.DrawText(_L("Hello from Claude"), Rect(), baseline, CGraphicsContext::ECenter);
        gc.DiscardFont();
        }
    };

class CHelloAppUi : public CAknAppUi
    {
public:
    void ConstructL()
        {
        BaseConstructL(EAknEnableSkin);
        iView = new (ELeave) CHelloView;
        iView->ConstructL(ClientRect());
        AddToStackL(iView);
        }
    ~CHelloAppUi()
        {
        if (iView)
            {
            RemoveFromStack(iView);
            delete iView;
            }
        }
private:
    void HandleCommandL(TInt aCommand)
        {
        if (aCommand == EAknSoftkeyExit || aCommand == EEikCmdExit)
            Exit();
        }
    void HandleResourceChangeL(TInt aType)
        {
        CAknAppUi::HandleResourceChangeL(aType);
        if (aType == KEikDynamicLayoutVariantSwitch && iView)
            iView->SetRect(ClientRect());   // keyboard slide / rotation
        }
    CHelloView* iView;
    };

class CHelloDocument : public CAknDocument
    {
public:
    CHelloDocument(CEikApplication& aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi* CreateAppUiL() { return new (ELeave) CHelloAppUi; }
    };

class CHelloApplication : public CAknApplication
    {
private:
    TUid AppDllUid() const { return KUidHelloClaude; }
    CApaDocument* CreateDocumentL() { return new (ELeave) CHelloDocument(*this); }
    };

LOCAL_C CApaApplication* NewApplication() { return new CHelloApplication; }
GLDEF_C TInt E32Main() { return EikStart::RunApplication(NewApplication); }
