/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWindowControlsPanel.cpp
 *
 * Implementation of the window controls panel.
 */

#include "UI/Components/SWindowControlsPanel.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "EditorStyleSet.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"
#include "UI/Components/SVerticalDivider.h"
#include "UI/Components/SCircularAvatar.h"
#include "UI/Components/SAccountMenu.h"
#include "UI/Components/SUpdateNotificationBadge.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"
#include "UI/Dropdown/SConvaiDropdown.h"
#include "UI/Utility/HoverAwareMenuWrapper.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/NavigationService.h"
#include "Services/IUpdateCheckService.h"
#include "Services/LogExport/ConvaiLogExporter.h"
#include "Services/LogExport/ConvaiLogExportDialog.h"

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#define CONVAI_EDITOR_STYLE FAppStyle
#else
#define CONVAI_EDITOR_STYLE FEditorStyle
#endif
#include "Services/IConvaiAccountService.h"
#include "Services/ConfigurationService.h"
#include "UI/Dialogs/SConvaiPrivacyConsentDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/Paths.h"
#include "Services/Routes.h"
#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"
#include "Models/ConvaiUserInfo.h"

#define LOCTEXT_NAMESPACE "SWindowControlsPanel"

void SWindowControlsPanel::Construct(const FArguments &InArgs)
{
    OnSettingsClicked = InArgs._OnSettingsClicked;
    OnMinimizeClicked = InArgs._OnMinimizeClicked;
    OnMaximizeClicked = InArgs._OnMaximizeClicked;
    OnCloseClicked = InArgs._OnCloseClicked;
    OnManageAccountClicked = InArgs._OnManageAccountClicked;
    OnSignOutClicked = InArgs._OnSignOutClicked;
    IsMaximized = InArgs._IsMaximized;

    CachedUsername = TEXT("User");
    CachedEmail = TEXT("user@convai.com");

    FetchUserAccountData();

    const ISlateStyle &Style = FConvaiStyle::Get();

    using namespace ConvaiEditor::Constants::Layout::Icons;
    const FVector2D SettingsIconSize = Settings;
    const FVector2D WindowControlIconSize = Minimize;
    const FMargin SettingsButtonPadding(
        Style.GetFloat("Convai.Spacing.windowControl.settingsButtonPaddingHorizontal"),
        Style.GetFloat("Convai.Spacing.windowControl.settingsButtonPaddingVertical"));
    const FMargin WindowControlButtonPadding(
        Style.GetFloat("Convai.Spacing.windowControl.controlButtonPaddingHorizontal"),
        Style.GetFloat("Convai.Spacing.windowControl.controlButtonPaddingVertical"));
    const float IconSpacing = Style.GetFloat("Convai.Spacing.windowControl.iconSpacing");
    const float DividerSideMargin = Style.GetFloat("Convai.Spacing.windowControl.dividerSideMargin");
    const float DividerVerticalMargin = Style.GetFloat("Convai.Spacing.windowControl.dividerVerticalMargin");
    const FVector2D WindowControlButtonSize = Style.GetVector("Convai.Size.windowControl.buttonSize");

    TSharedPtr<SButton> AccountButton;
    TSharedPtr<SButton> SettingsButton;
    TSharedPtr<FSettingsDropdownHoverState> AccountHoverState = MakeShared<FSettingsDropdownHoverState>();
    TSharedPtr<FSettingsDropdownHoverState> SettingsHoverState = MakeShared<FSettingsDropdownHoverState>();

    ChildSlot
        [SNew(SHorizontalBox)

         + SHorizontalBox::Slot()
               .AutoWidth()
               .VAlign(VAlign_Center)
               .Padding(0, 0, 8, 0)
                   [SAssignNew(AccountHoverState->anchor, SMenuAnchor)
                        .Method(EPopupMethod::UseCurrentWindow)
                        .UseApplicationMenuStack(false)
                        .Placement(MenuPlacement_BelowRightAnchor)
                        .OnGetMenuContent_Lambda([this, AccountHoverState]()
                                                 { return BuildAccountMenu(AccountHoverState); })
                        .OnMenuOpenChanged_Lambda([this, AccountHoverState](bool bOpen)
                                                  { 
                                                      if (bOpen)
                                                      {
                                                          RegisterMenuClickOutside(AccountHoverState->anchor, AccountHoverState, TEXT("SAccountMenu"));
                                                      }
                                                      else
                                                      {
                                                          UnregisterMenuClickOutside(AccountHoverState->anchor);
                                                      }
                                                      Invalidate(EInvalidateWidget::Layout); })
                            [SAssignNew(AccountButton, SButton)
                                 .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
                                 .ContentPadding(0)
                                 .ToolTipText(LOCTEXT("AccountTooltip", "Account"))
                                 .Cursor(EMouseCursor::Hand)
                                 .OnClicked_Lambda([AccountHoverState]()
                                                   {
                                     if (AccountHoverState->anchor.IsValid())
                                     {
                                         AccountHoverState->anchor->SetIsOpen(!AccountHoverState->anchor->IsOpen());
                                     }
                                     return FReply::Handled(); })
                                     [SNew(SCircularAvatar)
                                          .Username_Lambda([this]()
                                                           { return CachedUsername; })
                                          .Size(24.0f)
                                          .FontSize(10.0f)]]]

         + SHorizontalBox::Slot()
               .AutoWidth()
               .VAlign(VAlign_Center)
               .Padding(0)
                   [SNew(SOverlay) + SOverlay::Slot()[SAssignNew(SettingsHoverState->anchor, SMenuAnchor).Method(EPopupMethod::UseCurrentWindow).UseApplicationMenuStack(false).Placement(MenuPlacement_BelowRightAnchor).OnGetMenuContent_Lambda([this, SettingsHoverState]()
                                                                                                                                                                                                                                                  { return BuildSettingsDropdown(SettingsHoverState); })
                                                          .OnMenuOpenChanged_Lambda([this, SettingsHoverState](bool bOpen)
                                                                                    { 
                                                             if (bOpen)
                                                             {
                                                                 RegisterMenuClickOutside(SettingsHoverState->anchor, SettingsHoverState, TEXT("SConvaiDropdown"));
                                                             }
                                                             else
                                                             {
                                                            UnregisterMenuClickOutside(SettingsHoverState->anchor);
                                                            }
                                                            Invalidate(EInvalidateWidget::Layout); })[SAssignNew(SettingsButton, SButton).ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder").ContentPadding(SettingsButtonPadding).ToolTipText(LOCTEXT("SettingsTooltip", "Settings")).Cursor(EMouseCursor::Hand).OnClicked_Lambda([SettingsHoverState]()
                                                                                                                                                                                                                                                                                                                       {
                                            if (SettingsHoverState->anchor.IsValid())
                                            {
                                                SettingsHoverState->anchor->SetIsOpen(!SettingsHoverState->anchor->IsOpen());
                                            }
                                            return FReply::Handled(); })[SNew(SImage).Image(FConvaiStyle::Get().GetBrush("Convai.Icon.Settings")).DesiredSizeOverride(SettingsIconSize).ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([SettingsHoverState, SettingsButton]()
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 {
                             const ISlateStyle& ST = FConvaiStyle::Get();
                             if (SettingsHoverState->anchor.IsValid() && SettingsHoverState->anchor->IsOpen()) 
                                 return ST.GetColor("Convai.Color.action.active");
                             if (SettingsButton.IsValid() && SettingsButton->IsHovered())
                                 return ST.GetColor("Convai.Color.action.hover");
                             return ST.GetColor("Convai.Color.text.primary"); }))]]] +
                    SOverlay::Slot()
                        .HAlign(HAlign_Right)
                        .VAlign(VAlign_Top)
                        .Padding(0, 2, 2, 0)
                            [SAssignNew(UpdateBadge, SUpdateNotificationBadge)
                                 .BadgeColor(FLinearColor(0.2f, 0.8f, 0.2f, 1.0f))
                                 .BadgeSize(8.0f)
                                 .bEnableAnimation(true)
                                 .ToolTipText(LOCTEXT("UpdateAvailableTooltip", "Update available! Click Settings to learn more."))]] +
         SHorizontalBox::Slot()
             .AutoWidth()
             .VAlign(VAlign_Fill)
                 [SNew(SVerticalDivider)
                      .DividerType(EDividerType::WindowControl)
                      .Thickness(1.0f)
                      .Margin(FMargin(0, DividerVerticalMargin, DividerSideMargin, DividerVerticalMargin))
                      .Radius(0.f)] +
         SHorizontalBox::Slot()
             .AutoWidth()
             .VAlign(VAlign_Center)
             .Padding(DividerSideMargin, 0, IconSpacing, 0)
                 [SNew(SBox)
                      .WidthOverride(WindowControlButtonSize.X)
                      .HeightOverride(WindowControlButtonSize.Y)
                          [SNew(SBorder)
                               .BorderImage_Lambda([this]()
                                                   {
                    const ISlateStyle& ST = FConvaiStyle::Get();
                    if (MinimizeButton.IsValid() && MinimizeButton->IsPressed())
                        return ST.GetBrush("Convai.ColorBrush.windowControl.background.active");
                    if (MinimizeButton.IsValid() && MinimizeButton->IsHovered())
                        return ST.GetBrush("Convai.ColorBrush.windowControl.background.hover");
                    return ST.GetBrush("Convai.ColorBrush.windowControl.background.normal"); })
                               .Padding(0)
                                   [SAssignNew(MinimizeButton, SButton)
                                        .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
                                        .ContentPadding(FMargin(0))
                                        .ToolTipText(LOCTEXT("MinimizeTooltip", "Minimize"))
                                        .OnClicked_Lambda([this]()
                                                          { if (OnMinimizeClicked.IsBound()) OnMinimizeClicked.Execute(); return FReply::Handled(); })
                                        .HAlign(HAlign_Fill)
                                        .VAlign(VAlign_Fill)
                                            [SNew(SBox)
                                                 .HAlign(HAlign_Center)
                                                 .VAlign(VAlign_Center)
                                                     [SNew(SImage)
                                                          .Image(FConvaiStyle::Get().GetBrush("Convai.Icon.Minimize"))
                                                          .DesiredSizeOverride(WindowControlIconSize)
                                                          .ColorAndOpacity(Style.GetColor("Convai.Color.windowControl.normal"))]]]]] +
         SHorizontalBox::Slot()
             .AutoWidth()
             .VAlign(VAlign_Center)
             .Padding(0, 0, IconSpacing, 0)
                 [SNew(SBox)
                      .WidthOverride(WindowControlButtonSize.X)
                      .HeightOverride(WindowControlButtonSize.Y)
                          [SNew(SBorder)
                               .BorderImage_Lambda([this]()
                                                   {
                    const ISlateStyle& ST = FConvaiStyle::Get();
                    if (MaximizeButton.IsValid() && MaximizeButton->IsPressed())
                        return ST.GetBrush("Convai.ColorBrush.windowControl.background.active");
                    if (MaximizeButton.IsValid() && MaximizeButton->IsHovered())
                        return ST.GetBrush("Convai.ColorBrush.windowControl.background.hover");
                    return ST.GetBrush("Convai.ColorBrush.windowControl.background.normal"); })
                               .Padding(0)
                                   [SAssignNew(MaximizeButton, SButton)
                                        .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
                                        .ContentPadding(FMargin(0))
                                        .ToolTipText(LOCTEXT("MaximizeTooltip", "Maximize/Restore"))
                                        .OnClicked_Lambda([this]()
                                                          { if (OnMaximizeClicked.IsBound()) OnMaximizeClicked.Execute(); return FReply::Handled(); })
                                        .HAlign(HAlign_Fill)
                                        .VAlign(VAlign_Fill)
                                            [SNew(SBox)
                                                 .HAlign(HAlign_Center)
                                                 .VAlign(VAlign_Center)
                                                     [SNew(SImage)
                                                          .Image(this, &SWindowControlsPanel::GetMaximizeRestoreIcon)
                                                          .DesiredSizeOverride(WindowControlIconSize)
                                                          .ColorAndOpacity(Style.GetColor("Convai.Color.windowControl.normal"))]]]]] +
         SHorizontalBox::Slot()
             .AutoWidth()
             .VAlign(VAlign_Center)
                 [SNew(SBox)
                      .WidthOverride(WindowControlButtonSize.X)
                      .HeightOverride(WindowControlButtonSize.Y)
                          [SNew(SBorder)
                               .BorderImage_Lambda([this]()
                                                   {
                    const ISlateStyle& ST = FConvaiStyle::Get();
                    if (CloseButton.IsValid() && CloseButton->IsPressed())
                        return ST.GetBrush("Convai.ColorBrush.windowControl.close.background.active");
                    if (CloseButton.IsValid() && CloseButton->IsHovered())
                        return ST.GetBrush("Convai.ColorBrush.windowControl.close.background.hover");
                    return ST.GetBrush("Convai.ColorBrush.windowControl.close.background.normal"); })
                               .Padding(0)
                                   [SAssignNew(CloseButton, SButton)
                                        .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
                                        .ContentPadding(FMargin(0))
                                        .ToolTipText(LOCTEXT("CloseTooltip", "Close"))
                                        .OnClicked_Lambda([this]()
                                                          { if (OnCloseClicked.IsBound()) OnCloseClicked.Execute(); return FReply::Handled(); })
                                        .HAlign(HAlign_Fill)
                                        .VAlign(VAlign_Fill)
                                            [SNew(SBox)
                                                 .HAlign(HAlign_Center)
                                                 .VAlign(VAlign_Center)
                                                     [SNew(SImage)
                                                          .Image(FConvaiStyle::Get().GetBrush("Convai.Icon.Close"))
                                                          .DesiredSizeOverride(WindowControlIconSize)
                                                          .ColorAndOpacity(Style.GetColor("Convai.Color.windowControl.close.normal"))]]]]]];

    RefreshUpdateBadge();

    auto UpdateServiceResult = FConvaiDIContainerManager::Get().Resolve<IUpdateCheckService>();
    if (UpdateServiceResult.IsSuccess())
    {
        TSharedPtr<IUpdateCheckService> UpdateService = UpdateServiceResult.GetValue();
        UpdateService->OnUpdateAvailabilityChanged().AddSP(this, &SWindowControlsPanel::OnUpdateAvailabilityChanged);
    }
}

