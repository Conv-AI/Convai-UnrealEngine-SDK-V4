/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiEditor.h
 *
 * Main header for the ConvaiEditor plugin.
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "IDetailCustomization.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "HAL/CriticalSection.h"
#include "Utility/NetworkConnectivityMonitor.h"
#include <atomic>

/** Log category for Convai Editor messages */
DECLARE_LOG_CATEGORY_EXTERN(LogConvaiEditor, Log, All);

class IConvaiService;

/**
 * Base interface for all services in the ConvaiEditor system.
 */
class CONVAIEDITOR_API IConvaiService
{
public:
    virtual ~IConvaiService() = default;

    /** Called after service construction for initialization */
    virtual void Startup() {}

    /** Called before service destruction for cleanup */
    virtual void Shutdown() {}

    /** Returns the service type name for registration and lookup */
    static FName StaticType() { return TEXT("IConvaiService"); }
};

/** Delegate for network connectivity restoration */
DECLARE_MULTICAST_DELEGATE(FOnNetworkRestoredDelegate);

/**
 * Main module for the ConvaiEditor plugin.
 */
class FConvaiEditorModule : public IModuleInterface
{
public:
    /** Initializes the module and registers services */
    virtual void StartupModule() override;

    /** Shuts down the module and cleans up resources */
    virtual void ShutdownModule() override;

    /** Opens the main Convai Editor window */
    void OpenConvaiWindow(bool bShouldBeTopmost = false);

    /** Returns the network restoration delegate */
    static FOnNetworkRestoredDelegate &GetNetworkRestoredDelegate();

    /** Returns the module singleton instance */
    static FConvaiEditorModule &Get();

private:
    /** Initializes core architecture systems */
    void InitializeCoreArchitecture();

    /** Registers core services with the DI container */
    void RegisterCoreServices();

    /** Initializes the theme and styling system */
    void InitializeThemeSystem();

    /** Registers application services with the DI container */
    void RegisterApplicationServices();

    /** Initializes window managers */
    void InitializeWindowManagers();

    /** Registers editor menu integration */
    void RegisterEditorMenu();

    /** Registers toolbar button integration */
    void RegisterToolbarExtension();

    /** Initializes ViewModels */
    void InitializeViewModels();

    /** Shows the welcome window if needed for first-time setup */
    void ShowWelcomeWindowIfNeeded();

    /** Registers ContentFeedService for announcements */
    void RegisterAnnouncementContentService(class IConvaiDIContainer &DIContainer);

    /** Creates a content feed service instance configured for changelogs */
    TSharedPtr<class IContentFeedService> CreateChangelogContentService();

    /** Handles network connectivity changes */
    void OnNetworkConnectivityChanged(bool bIsConnected);

    /** Called when editor initialization is complete */
    void OnEditorInitialized(double DeltaTime);

    /** Called during EnginePreExit for very early cleanup (before Slate window destruction) */
    void OnEnginePreExit();

    TSharedPtr<class IContentFeedService> AnnouncementContentService;
    TUniquePtr<ConvaiEditor::FNetworkConnectivityMonitor> NetworkMonitor;
    FDelegateHandle EditorInitializedHandle;
    FDelegateHandle EnginePreExitHandle;
    FDelegateHandle LogSuppressionHandle;
    std::atomic<bool> bEarlyCleanupCompleted{false};
};

/**
 * Customization for Convai Editor settings in the project settings.
 */
class FConvaiEditorSettingsCustomization : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder &DetailBuilder) override;

private:
    FReply OnSpawnTabClicked();
    FReply OnToggleAPIKeyVisibility();
    FReply OnToggleAuthTokenVisibility();

    TSharedPtr<IPropertyHandle> APIKeyPropertyHandle;
    TSharedPtr<IPropertyHandle> AuthTokenPropertyHandle;
    bool bShowAPIKey = false;
    bool bShowAuthToken = false;
};
