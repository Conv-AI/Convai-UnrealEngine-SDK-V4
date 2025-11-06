/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiEditorModule.cpp
 *
 * Implementation of the ConvaiEditor module.
 */

#include "ConvaiEditor.h"
#include "ConvaiContentBrowserContextMenu.h"
#include "EditorUtilitySubsystem.h"
#include "Widgets/Input/SButton.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "Utility/Log/ConvaiLogger.h"
#include "Interfaces/IPluginManager.h"
#include "LevelEditor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ToolMenus.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "Styling/IThemeManager.h"
#include "Styling/ThemeManager.h"
#include "Styling/IConvaiStyleRegistry.h"
#include "Styling/ConvaiStyleRegistry.h"

// Version-specific style includes
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#else
#include "EditorStyleSet.h"
#endif
#include "Services/NavigationService.h"
#include "Services/ConfigurationService.h"
#include "Services/Configuration/IConfigurationReader.h"
#include "Services/Configuration/IConfigurationWriter.h"
#include "Services/Configuration/IAuthProvider.h"
#include "Services/Configuration/IThemeProvider.h"
#include "Services/Navigation/NavigationMiddlewareManager.h"
#include "Services/Navigation/AuthenticationMiddleware.h"
#include "Services/Navigation/LoggingMiddleware.h"
#include "Services/Navigation/NavigationHooksMiddleware.h"
#include "Services/IWelcomeService.h"
#include "Services/WelcomeService.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/IMainWindowManager.h"
#include "Services/MainWindowManager.h"
#include "Services/IUpdateCheckService.h"
#include "Services/UpdateCheckService.h"
#include "Services/ConvaiServiceRegistrationHelpers.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Services/Routes.h"
#include "UI/Shell/SConvaiShell.h"
#include "Framework/Application/SlateApplication.h"
#include "MVVM/ViewModel.h"
#include "MVVM/SamplesViewModel.h"
#include "MVVM/AnnouncementViewModel.h"
#include "MVVM/ChangelogViewModel.h"
#include "MVVM/BindingManager.h"
#include "UI/Utility/ConvaiWidgetFactory.h"
#include "UI/Utility/ConvaiPageFactoryUtils.h"
#include "UI/Factories/PageFactoryManager.h"
#include "UI/Factories/ConvaiPageFactories.h"
#include "UI/Shell/SWelcomeShell.h"
#include "UI/Pages/SWelcomePage.h"
#include "Services/ApiValidationService.h"
#include "Services/YouTubeTypes.h"
#include "Services/IYouTubeService.h"
#include "Services/YouTubeService.h"
#include "Services/OAuth/OAuthAuthenticationService.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"
#include "Services/OAuth/IOAuthHttpServerService.h"
#include "Services/OAuth/OAuthHttpServerService.h"
#include "Services/OAuth/IDecryptionService.h"
#include "Services/OAuth/DecryptionService.h"
#include "Services/IConvaiAccountService.h"
#include "Events/EventAggregator.h"
#include "Events/EventTypes.h"
#include "Services/ConvaiAccountService.h"
#include "Services/IAuthWindowManager.h"
#include "Services/AuthWindowManager.h"
#include "Services/IWelcomeWindowManager.h"
#include "Services/WelcomeWindowManager.h"
#include "Services/IMainWindowManager.h"
#include "Services/MainWindowManager.h"
#include "Services/ConvaiCharacterApiService.h"
#include "Services/ConvaiCharacterDiscoveryService.h"
#include "Services/IContentFeedService.h"
#include "Services/ContentFeedService.h"
#include "Services/RemoteContentFeedProvider.h"
#include "Services/MultiSourceContentFeedProvider.h"
#include "Services/ContentFeedCacheManager.h"
#include "Services/Configuration/IConfigurationValidator.h"
#include "Services/Configuration/ConfigurationValidator.h"
#include "Utility/ConvaiConstants.h"
#include "Utility/ConvaiWindowUtils.h"
#include "Utility/ConvaiURLs.h"
#include "Utility/NetworkConnectivityMonitor.h"
#include "Utility/CircuitBreakerRegistry.h"
#include "PropertyEditorModule.h"
#include "Misc/CoreDelegates.h"
#include "../Convai.h"