TSharedRef<SWidget> SWindowControlsPanel::BuildDropdownMenu(
    const TArray<FConvaiMenuEntry> &Entries,
    const FName &FontStyle)
{
    const FName EffectiveFontStyle =
        FontStyle.IsNone() ? FName("Convai.Font.dropdown") : FontStyle;

    return SNew(SConvaiDropdown)
        .Entries(Entries)
        .FontStyle(EffectiveFontStyle);
}

TSharedRef<SWidget> SWindowControlsPanel::BuildSettingsDropdown(TSharedPtr<FSettingsDropdownHoverState> HoverState)
{
    TArray<FConvaiMenuEntry> Items;

    Items.Add({LOCTEXT("ExportLogs", "Export Logs"),
               FSimpleDelegate::CreateLambda([this]()
                                             { OnExportLogsClicked(); })});

    bool bHighlightUpdate = false;
    auto UpdateServiceResultTemp = FConvaiDIContainerManager::Get().Resolve<IUpdateCheckService>();
    if (UpdateServiceResultTemp.IsSuccess())
    {
        bHighlightUpdate = UpdateServiceResultTemp.GetValue()->IsUpdateAvailable();
    }

    Items.Add({LOCTEXT("CheckUpdate", "Check for Updates"),
               FSimpleDelegate::CreateLambda([this]()
                                             { OnCheckForUpdatesClicked(); }),
               bHighlightUpdate});

    return BuildDropdownMenu(
        Items,
        FName("Convai.Font.dropdownIcon"));
}

