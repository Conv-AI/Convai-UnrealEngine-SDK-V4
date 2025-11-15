/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * WelcomeWindowManager.h
 *
 * Manages the welcome window lifecycle in the Convai Editor.
 */

#pragma once

#include "Services/IWelcomeWindowManager.h"
#include "UI/Shell/SWelcomeShell.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Delegates/Delegate.h"

namespace ConvaiEditor
{
    class FServiceScope;
}

/**
 * Manages the welcome window lifecycle in the Convai Editor.
 */
class FWelcomeWindowManager : public IWelcomeWindowManager, public TSharedFromThis<FWelcomeWindowManager>
{
public:
    FWelcomeWindowManager();
    virtual ~FWelcomeWindowManager();

    virtual void ShowWelcomeWindow() override;
    virtual void CloseWelcomeWindow() override;
    virtual bool IsWelcomeWindowOpen() const override;
    virtual void BringWelcomeWindowToFront() override;
    virtual FVector2D GetWelcomeWindowSize() const override;
    virtual FVector2D GetWelcomeWindowMinSize() const override;
    virtual void SetWelcomeWindowTitle(const FString &Title) override;
    virtual FOnWelcomeWindowOpened &OnWelcomeWindowOpened() override { return WelcomeWindowOpenedDelegate; }
    virtual FOnWelcomeWindowClosed &OnWelcomeWindowClosed() override { return WelcomeWindowClosedDelegate; }
    virtual void Startup() override;
    virtual void Shutdown() override;

private:
    void CreateWelcomeWindow();
    void HandleWelcomeWindowClosed(const TSharedRef<SWindow, ESPMode::ThreadSafe> &Window);
    void LoadWelcomeWindowDimensions();
    void ResetAuthenticationState();

    TWeakPtr<SWelcomeShell> WelcomeWindow;
    TSharedPtr<ConvaiEditor::FServiceScope> WindowScope;
    TSharedPtr<class IOAuthAuthenticationService> CachedAuthService;
    FDelegateHandle WindowClosedDelegateHandle;
    FString WindowTitle;
    FVector2D WindowSize;
    FVector2D WindowMinSize;

    FOnWelcomeWindowOpened WelcomeWindowOpenedDelegate;
    FOnWelcomeWindowClosed WelcomeWindowClosedDelegate;
};