#define LOCTEXT_NAMESPACE "FConvaiEditorModule"

DEFINE_LOG_CATEGORY(LogConvaiEditor);

static FOnNetworkRestoredDelegate NetworkRestoredDelegate;

void FConvaiEditorModule::StartupModule()
{
    // Skip initialization when running commandlets (e.g., during packaging, cooking, etc.)
    // Editor modules should only initialize in the actual editor environment
    if (IsRunningCommandlet())
    {
        UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor module skipping initialization - running in commandlet mode"));
        return;
    }

    FPropertyEditorModule &PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    PropertyEditor.RegisterCustomClassLayout(
        UConvaiSettings::StaticClass()->GetFName(),
        FOnGetDetailCustomizationInstance::CreateStatic(&FConvaiEditorSettingsCustomization::MakeInstance));

    PropertyEditor.NotifyCustomizationModuleChanged();

    FConvaiContentBrowserContextMenu::Register();

    if (!FModuleManager::Get().LoadModule("WebBrowser"))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("WebBrowser module failed to load - web-based features may be unavailable"));
    }

    // Suppress WebBrowser and CEF logging after engine initialization
    FCoreDelegates::OnBeginFrame.AddLambda([]()
                                           {
    static bool bHasRun = false;
    if (!bHasRun && GEngine)
    {
        bHasRun = true;
        GEngine->Exec(nullptr, TEXT("Log LogWebBrowser off"));
        GEngine->Exec(nullptr, TEXT("Log LogCEF off"));
    } });

    InitializeCoreArchitecture();
    RegisterCoreServices();
    InitializeThemeSystem();
    RegisterApplicationServices();
    InitializeWindowManagers();
    RegisterEditorMenu();
    RegisterToolbarExtension();
    InitializeViewModels();
    ShowWelcomeWindowIfNeeded();

    ConvaiEditor::FNetworkConnectivityMonitor::FConfig NetworkConfig;
    NetworkConfig.CheckIntervalSeconds = 10.0f;
    NetworkConfig.ProbeTimeoutSeconds = 3.0f;
    NetworkConfig.bEnableLogging = false;
    NetworkConfig.bAutoStart = true;

    NetworkMonitor = MakeUnique<ConvaiEditor::FNetworkConnectivityMonitor>(NetworkConfig);
    NetworkMonitor.Get()->OnConnectivityChanged().AddRaw(this, &FConvaiEditorModule::OnNetworkConnectivityChanged);

    ConvaiEditor::FBindingManager::Get().Initialize();
}

void FConvaiEditorModule::ShutdownModule()
{
    // Skip shutdown if we never initialized (e.g., running in commandlet mode)
    if (IsRunningCommandlet())
    {
        return;
    }

    FConvaiContentBrowserContextMenu::Unregister();

    // Close all Convai windows before shutting down services
    if (FConvaiDIContainerManager::IsInitialized())
    {
        // Close main window
        auto MainWindowResult = FConvaiDIContainerManager::Get().Resolve<IMainWindowManager>();
        if (MainWindowResult.IsSuccess() && MainWindowResult.GetValue()->IsMainWindowOpen())
        {
            MainWindowResult.GetValue()->CloseMainWindow();
        }

        // Close welcome window
        auto WelcomeWindowResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeWindowManager>();
        if (WelcomeWindowResult.IsSuccess() && WelcomeWindowResult.GetValue()->IsWelcomeWindowOpen())
        {
            WelcomeWindowResult.GetValue()->CloseWelcomeWindow();
        }

        // Close auth window
        auto AuthWindowResult = FConvaiDIContainerManager::Get().Resolve<IAuthWindowManager>();
        if (AuthWindowResult.IsSuccess() && AuthWindowResult.GetValue()->IsAuthWindowOpen())
        {
            AuthWindowResult.GetValue()->CloseAuthWindow();
        }
    }

    if (NetworkMonitor.IsValid())
    {
        NetworkMonitor.Get()->Stop();
        NetworkMonitor.Reset();
    }

    ConvaiEditor::FBindingManager::Get().Shutdown();

    FConvaiWidgetFactory::Shutdown();

    FConvaiStyle::Shutdown();

    ConvaiEditor::FNavigationMiddlewareManager::Get().Shutdown();

    FConvaiDIContainerManager::Shutdown();
    FViewModelRegistry::Shutdown();

    ConvaiEditor::FEventAggregator::Get().Shutdown();

    FConvaiStyleResources::Shutdown();
}