const FSlateBrush *SWindowControlsPanel::GetMaximizeRestoreIcon() const
{
    const bool bIsMaximized = IsMaximized.IsSet() ? IsMaximized.Get() : false;
    return FConvaiStyle::Get().GetBrush(bIsMaximized ? "Convai.Icon.Maximize" : "Convai.Icon.Restore");
}

void SWindowControlsPanel::OnExportLogsClicked()
{
    if (!SConvaiPrivacyConsentDialog::ShowConsentDialog(true))
    {
        return;
    }

    FNotificationInfo Info(LOCTEXT("ExportLogsStarted", "Exporting Logs...\n\nPlease wait while we package your log files."));
    Info.bFireAndForget = false;
    Info.FadeOutDuration = 0.5f;
    Info.ExpireDuration = 10.0f;

    TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
    if (NotificationItem.IsValid())
    {
        NotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
    }

    FConvaiLogExportOptions Options = FConvaiLogExportOptions::Default();
    Options.bOpenLocationAfterExport = true;

    FConvaiLogExporter::Get().ExportLogsAsync(Options,
                                              [NotificationItem](const FConvaiPackageResult &Result)
                                              {
                                                  if (!NotificationItem.IsValid())
                                                  {
                                                      return;
                                                  }

                                                  if (Result.bSuccess)
                                                  {
                                                      FString SuccessMessage = FString::Printf(
                                                          TEXT("Logs Exported Successfully\n\n%d file%s (%.2f MB)\nOpening in File Explorer..."),
                                                          Result.TotalFiles,
                                                          Result.TotalFiles > 1 ? TEXT("s") : TEXT(""),
                                                          Result.TotalSizeBytes / (1024.0 * 1024.0));

                                                      NotificationItem->SetText(FText::FromString(SuccessMessage));
                                                      NotificationItem->SetExpireDuration(10.0f);
                                                      NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
                                                      NotificationItem->ExpireAndFadeout();
                                                  }
                                                  else
                                                  {
                                                      FString ErrorMessage = FString::Printf(
                                                          TEXT("Log Export Failed\n\n%s"),
                                                          *Result.ErrorMessage);

                                                      NotificationItem->SetText(FText::FromString(ErrorMessage));
                                                      NotificationItem->SetExpireDuration(10.0f);
                                                      NotificationItem->SetCompletionState(SNotificationItem::CS_Fail);
                                                      NotificationItem->ExpireAndFadeout();
                                                  }
                                              });
}

