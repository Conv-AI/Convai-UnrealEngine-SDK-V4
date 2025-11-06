/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * MainWindowManager.h
 *
 * Manages the main editor window lifecycle in the Convai Editor.
 */

#pragma once

#include "Services/IMainWindowManager.h"
#include "UI/Shell/SConvaiShell.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

namespace ConvaiEditor
{
    class FServiceScope;
}

/**
 * Manages the main editor window lifecycle in the Convai Editor.
 */
class FMainWindowManager : public IMainWindowManager, public TSharedFromThis<FMainWindowManager>
{
public:
    FMainWindowManager();
    virtual ~FMainWindowManager();

    virtual void OpenMainWindow(bool bShouldBeTopmost = false) override;
    virtual void CloseMainWindow() override;
    virtual bool IsMainWindowOpen() const override;
    virtual void BringMainWindowToFront() override;
    virtual TWeakPtr<SConvaiShell> GetMainWindow() const override { return MainWindow; }
    virtual FVector2D GetWindowSize() const override;
    virtual FVector2D GetMinWindowSize() const override;
    virtual void SetMainWindowTitle(const FString &Title) override;
    virtual void DisableMainWindowTopmost() override;
    virtual FOnMainWindowOpened &OnMainWindowOpened() override { return MainWindowOpenedDelegate; }
    virtual FOnMainWindowClosed &OnMainWindowClosed() override { return MainWindowClosedDelegate; }
    virtual void Startup() override;
    virtual void Shutdown() override;
    static FName StaticType() { return TEXT("IMainWindowManager"); }

private:
    TSharedRef<SConvaiShell> CreateMainWindow(bool bShouldBeTopmost);
    void InitializePageFactories(TSharedRef<SConvaiShell> Window);
    void SetupNavigationService(TSharedRef<SConvaiShell> Window);
    void HandleMainWindowClosed(const TSharedRef<SWindow> &ClosedWindow);
    bool ShouldShowWelcomeFlow() const;
    void LoadWindowDimensions();

    TWeakPtr<SConvaiShell> MainWindow;
    TSharedPtr<ConvaiEditor::FServiceScope> WindowScope;
    int32 WindowWidth;
    int32 WindowHeight;
    float MinWindowWidth;
    float MinWindowHeight;

    FOnMainWindowOpened MainWindowOpenedDelegate;
    FOnMainWindowClosed MainWindowClosedDelegate;
};