void FConvaiEditorModule::OpenConvaiWindow(bool bShouldBeTopmost)
{
    FConvaiDIContainerManager::Get().Resolve<IMainWindowManager>().LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve MainWindowManager")).Tap([bShouldBeTopmost](TSharedPtr<IMainWindowManager> MainWindowManager)
                                                                                                                                                  { MainWindowManager->OpenMainWindow(bShouldBeTopmost); });
}

void FConvaiEditorModule::InitializeCoreArchitecture()
{
    ConvaiEditor::FEventAggregatorConfig EventConfig;
    EventConfig.bEnableEventHistory = false;
    EventConfig.MaxEventHistory = 100;
    EventConfig.bEnableVerboseLogging = false;
    ConvaiEditor::FEventAggregator::Get().Initialize(EventConfig);

    FConvaiDIContainerManager::Initialize();
    FViewModelRegistry::Initialize();

    ConvaiEditor::FNavigationMiddlewareManager::Get().Initialize();

    TSharedPtr<ConvaiEditor::FAuthenticationMiddleware> AuthMiddleware = MakeShared<ConvaiEditor::FAuthenticationMiddleware>();
    ConvaiEditor::FNavigationMiddlewareManager::Get().RegisterMiddleware(AuthMiddleware);

    TSharedPtr<ConvaiEditor::FLoggingMiddleware> LoggingMiddleware = MakeShared<ConvaiEditor::FLoggingMiddleware>();
    ConvaiEditor::FNavigationMiddlewareManager::Get().RegisterMiddleware(LoggingMiddleware);

    TSharedPtr<ConvaiEditor::FNavigationHooksMiddleware> HooksMiddleware = MakeShared<ConvaiEditor::FNavigationHooksMiddleware>();
    ConvaiEditor::FNavigationMiddlewareManager::Get().RegisterMiddleware(HooksMiddleware);
}

void FConvaiEditorModule::RegisterCoreServices()
{
    IConvaiDIContainer &DIContainer = FConvaiDIContainerManager::Get();

    ConvaiEditor::ServiceHelpers::FServiceRegistrationBatch()
        .Register<IConfigurationService, FConfigurationService>(
            DIContainer, TEXT("ConfigurationService"))
        .Register<IConfigurationValidator, FConfigurationValidator>(
            DIContainer, TEXT("ConfigurationValidator"))
        .Register<IWelcomeService, FWelcomeService>(
            DIContainer, TEXT("WelcomeService"))
        .Register<IThemeManager, FThemeManager>(
            DIContainer, TEXT("ThemeManager"))
        .Register<IConvaiStyleRegistry, FConvaiStyleRegistry>(
            DIContainer, TEXT("ConvaiStyleRegistry"))
        .LogSummary();

    DIContainer.RegisterServiceWithFactory<IConfigurationReader>(
        [](IConvaiDIContainer *Container) -> TSharedPtr<IConfigurationReader>
        {
            auto ConfigService = Container->Resolve<IConfigurationService>().GetValue();
            auto ConcreteService = StaticCastSharedPtr<FConfigurationService>(ConfigService);
            return StaticCastSharedPtr<IConfigurationReader>(ConcreteService);
        },
        EConvaiServiceLifetime::Singleton,
        TEXT("IConfigurationReader"));

    DIContainer.RegisterServiceWithFactory<IConfigurationWriter>(
        [](IConvaiDIContainer *Container) -> TSharedPtr<IConfigurationWriter>
        {
            auto ConfigService = Container->Resolve<IConfigurationService>().GetValue();
            auto ConcreteService = StaticCastSharedPtr<FConfigurationService>(ConfigService);
            return StaticCastSharedPtr<IConfigurationWriter>(ConcreteService);
        },
        EConvaiServiceLifetime::Singleton,
        TEXT("IConfigurationWriter"));

    DIContainer.RegisterServiceWithFactory<IAuthProvider>(
        [](IConvaiDIContainer *Container) -> TSharedPtr<IAuthProvider>
        {
            auto ConfigService = Container->Resolve<IConfigurationService>().GetValue();
            auto ConcreteService = StaticCastSharedPtr<FConfigurationService>(ConfigService);
            return StaticCastSharedPtr<IAuthProvider>(ConcreteService);
        },
        EConvaiServiceLifetime::Singleton,
        TEXT("IAuthProvider"));

    DIContainer.RegisterServiceWithFactory<IThemeProvider>(
        [](IConvaiDIContainer *Container) -> TSharedPtr<IThemeProvider>
        {
            auto ConfigService = Container->Resolve<IConfigurationService>().GetValue();
            auto ConcreteService = StaticCastSharedPtr<FConfigurationService>(ConfigService);
            return StaticCastSharedPtr<IThemeProvider>(ConcreteService);
        },
        EConvaiServiceLifetime::Singleton,
        TEXT("IThemeProvider"));
}