void SWindowControlsPanel::OnContactSupportClicked()
{
    // Show privacy consent dialog first
    if (!SConvaiPrivacyConsentDialog::ShowConsentDialog(false))
    {
        // User declined
        return;
    }

    // Show dialog to collect user's issue description
    FConvaiIssueReport UserReport;
    if (!SConvaiLogExportDialog::ShowDialog(UserReport))
    {
        // User cancelled
        return;
    }

    // Show processing notification
    FNotificationInfo Info(LOCTEXT("ProcessingSupport", "Creating Support Package...\n\nPlease wait while we prepare your support files."));
    Info.bFireAndForget = false;
    Info.FadeOutDuration = 0.5f;
    Info.ExpireDuration = 10.0f;

    TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
    if (NotificationItem.IsValid())
    {
        NotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
    }

    // Export logs with user report
    FConvaiLogExportOptions Options = FConvaiLogExportOptions::Default();
    Options.bOpenLocationAfterExport = false;
    Options.UserReport = MakeShared<FConvaiIssueReport>(UserReport);

    FConvaiLogExporter::Get().ExportLogsAsync(Options,
                                              [NotificationItem, UserReport](const FConvaiPackageResult &Result)
                                              {
                                                  if (!NotificationItem.IsValid())
                                                  {
                                                      return;
                                                  }

                                                  if (Result.bSuccess)
                                                  {
                                                      // Success - keep message concise
                                                      FString SuccessMessage = FString::Printf(
                                                          TEXT("Support Package Created Successfully\n\n%d file%s (%.2f MB)\nOpening in File Explorer..."),
                                                          Result.TotalFiles,
                                                          Result.TotalFiles > 1 ? TEXT("s") : TEXT(""),
                                                          Result.TotalSizeBytes / (1024.0 * 1024.0));

                                                      NotificationItem->SetText(FText::FromString(SuccessMessage));
                                                      NotificationItem->SetExpireDuration(10.0f);
                                                      NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
                                                      NotificationItem->ExpireAndFadeout();

            // Open export location with file selected (Windows specific)
#if PLATFORM_WINDOWS
                                                      FString AbsolutePath = FPaths::ConvertRelativePathToFull(Result.PackagePath);
                                                      FPaths::MakePlatformFilename(AbsolutePath);
                                                      FString ExplorerArgs = FString::Printf(TEXT("/select,\"%s\""), *AbsolutePath);
                                                      FPlatformProcess::CreateProc(TEXT("explorer.exe"), *ExplorerArgs, true, false, false, nullptr, 0, nullptr, nullptr);
#else
                                                      // For other platforms, just open the folder
                                                      FString FolderPath = FPaths::GetPath(Result.PackagePath);
                                                      FPlatformProcess::ExploreFolder(*FolderPath);
#endif
                                                  }
                                                  else
                                                  {
                                                      // Failure notification
                                                      FString ErrorMessage = FString::Printf(
                                                          TEXT("Support Package Creation Failed\n\n%s"),
                                                          *Result.ErrorMessage);

                                                      NotificationItem->SetText(FText::FromString(ErrorMessage));
                                                      NotificationItem->SetExpireDuration(10.0f);
                                                      NotificationItem->SetCompletionState(SNotificationItem::CS_Fail);
                                                      NotificationItem->ExpireAndFadeout();
                                                  }
                                              });
}

