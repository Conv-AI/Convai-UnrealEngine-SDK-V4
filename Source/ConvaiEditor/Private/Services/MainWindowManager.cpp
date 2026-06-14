/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * MainWindowManager.cpp
 *
 * Implementation of the main window manager service.
 */

#include "Services/MainWindowManager.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ServiceScope.h"
#include "Services/IWelcomeService.h"
#include "Services/IWelcomeWindowManager.h"
#include "Services/NavigationService.h"
#include "Services/IUIContainer.h"
#include "Services/Routes.h"
#include "Services/ConfigurationService.h"
#include "UI/Factories/PageFactoryManager.h"
#include "UI/Utility/ConvaiPageFactoryUtils.h"
#include "Utility/ConvaiConstants.h"
#include "Utility/ConvaiWindowUtils.h"

FMainWindowManager::FMainWindowManager()
    : WindowWidth(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowWidth)), WindowHeight(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowHeight)), MinWindowWidth(ConvaiEditor::Constants::Layout::Window::MainWindowMinWidth), MinWindowHeight(ConvaiEditor::Constants::Layout::Window::MainWindowMinHeight)
{
}

FMainWindowManager::~FMainWindowManager()
{
    Shutdown();
}

void FMainWindowManager::Startup()
{
    LoadWindowDimensions();

    auto WelcomeResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeService>();
    if (WelcomeResult.IsSuccess())
    {
        CachedWelcomeService = WelcomeResult.GetValue();
    }
}

void FMainWindowManager::Shutdown()
{
    CloseMainWindow();
    NavigationService.Reset();
    CachedWelcomeService.Reset();
    MainWindowOpenedDelegate.Clear();
    MainWindowClosedDelegate.Clear();
}

void FMainWindowManager::OpenMainWindow(bool bShouldBeTopmost)
{
    if (!FSlateApplication::IsInitialized())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Main window cannot be opened: Slate application not initialized"));
        return;
    }

    if (ShouldShowWelcomeFlow())
    {
        UE_LOG(LogConvaiEditor, Log, TEXT("Welcome flow required: displaying welcome window"));

        FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>().LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve WelcomeWindowManager")).Tap([this](TSharedPtr<IWelcomeWindowManager> WelcomeWindowManager)
                                                                                                                                                            { WelcomeWindowManager->ShowWelcomeWindow(); });
        return;
    }

    if (IsMainWindowOpen())
    {
        BringMainWindowToFront();
        return;
    }

    WindowScope = FConvaiDIContainerManager::CreateScope("MainWindow");
    if (!WindowScope.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Main window creation failed: unable to create service scope"));
        return;
    }

    TSharedRef<SConvaiShell> Window = CreateMainWindow(bShouldBeTopmost);
    MainWindow = Window;

    InitializePageFactories(Window);
    SetupNavigationService(Window);

    Window->GetOnWindowClosedEvent().AddSP(this, &FMainWindowManager::HandleMainWindowClosed);

    FSlateApplication::Get().AddWindow(Window);
    Window->BringToFront();

    MainWindowOpenedDelegate.Broadcast();
}

void FMainWindowManager::CloseMainWindow()
{
    TSharedPtr<SConvaiShell> Window = MainWindow.Pin();
    if (Window.IsValid())
    {
        if (FSlateApplication::IsInitialized())
        {
            Window->RequestDestroyWindow();
        }
        MainWindow.Reset();

        if (NavigationService.IsValid())
        {
            NavigationService->ResetWindowState();
        }

        if (WindowScope.IsValid())
        {
            FConvaiDIContainerManager::DestroyScope(WindowScope);
            WindowScope.Reset();
        }

        MainWindowClosedDelegate.Broadcast();
    }
}

bool FMainWindowManager::IsMainWindowOpen() const
{
    return MainWindow.IsValid();
}

void FMainWindowManager::BringMainWindowToFront()
{
    TSharedPtr<SConvaiShell> Window = MainWindow.Pin();
    if (Window.IsValid())
    {
        Window->BringToFront();
    }
}

FVector2D FMainWindowManager::GetWindowSize() const
{
    return FVector2D(WindowWidth, WindowHeight);
}

FVector2D FMainWindowManager::GetMinWindowSize() const
{
    return FVector2D(MinWindowWidth, MinWindowHeight);
}

void FMainWindowManager::SetMainWindowTitle(const FString &Title)
{
    TSharedPtr<SConvaiShell> Window = MainWindow.Pin();
    if (Window.IsValid())
    {
    }
}