void FConvaiEditorModule::InitializeThemeSystem()
{
    IConvaiDIContainer &DIContainer = FConvaiDIContainerManager::Get();

    TSharedPtr<IConfigurationService> ConfigSvc;
    DIContainer.Resolve<IConfigurationService>()
        .LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve ConfigurationService"))
        .Tap([&ConfigSvc](TSharedPtr<IConfigurationService> Service)
             { ConfigSvc = Service; });

    TSharedPtr<IThemeManager> ThemeSvc;
    auto ThemeResult = DIContainer.Resolve<IThemeManager>()
                           .LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve ThemeManager"));

    if (ThemeResult.IsFailure())
    {
        return;
    }

    ThemeSvc = ThemeResult.GetValue();

    FString ThemeId = TEXT("dark");
    if (ConfigSvc.IsValid())
    {
        ThemeId = ConfigSvc->GetThemeId();
    }

    ThemeSvc->SetActiveTheme(ThemeId);

    if (!ThemeSvc->GetStyle().IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to load theme '%s' - falling back to default"), *ThemeId);
        return;
    }

    FConvaiStyleResources::Initialize();
    FConvaiStyle::Initialize(nullptr);
    FConvaiWidgetFactory::Initialize();
}

void FConvaiEditorModule::RegisterApplicationServices()
{
    IConvaiDIContainer &DIContainer = FConvaiDIContainerManager::Get();

    ConvaiEditor::ServiceHelpers::FServiceRegistrationBatch()
        .Register<IPageFactoryManager, FPageFactoryManager>(
            DIContainer, TEXT("PageFactoryManager"))
        .Register<INavigationService, FNavigationService>(
            DIContainer, TEXT("NavigationService"))
        .Register<IApiValidationService, FApiValidationService>(
            DIContainer, TEXT("ApiValidationService"))
        .Register<IYouTubeService, FYouTubeService>(
            DIContainer, TEXT("YouTubeService"), EConvaiServiceLifetime::Singleton)
        .Register<IConvaiAccountService, FConvaiAccountService>(
            DIContainer, TEXT("ConvaiAccountService"))
        .Register<IConvaiCharacterApiService, FConvaiCharacterApiService>(
            DIContainer, TEXT("ConvaiCharacterApiService"), EConvaiServiceLifetime::Singleton)
        .Register<IConvaiCharacterDiscoveryService, FConvaiCharacterDiscoveryService>(
            DIContainer, TEXT("ConvaiCharacterDiscoveryService"), EConvaiServiceLifetime::Singleton)
        .Register<IOAuthHttpServerService, FOAuthHttpServerService>(
            DIContainer, TEXT("OAuthHttpServerService"))
        .Register<IDecryptionService, FDecryptionService>(
            DIContainer, TEXT("DecryptionService"))
        .Register<IOAuthAuthenticationService, FOAuthAuthenticationService>(
            DIContainer, TEXT("OAuthAuthenticationService"))
        .Register<IAuthWindowManager, FAuthWindowManager>(
            DIContainer, TEXT("AuthWindowManager"))
        .Register<IWelcomeWindowManager, FWelcomeWindowManager>(
            DIContainer, TEXT("WelcomeWindowManager"))
        .Register<IMainWindowManager, FMainWindowManager>(
            DIContainer, TEXT("MainWindowManager"))
        .Register<IUpdateCheckService, FUpdateCheckService>(
            DIContainer, TEXT("UpdateCheckService"), EConvaiServiceLifetime::Singleton)
        .LogSummary();

    RegisterAnnouncementContentService(DIContainer);
}