TSharedRef<SWidget> SWindowControlsPanel::BuildAccountMenu(TSharedPtr<FSettingsDropdownHoverState> HoverState)
{
    // Build the account menu with an invisible backdrop to detect clicks outside
    return SNew(SBox)
        .Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
            [SNew(SAccountMenu)
                 .Username(CachedUsername)
                 .Email(CachedEmail)
                 .OnManageAccountClicked_Lambda([this, HoverState]()
                                                {
                          // Close menu
                          if (HoverState->anchor.IsValid())
                          {
                              HoverState->anchor->SetIsOpen(false);
                          }

                          // Open manage account URL
                          FPlatformProcess::LaunchURL(TEXT("https://convai.com/management/profile"), nullptr, nullptr); })
                 .OnSignOutClicked_Lambda([this, HoverState]()
                                          {
                          // Close menu
                          if (HoverState->anchor.IsValid())
                          {
                              HoverState->anchor->SetIsOpen(false);
                          }

                          // Execute sign out callback if bound
                          if (OnSignOutClicked.IsBound())
                          {
                              OnSignOutClicked.Execute();
                          } })];
}

void SWindowControlsPanel::FetchUserAccountData()
{
    auto ConfigResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
    if (ConfigResult.IsFailure())
    {
        return;
    }

    TSharedPtr<IConfigurationService> ConfigService = ConfigResult.GetValue();

    FConvaiUserInfo UserInfo;
    if (ConfigService->GetUserInfo(UserInfo))
    {
        CachedUsername = UserInfo.Username;
        CachedEmail = UserInfo.Email;
        Invalidate(EInvalidateWidget::Layout);
    }
}

