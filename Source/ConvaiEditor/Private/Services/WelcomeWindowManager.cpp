/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * WelcomeWindowManager.cpp
 *
 * Implementation of welcome window manager.
 */

#include "Services/WelcomeWindowManager.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ServiceScope.h"
#include "UI/Pages/SWelcomePage.h"
#include "Utility/ConvaiWindowUtils.h"
#include "Utility/ConvaiConstants.h"
#include "Framework/Application/SlateApplication.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"

FWelcomeWindowManager::FWelcomeWindowManager()
    : WindowTitle(TEXT("Welcome to Convai")), WindowSize(ConvaiEditor::Constants::Layout::Window::WelcomeWindowWidth, ConvaiEditor::Constants::Layout::Window::WelcomeWindowHeight), WindowMinSize(ConvaiEditor::Constants::Layout::Window::WelcomeWindowMinWidth, ConvaiEditor::Constants::Layout::Window::WelcomeWindowMinHeight)
{
}

FWelcomeWindowManager::~FWelcomeWindowManager()
{
    Shutdown();
}

void FWelcomeWindowManager::Startup()
{
    LoadWelcomeWindowDimensions();
}

void FWelcomeWindowManager::Shutdown()
{
    CloseWelcomeWindow();
}

void FWelcomeWindowManager::ShowWelcomeWindow()
{
    ResetAuthenticationState();

    if (IsWelcomeWindowOpen())
    {
        BringWelcomeWindowToFront();
        return;
    }

    if (!FSlateApplication::IsInitialized())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeWindowManager: Slate not initialized - cannot open welcome window"));
        return;
    }

    CreateWelcomeWindow();
}

void FWelcomeWindowManager::CloseWelcomeWindow()
{
    if (WelcomeWindow.IsValid())
    {
        TSharedPtr<SWelcomeShell> Window = WelcomeWindow.Pin();
        if (Window.IsValid())
        {
            Window->RequestDestroyWindow();
        }
        WelcomeWindow.Reset();

        if (WindowScope.IsValid())
        {
            FConvaiDIContainerManager::DestroyScope(WindowScope);
            WindowScope.Reset();
        }
    }
}

bool FWelcomeWindowManager::IsWelcomeWindowOpen() const
{
    return WelcomeWindow.IsValid();
}

void FWelcomeWindowManager::BringWelcomeWindowToFront()
{
    if (WelcomeWindow.IsValid())
    {
        TSharedPtr<SWelcomeShell> Window = WelcomeWindow.Pin();
        if (Window.IsValid())
        {
            Window->BringToFront();
        }
    }
}

FVector2D FWelcomeWindowManager::GetWelcomeWindowSize() const
{
    return WindowSize;
}

FVector2D FWelcomeWindowManager::GetWelcomeWindowMinSize() const
{
    return WindowMinSize;
}

void FWelcomeWindowManager::SetWelcomeWindowTitle(const FString &Title)
{
    WindowTitle = Title;
}

void FWelcomeWindowManager::CreateWelcomeWindow()
{
    WindowScope = FConvaiDIContainerManager::CreateScope("WelcomeWindow");
    if (!WindowScope.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("WelcomeWindowManager: failed to create service scope for welcome window"));
        return;
    }

    ConvaiEditor::WindowUtils::FWindowDimensions WelcomeDimensions = ConvaiEditor::WindowUtils::GetWelcomeWindowDimensions();
    ConvaiEditor::WindowUtils::ValidateWindowDimensions(WelcomeDimensions, TEXT("Welcome Window"));

    TSharedRef<SWelcomeShell> Window = SNew(SWelcomeShell)
                                           .InitialWidth(WelcomeDimensions.InitialWidth)
                                           .InitialHeight(WelcomeDimensions.InitialHeight)
                                           .MinWidth(WelcomeDimensions.MinWidth)
                                           .MinHeight(WelcomeDimensions.MinHeight);

    TSharedRef<SWelcomePage> WelcomePage = SNew(SWelcomePage);
    Window->SetWelcomeContent(WelcomePage);

    Window->GetOnWindowClosedEvent().AddLambda([this](const TSharedRef<SWindow, ESPMode::ThreadSafe> &ClosedWindow)
                                               { HandleWelcomeWindowClosed(ClosedWindow); });

    FSlateApplication::Get().AddWindow(Window);
    WelcomeWindow = Window;

    Window->BringToFront();

    WelcomeWindowOpenedDelegate.Broadcast();
}

void FWelcomeWindowManager::HandleWelcomeWindowClosed(const TSharedRef<SWindow, ESPMode::ThreadSafe> &Window)
{
    if (WelcomeWindow.IsValid() && WelcomeWindow.Pin() == StaticCastSharedRef<SWelcomeShell>(Window))
    {
        WelcomeWindow.Reset();

        if (WindowScope.IsValid())
        {
            FConvaiDIContainerManager::DestroyScope(WindowScope);
            WindowScope.Reset();
        }

        WelcomeWindowClosedDelegate.Broadcast();
    }
}

void FWelcomeWindowManager::LoadWelcomeWindowDimensions()
{
    ConvaiEditor::WindowUtils::FWindowDimensions WelcomeDimensions = ConvaiEditor::WindowUtils::GetWelcomeWindowDimensions();
    ConvaiEditor::WindowUtils::ValidateWindowDimensions(WelcomeDimensions, TEXT("Welcome Window"));

    WindowSize = FVector2D(WelcomeDimensions.InitialWidth, WelcomeDimensions.InitialHeight);
    WindowMinSize = FVector2D(WelcomeDimensions.MinWidth, WelcomeDimensions.MinHeight);
}

void FWelcomeWindowManager::ResetAuthenticationState()
{
    auto AuthServiceResult = FConvaiDIContainerManager::Get().Resolve<IOAuthAuthenticationService>();
    if (AuthServiceResult.IsSuccess())
    {
        TSharedPtr<IOAuthAuthenticationService> AuthService = AuthServiceResult.GetValue();
        if (AuthService.IsValid() && AuthService->IsAuthInProgress())
        {
            AuthService->CancelLogin();
            UE_LOG(LogConvaiEditorConfig, Log, TEXT("WelcomeWindowManager: reset active authentication state"));
        }
    }
}