void FConvaiEditorModule::InitializeWindowManagers()
{
    IConvaiDIContainer &DIContainer = FConvaiDIContainerManager::Get();

    DIContainer.Resolve<IAuthWindowManager>()
        .LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve AuthWindowManager"))
        .Tap([](TSharedPtr<IAuthWindowManager> Manager)
             { Manager->Startup(); });

    DIContainer.Resolve<IWelcomeWindowManager>()
        .LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve WelcomeWindowManager"))
        .Tap([](TSharedPtr<IWelcomeWindowManager> Manager)
             { Manager->Startup(); });
}

void FConvaiEditorModule::RegisterEditorMenu()
{
    if (!UToolMenus::IsToolMenuUIEnabled())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Editor menu registration skipped - tool menus unavailable"));
        return;
    }

    UToolMenus *ToolMenus = UToolMenus::Get();
    UToolMenu *Menu = ToolMenus->ExtendMenu("LevelEditor.MainMenu.Window");
    FToolMenuSection &Section = Menu->AddSection("ConvaiEditor", LOCTEXT("ConvaiEditorHeader", "Convai"));
    Section.AddMenuEntry(
        "ConvaiEditor.Open",
        LOCTEXT("ConvaiEditorOpenLabel", "Open Convai Editor"),
        LOCTEXT("ConvaiEditorOpenTooltip", "Open Convai Editor"),
        FSlateIcon(FConvaiStyle::GetStyleSetName(), "Convai.Icon.16"),
        FUIAction(FExecuteAction::CreateLambda([this]()
                                               { OpenConvaiWindow(false); })));
}

void FConvaiEditorModule::RegisterToolbarExtension()
{
    if (!UToolMenus::IsToolMenuUIEnabled())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Toolbar registration skipped - tool menus unavailable"));
        return;
    }

    UToolMenus *ToolMenus = UToolMenus::Get();
    UToolMenu *ToolbarMenu = ToolMenus->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
    FToolMenuSection &ToolbarSection = ToolbarMenu->FindOrAddSection("Convai");
    ToolbarSection.AddEntry(FToolMenuEntry::InitToolBarButton(
        "OpenConvaiEditor",
        FUIAction(FExecuteAction::CreateLambda([this]()
                                               { OpenConvaiWindow(false); })),
        LOCTEXT("ConvaiEditorToolbarLabel", "Convai Editor"),
        LOCTEXT("ConvaiEditorToolbarTooltip", "Open Convai Editor"),
        FSlateIcon(FConvaiStyle::GetStyleSetName(), "Convai.Icon.40")));
}

void FConvaiEditorModule::InitializeViewModels()
{
    FViewModelRegistry::Get().CreateViewModel<FSamplesViewModel>();

    if (AnnouncementContentService.IsValid())
    {
        auto AnnouncementViewModel = MakeShared<FAnnouncementViewModel>(AnnouncementContentService);
        AnnouncementViewModel->Initialize();
        FViewModelRegistry::Get().RegisterViewModel(FAnnouncementViewModel::StaticType(), AnnouncementViewModel);
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to initialize announcement content service"));
    }

    TSharedPtr<IContentFeedService> ChangelogService = CreateChangelogContentService();
    if (ChangelogService.IsValid())
    {
        auto ChangelogViewModel = MakeShared<FChangelogViewModel>(ChangelogService);
        ChangelogViewModel->Initialize();
        FViewModelRegistry::Get().RegisterViewModel(FChangelogViewModel::StaticType(), ChangelogViewModel);
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to initialize changelog content service"));
    }

    IConvaiDIContainer &DIContainer = FConvaiDIContainerManager::Get();
    DIContainer.Resolve<IUpdateCheckService>()
        .LogOnFailure(LogConvaiEditor, TEXT("Failed to resolve UpdateCheckService"))
        .Tap([](TSharedPtr<IUpdateCheckService> Service)
             { Service->Startup(); });
}