void SWindowControlsPanel::OnCheckForUpdatesClicked()
{

    // Resolve update check service
    auto UpdateServiceResult = FConvaiDIContainerManager::Get().Resolve<IUpdateCheckService>();
    if (UpdateServiceResult.IsFailure())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("SWindowControlsPanel: update check service unavailable"));

        // Show error notification
        FNotificationInfo Info(LOCTEXT("UpdateCheckServiceError", "Failed to access update service"));
        Info.ExpireDuration = 5.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
        return;
    }

    TSharedPtr<IUpdateCheckService> UpdateService = UpdateServiceResult.GetValue();

    // Show checking notification
    FNotificationInfo Info(LOCTEXT("CheckingForUpdates", "Checking for updates...\n\nPlease wait while we check GitHub for the latest version."));
    Info.bFireAndForget = false;
    Info.FadeOutDuration = 0.5f;
    Info.ExpireDuration = 10.0f;

    TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
    if (NotificationItem.IsValid())
    {
        NotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
    }

    // Perform async update check (force refresh to get latest data)
    UpdateService->CheckForUpdatesAsync(true).Next([UpdateService, NotificationItem](FUpdateCheckResult Result)
                                                   { AsyncTask(ENamedThreads::GameThread, [UpdateService, NotificationItem, Result]()
                                                               {
            if (!NotificationItem.IsValid())
            {
                return;
            }

            if (Result.IsSuccess())
            {
                if (Result.bUpdateAvailable)
                {
                    const bool bIsPreRelease = Result.LatestRelease.bIsPreRelease;

                    FString Message;
                    FText HyperlinkText;

                    if (bIsPreRelease)
                    {
                        // Pre-release (beta, RC, etc.) – Professional cautionary message
                        // Following industry standards (VS Code, Chrome, etc.)
                        Message = FString::Printf(
                            TEXT("Pre-release Version Available\n\n")
                            TEXT("Current: v%s\n")
                            TEXT("Available: v%s (Pre-release)\n\n")
                            TEXT("Pre-release versions may contain bugs and are not recommended for production use.\n\n")
                            TEXT("Click 'View Release Notes' for details."),
                            *Result.CurrentVersion.VersionString,
                            *Result.LatestVersion.VersionString);

                        HyperlinkText = LOCTEXT("ViewPreReleaseNotes", "View Release Notes");
                    }
                    else
                    {
                        // Stable release - Professional positive message
                        Message = FString::Printf(
                            TEXT("Update Available\n\n")
                            TEXT("Current version: v%s\n")
                            TEXT("Latest stable version: v%s\n\n")
                            TEXT("A new stable version is available with improvements and bug fixes.\n\n")
                            TEXT("Click 'View Release' to see what's new."),
                            *Result.CurrentVersion.VersionString,
                            *Result.LatestVersion.VersionString);

                        HyperlinkText = LOCTEXT("ViewStableRelease", "View Release");
                    }

                    NotificationItem->SetText(FText::FromString(Message));
                    NotificationItem->SetCompletionState(SNotificationItem::CS_Success);

                    // Add hyperlink to open the specific release page
                    // If we have the specific release URL from GitHub API, use it (preferred)
                    // Otherwise, fall back to the general releases page
                    FString ReleaseUrlToOpen = Result.LatestRelease.ReleaseUrl;
                    if (ReleaseUrlToOpen.IsEmpty())
                    {
                        UE_LOG(LogConvaiEditor, Warning, TEXT("SWindowControlsPanel: release URL unavailable, using general releases page"));
                        NotificationItem->SetHyperlink(
                            FSimpleDelegate::CreateLambda([UpdateService]()
                            {
                                UpdateService->OpenReleasesPage();
                            }),
                            HyperlinkText);
                    }
                    else
                    {
                        NotificationItem->SetHyperlink(
                            FSimpleDelegate::CreateLambda([ReleaseUrlToOpen]()
                            {
                                FPlatformProcess::LaunchURL(*ReleaseUrlToOpen, nullptr, nullptr);
                            }),
                            HyperlinkText);
                    }

                    // Mark this version as acknowledged so user doesn't see it again
                    // This persists across editor restarts (saved to ConvaiEditorSettings.ini)
                    UpdateService->AcknowledgeUpdate(Result.LatestVersion.VersionString);
                }
                else
                {
                    // Up to date - Professional confirmation message
                    FString Message = FString::Printf(
                        TEXT("You're Up to Date\n\n")
                        TEXT("Current version: v%s\n\n")
                        TEXT("You have the latest stable version installed."),
                        *Result.CurrentVersion.VersionString);

                    NotificationItem->SetText(FText::FromString(Message));
                    NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
                }

                NotificationItem->SetExpireDuration(10.0f);
                NotificationItem->ExpireAndFadeout();
            }
            else
            {
                // Error occurred
                FString ErrorMessage = FString::Printf(
                    TEXT("Update Check Failed\n\n%s"),
                    *Result.GetStatusMessage());

                NotificationItem->SetText(FText::FromString(ErrorMessage));
                NotificationItem->SetCompletionState(SNotificationItem::CS_Fail);
                NotificationItem->SetExpireDuration(10.0f);
                NotificationItem->ExpireAndFadeout();

                UE_LOG(LogConvaiEditor, Warning, TEXT("SWindowControlsPanel: update check failed: %s"), *Result.ErrorMessage);
            } }); });
}