void FMainWindowManager::DisableMainWindowTopmost()
{
    TSharedPtr<SConvaiShell> Window = MainWindow.Pin();
    if (Window.IsValid())
    {
        Window->DisableTopmost();
    }
}

TSharedRef<SConvaiShell> FMainWindowManager::CreateMainWindow(bool bShouldBeTopmost)
{
    TSharedRef<SConvaiShell> Window = SNew(SConvaiShell)
                                          .InitialWidth(WindowWidth)
                                          .InitialHeight(WindowHeight)
                                          .MinWidth(MinWindowWidth)
                                          .MinHeight(MinWindowHeight)
                                          .ShouldBeTopmost(bShouldBeTopmost);

    return Window;
}

void FMainWindowManager::InitializePageFactories(TSharedRef<SConvaiShell> Window)
{
    TSharedPtr<IPageFactoryManager> PageFactoryManager;
    auto FactoryResult = FConvaiDIContainerManager::Get().Resolve<IPageFactoryManager>().LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve PageFactoryManager"));

    if (FactoryResult.IsFailure())
    {
        return;
    }

    PageFactoryManager = FactoryResult.GetValue();

    // Check if factories are already registered to avoid duplicate registration warnings
    TArray<ConvaiEditor::Route::E> RegisteredRoutes = PageFactoryManager->GetRegisteredRoutes();
    if (RegisteredRoutes.Num() > 0)
    {
        // Factories already registered, skip registration
        return;
    }

    TArray<TSharedPtr<IPageFactory>> Factories = FConvaiPageFactoryUtils::CreateStandardFactories(Window);

    int32 SuccessCount = FConvaiPageFactoryUtils::RegisterFactoriesWithLogging(PageFactoryManager, Factories);
}

void FMainWindowManager::SetupNavigationService(TSharedRef<SConvaiShell> Window)
{
    FConvaiDIContainerManager::Get().Resolve<INavigationService>().LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve NavigationService")).Tap([this, &Window](TSharedPtr<INavigationService> NavSvc)
                                                                                                                                                   {
                                                                                                                                                       NavigationService = NavSvc;
                                                                                                                                                       TWeakPtr<IUIContainer> UIContainer = StaticCastSharedRef<IUIContainer>(Window);
                                                                                                                                                       NavSvc->SetUIContainer(UIContainer);

                                                                                                                                                       NavSvc->Navigate(ConvaiEditor::Route::E::Home); });
}

void FMainWindowManager::HandleMainWindowClosed(const TSharedRef<SWindow> &ClosedWindow)
{
    if (MainWindow.IsValid() && MainWindow.Pin() == StaticCastSharedRef<SConvaiShell>(ClosedWindow))
    {
        MainWindow.Reset();

        if (NavigationService.IsValid())
        {
            NavigationService->ResetWindowState();
        }

        if (WindowScope.IsValid())
        {
            FConvaiDIContainerManager::DestroyScope(WindowScope);
            WindowScope.Reset();
        }

        MainWindowClosedDelegate.Broadcast();
    }
}

bool FMainWindowManager::ShouldShowWelcomeFlow() const
{
    if (CachedWelcomeService.IsValid())
    {
        return !CachedWelcomeService->HasCompletedWelcome() || !CachedWelcomeService->HasValidApiKey();
    }

    return true;
}

void FMainWindowManager::LoadWindowDimensions()
{
    ConvaiEditor::WindowUtils::FWindowDimensions MainWindowDimensions = ConvaiEditor::WindowUtils::GetMainWindowDimensions();
    ConvaiEditor::WindowUtils::ValidateWindowDimensions(MainWindowDimensions, TEXT("Main Window"));

    auto ConfigSvcResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
    if (ConfigSvcResult.IsSuccess())
    {
        TSharedPtr<IConfigurationService> ConfigSvc = ConfigSvcResult.GetValue();
        WindowWidth = ConfigSvc->GetWindowWidth();
        WindowHeight = ConfigSvc->GetWindowHeight();
        MinWindowWidth = ConfigSvc->GetMinWindowWidth();
        MinWindowHeight = ConfigSvc->GetMinWindowHeight();
    }
    else
    {
        WindowWidth = MainWindowDimensions.InitialWidth;
        WindowHeight = MainWindowDimensions.InitialHeight;
        MinWindowWidth = MainWindowDimensions.MinWidth;
        MinWindowHeight = MainWindowDimensions.MinHeight;

        UE_LOG(LogConvaiEditor, Warning, TEXT("Configuration service unavailable, using default window dimensions: %s"), *ConfigSvcResult.GetError());
    }
}