void FConvaiEditorModule::ShowWelcomeWindowIfNeeded()
{
    if (FSlateApplication::IsInitialized())
    {
        auto WelcomeSvcResult = FConvaiDIContainerManager::Get().Resolve<IWelcomeService>();
        if (WelcomeSvcResult.IsSuccess())
        {
            TSharedPtr<IWelcomeService> WelcomeSvc = WelcomeSvcResult.GetValue();
            WelcomeSvc->ShowWelcomeWindowIfNeeded();
        }
        else
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Failed to initialize welcome service - Error: %s"), *WelcomeSvcResult.GetError());
        }
    }
}

void FConvaiEditorModule::RegisterAnnouncementContentService(IConvaiDIContainer &DIContainer)
{
    TArray<FString> SourceURLs = FConvaiURLs::GetAnnouncementsFeedURLs();

    FRemoteContentFeedProvider::FConfig BaseConfig;
    BaseConfig.TimeoutSeconds = 10.0f;
    BaseConfig.MaxRetries = 2;
    BaseConfig.RetryDelaySeconds = 1.0f;

    FMultiSourceContentFeedProvider::FMultiSourceConfig MultiSourceConfig;
    MultiSourceConfig.SourceURLs = SourceURLs;
    MultiSourceConfig.ContentType = EContentType::Announcements;
    MultiSourceConfig.BaseConfig = BaseConfig;
    MultiSourceConfig.bRequireAllSources = false;
    MultiSourceConfig.bDeduplicateByID = true;

    TUniquePtr<IContentFeedProvider> Provider = MakeUnique<FMultiSourceContentFeedProvider>(MultiSourceConfig);

    FContentFeedCacheManager::FConfig CacheConfig;
    CacheConfig.ContentType = EContentFeedCacheType::Announcements;
    CacheConfig.TTLSeconds = 3600.0;
    CacheConfig.CacheFileName = TEXT("announcements_cache.json");
    CacheConfig.bEnableDiskCache = true;

    TUniquePtr<FContentFeedCacheManager> CacheManager = MakeUnique<FContentFeedCacheManager>(CacheConfig);

    TSharedPtr<FContentFeedService> Service = MakeShared<FContentFeedService>(
        MoveTemp(Provider),
        MoveTemp(CacheManager),
        EContentFeedType::Announcements);

    AnnouncementContentService = Service;

    if (!Service.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to create announcement content service"));
    }
}

TSharedPtr<IContentFeedService> FConvaiEditorModule::CreateChangelogContentService()
{
    TArray<FString> SourceURLs = FConvaiURLs::GetChangelogsFeedURLs();

    FRemoteContentFeedProvider::FConfig BaseConfig;
    BaseConfig.TimeoutSeconds = 10.0f;
    BaseConfig.MaxRetries = 2;
    BaseConfig.RetryDelaySeconds = 1.0f;

    FMultiSourceContentFeedProvider::FMultiSourceConfig MultiSourceConfig;
    MultiSourceConfig.SourceURLs = SourceURLs;
    MultiSourceConfig.ContentType = EContentType::Changelogs;
    MultiSourceConfig.BaseConfig = BaseConfig;
    MultiSourceConfig.bRequireAllSources = false;
    MultiSourceConfig.bDeduplicateByID = true;

    TUniquePtr<IContentFeedProvider> Provider = MakeUnique<FMultiSourceContentFeedProvider>(MultiSourceConfig);

    FContentFeedCacheManager::FConfig CacheConfig;
    CacheConfig.ContentType = EContentFeedCacheType::Changelogs;
    CacheConfig.TTLSeconds = 3600.0;
    CacheConfig.CacheFileName = TEXT("changelogs_cache.json");
    CacheConfig.bEnableDiskCache = true;

    TUniquePtr<FContentFeedCacheManager> CacheManager = MakeUnique<FContentFeedCacheManager>(CacheConfig);

    TSharedPtr<IContentFeedService> ChangelogService = MakeShared<FContentFeedService>(
        MoveTemp(Provider),
        MoveTemp(CacheManager),
        EContentFeedType::Changelogs);

    if (!ChangelogService.IsValid())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("Failed to create changelog content service"));
    }

    return ChangelogService;
}