void SWindowControlsPanel::OnUpdateAvailabilityChanged(bool bUpdateAvailable, const FString &LatestVersion)
{

    RefreshUpdateBadge();
}

void SWindowControlsPanel::RefreshUpdateBadge()
{
    if (!UpdateBadge.IsValid())
    {
        return;
    }

    // Resolve update check service
    auto UpdateServiceResult = FConvaiDIContainerManager::Get().Resolve<IUpdateCheckService>();
    if (UpdateServiceResult.IsFailure())
    {
        // Service not available, hide badge
        UpdateBadge->Hide();
        return;
    }

    TSharedPtr<IUpdateCheckService> UpdateService = UpdateServiceResult.GetValue();

    // Show or hide badge based on update availability
    if (UpdateService->IsUpdateAvailable())
    {
        FUpdateCheckResult LastResult = UpdateService->GetLastCheckResult();
        FString LatestVersion = UpdateService->GetLatestVersionString();

        // Professional tooltip with release type distinction
        FText TooltipText;
        if (LastResult.LatestRelease.bIsPreRelease)
        {
            TooltipText = FText::Format(
                LOCTEXT("PreReleaseAvailableTooltip", "Pre-release available: v{0}\n\nClick Settings > Check for Updates for details."),
                FText::FromString(LatestVersion));
        }
        else
        {
            TooltipText = FText::Format(
                LOCTEXT("StableUpdateAvailableTooltip", "Stable update available: v{0}\n\nClick Settings > Check for Updates to learn more."),
                FText::FromString(LatestVersion));
        }

        UpdateBadge->SetToolTipText(TooltipText);
        UpdateBadge->Show(true);
    }
    else
    {
        UpdateBadge->Hide();
    }
}