void FConvaiEditorModule::OnNetworkConnectivityChanged(bool bIsConnected)
{
    if (!bIsConnected)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Network connectivity lost - API features unavailable"));

        ConvaiEditor::FEventAggregator::Get().Publish(
            ConvaiEditor::FNetworkDisconnectedEvent(TEXT("Network connectivity monitor detected loss")));

        return;
    }

    int32 ResetCount = ConvaiEditor::FCircuitBreakerRegistry::Get().ForceAllClosed();

    NetworkRestoredDelegate.Broadcast();

    ConvaiEditor::FEventAggregator::Get().Publish(
        ConvaiEditor::FNetworkRestoredEvent(0.0, ResetCount));
}

FOnNetworkRestoredDelegate &FConvaiEditorModule::GetNetworkRestoredDelegate()
{
    return NetworkRestoredDelegate;
}

FConvaiEditorModule &FConvaiEditorModule::Get()
{
    return FModuleManager::LoadModuleChecked<FConvaiEditorModule>("ConvaiEditor");
}

TSharedRef<IDetailCustomization> FConvaiEditorSettingsCustomization::MakeInstance()
{
    return MakeShareable(new FConvaiEditorSettingsCustomization);
}

void FConvaiEditorSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder &DetailBuilder)
{
    IDetailCategoryBuilder &ConvaiCategory = DetailBuilder.EditCategory(TEXT("Convai"), FText::FromString("Convai"));

    // Get property handles for API Key and Auth Token
    APIKeyPropertyHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UConvaiSettings, API_Key));
    AuthTokenPropertyHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UConvaiSettings, AuthToken));

    // Custom row for API Key with hide/show button
    ConvaiCategory.AddCustomRow(FText::FromString("API Key"))
        .NameContent()
            [APIKeyPropertyHandle->CreatePropertyNameWidget()]
        .ValueContent()
        .MinDesiredWidth(250.0f)
        .MaxDesiredWidth(600.0f)
            [SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SEditableTextBox).Text_Lambda([this]()
                                                                                                              {
        FString Value;
        if (APIKeyPropertyHandle.IsValid())
        {
            APIKeyPropertyHandle->GetValue(Value);
            return FText::FromString(bShowAPIKey ? Value : FString::ChrN(Value.Len(), TEXT('●')));
        }
        return FText::GetEmpty(); })
                                                                               .IsReadOnly(true)
                                                                               .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                                                                               .ToolTipText(FText::FromString("Automatically provided by Convai Editor UI"))] +
             SHorizontalBox::Slot()
                 .AutoWidth()
                 .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                     [SNew(SButton)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                          .ButtonStyle(FAppStyle::Get(), "SimpleButton")
#else
                          .ButtonStyle(FEditorStyle::Get(), "SimpleButton")
#endif
                          .OnClicked(this, &FConvaiEditorSettingsCustomization::OnToggleAPIKeyVisibility)
                          .ToolTipText(FText::FromString("Toggle API Key visibility"))
                          .ContentPadding(FMargin(2.0f))
                              [SNew(SImage)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                                   .Image_Lambda([this]()
                                                 { return bShowAPIKey ? FAppStyle::GetBrush("Icons.Visible") : FAppStyle::GetBrush("Icons.Hidden"); })
#else
                                   .Image(bShowAPIKey ? FEditorStyle::GetBrush("Icons.Visible") : FEditorStyle::GetBrush("Icons.Hidden"))
#endif
    ]]];

    // Custom row for Auth Token with hide/show button (marked as advanced)
    ConvaiCategory.AddCustomRow(FText::FromString("Auth Token"), true)
        .NameContent()
            [AuthTokenPropertyHandle->CreatePropertyNameWidget()]
        .ValueContent()
        .MinDesiredWidth(250.0f)
        .MaxDesiredWidth(600.0f)
            [SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SEditableTextBox).Text_Lambda([this]()
                                                                                                              {
        FString Value;
        if (AuthTokenPropertyHandle.IsValid())
        {
            AuthTokenPropertyHandle->GetValue(Value);
            return FText::FromString(bShowAuthToken ? Value : FString::ChrN(Value.Len(), TEXT('●')));
        }
        return FText::GetEmpty(); })
                                                                               .IsReadOnly(true)
                                                                               .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                                                                               .ToolTipText(FText::FromString("Automatically provided by Convai Editor UI"))] +
             SHorizontalBox::Slot()
                 .AutoWidth()
                 .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                     [SNew(SButton)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                          .ButtonStyle(FAppStyle::Get(), "SimpleButton")
#else
                          .ButtonStyle(FEditorStyle::Get(), "SimpleButton")
#endif
                          .OnClicked(this, &FConvaiEditorSettingsCustomization::OnToggleAuthTokenVisibility)
                          .ToolTipText(FText::FromString("Toggle Auth Token visibility"))
                          .ContentPadding(FMargin(2.0f))
                              [SNew(SImage)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                                   .Image_Lambda([this]()
                                                 { return bShowAuthToken ? FAppStyle::GetBrush("Icons.Visible") : FAppStyle::GetBrush("Icons.Hidden"); })
#else
                                   .Image(bShowAuthToken ? FEditorStyle::GetBrush("Icons.Visible") : FEditorStyle::GetBrush("Icons.Hidden"))
#endif
    ]]];

    // Hide original properties since we're customizing them
    DetailBuilder.HideProperty(APIKeyPropertyHandle);
    DetailBuilder.HideProperty(AuthTokenPropertyHandle);

    IDetailCategoryBuilder &SubCategory = DetailBuilder.EditCategory(TEXT("Long Term Memory"), FText::FromString("Long Term Memory"));

    SubCategory.AddCustomRow(FText::FromString("Spawn Tab"))
        .WholeRowWidget
            [SNew(SHorizontalBox) + SHorizontalBox::Slot()
                                        .HAlign(HAlign_Left)
                                        .VAlign(VAlign_Center)
                                        .AutoWidth()
                                            [SNew(SButton)
                                                 .Text(FText::FromString("Manage Speaker ID"))
                                                 .HAlign(HAlign_Center)
                                                 .VAlign(VAlign_Center)
                                                 .ContentPadding(FMargin(8.0f, 2.0f))
                                                 .OnClicked(this, &FConvaiEditorSettingsCustomization::OnSpawnTabClicked)]];
}