SWindowControlsPanel::~SWindowControlsPanel()
{
    // CRITICAL: Slate may already be shutdown - unregistering after shutdown causes access violations
    if (FSlateApplication::IsInitialized())
    {
        UnregisterAllMenuDetectors();
    }
    else
    {
        ActiveMenuDetectors.Empty();
    }
}

void SWindowControlsPanel::RegisterMenuClickOutside(TSharedPtr<SMenuAnchor> MenuAnchor, TSharedPtr<FSettingsDropdownHoverState> HoverState, const FString &MenuWidgetTypeName)
{
    if (!MenuAnchor.IsValid() || !HoverState.IsValid())
    {
        return;
    }

    TWeakPtr<SMenuAnchor> WeakAnchor = MenuAnchor;
    if (ActiveMenuDetectors.Contains(WeakAnchor))
    {
        return;
    }

    TSharedPtr<FMenuClickOutsideDetector> Detector = MakeShared<FMenuClickOutsideDetector>(HoverState, MenuWidgetTypeName);
    FSlateApplication::Get().RegisterInputPreProcessor(Detector);
    ActiveMenuDetectors.Add(WeakAnchor, Detector);
}

void SWindowControlsPanel::UnregisterMenuClickOutside(TSharedPtr<SMenuAnchor> MenuAnchor)
{
    if (!MenuAnchor.IsValid())
    {
        return;
    }

    TWeakPtr<SMenuAnchor> WeakAnchor = MenuAnchor;
    TSharedPtr<FMenuClickOutsideDetector> Detector;
    
    if (ActiveMenuDetectors.RemoveAndCopyValue(WeakAnchor, Detector))
    {
        if (Detector.IsValid())
        {
            FSlateApplication::Get().UnregisterInputPreProcessor(Detector);
        }
    }
}

void SWindowControlsPanel::UnregisterAllMenuDetectors()
{
    for (auto &Pair : ActiveMenuDetectors)
    {
        if (Pair.Value.IsValid())
        {
            FSlateApplication::Get().UnregisterInputPreProcessor(Pair.Value);
        }
    }

    ActiveMenuDetectors.Empty();
}

bool FMenuClickOutsideDetector::HandleMouseButtonDownEvent(FSlateApplication &SlateApp, const FPointerEvent &MouseEvent)
{
    if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
    {
        return false;
    }

    TSharedPtr<FSettingsDropdownHoverState> HoverStatePinned = HoverState.Pin();
    if (!HoverStatePinned.IsValid() || !HoverStatePinned->anchor.IsValid())
    {
        return false;
    }

    if (!HoverStatePinned->anchor->IsOpen())
    {
        return false;
    }

    FVector2D CursorPos = MouseEvent.GetScreenSpacePosition();
    TSharedPtr<SMenuAnchor> Anchor = HoverStatePinned->anchor;
    
    if (!Anchor.IsValid())
    {
        return false;
    }

    FGeometry AnchorGeometry = Anchor->GetTickSpaceGeometry();
    if (AnchorGeometry.IsUnderLocation(CursorPos))
    {
        return false;
    }

    FWidgetPath WidgetsUnderCursor = SlateApp.LocateWindowUnderMouse(
        CursorPos,
        SlateApp.GetInteractiveTopLevelWindows(),
        false);

    if (WidgetsUnderCursor.IsValid())
    {
        for (int32 i = 0; i < WidgetsUnderCursor.Widgets.Num(); ++i)
        {
            TSharedRef<SWidget> Widget = WidgetsUnderCursor.Widgets[i].Widget;

            if (Widget == Anchor)
            {
                return false;
            }
            
            if (!MenuWidgetTypeName.IsEmpty() && Widget->GetTypeAsString().Contains(MenuWidgetTypeName))
            {
                return false;
            }
        }
    }

    HoverStatePinned->anchor->SetIsOpen(false);
    return false;
}

#undef LOCTEXT_NAMESPACE