FReply FConvaiEditorSettingsCustomization::OnToggleAPIKeyVisibility()
{
    bShowAPIKey = !bShowAPIKey;
    return FReply::Handled();
}

FReply FConvaiEditorSettingsCustomization::OnToggleAuthTokenVisibility()
{
    bShowAuthToken = !bShowAuthToken;
    return FReply::Handled();
}

FReply FConvaiEditorSettingsCustomization::OnSpawnTabClicked()
{
    const FString WidgetPath = TEXT("/ConvAI/Editor/EUW_LTM.EUW_LTM");

    UEditorUtilityWidgetBlueprint *WidgetBlueprint = LoadObject<UEditorUtilityWidgetBlueprint>(nullptr, *WidgetPath);

    if (WidgetBlueprint)
    {
        if (UEditorUtilitySubsystem *Subsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>())
        {
            Subsystem->SpawnAndRegisterTab(WidgetBlueprint);
            CONVAI_LOG(LogTemp, Log, TEXT("Successfully spawned the Editor Utility Widget: %s"), *WidgetPath);
        }
        else
        {
            CONVAI_LOG(LogTemp, Warning, TEXT("Failed to get Editor Utility Subsystem."));
        }
    }
    else
    {
        CONVAI_LOG(LogTemp, Error, TEXT("Failed to load Editor Utility Widget Blueprint at path: %s"), *WidgetPath);
    }

    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FConvaiEditorModule, ConvaiEditor);
