/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SHeaderBar.cpp
 *
 * Implementation of the header bar widget.
 */

#include "UI/Header/SHeaderBar.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "Services/NavigationService.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ConfigurationService.h"
#include "Services/IWelcomeWindowManager.h"
#include "ConvaiEditor.h"
#include "Services/IAuthWindowManager.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "SlateOptMacros.h"
#include "UI/Dropdown/SConvaiDropdown.h"
#include "UI/Dropdown/SConvaiNestedDropdown.h"
#include "UI/Utility/HoverAwareMenuWrapper.h"
#include "IWebBrowserCookieManager.h"
#include "IWebBrowserSingleton.h"
#include "WebBrowserModule.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Services/Routes.h"
#include "Utility/ConvaiConstants.h"
#include "UI/Components/SWindowControlsPanel.h"
#include "UI/Components/SVerticalDivider.h"
#include "UI/Components/SDevInfoBox.h"

#define LOCTEXT_NAMESPACE "SHeaderBar"

void FDropdownHoverState::Shutdown()
{
    ClearTicker();
}

void FDropdownHoverState::ClearTicker()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
}

void FDropdownHoverState::CloseIfNotHovered()
{
    if (anchor.IsValid() && anchor->IsOpen() && !bAnchorHovered && !bMenuHovered)
    {
        anchor->SetIsOpen(false);
    }
}

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

void SHeaderBar::Construct(const FArguments &InArgs)
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    const float PadTop = ConvaiEditor::Constants::Layout::Spacing::HeaderPaddingTop;
    const float PadBot = ConvaiEditor::Constants::Layout::Spacing::HeaderPaddingBottom;

    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();
        if (NavService.IsValid())
        {
            RouteChangedHandle = NavService->OnRouteChanged().AddLambda([this](ConvaiEditor::Route::E PreviousRoute, ConvaiEditor::Route::E NewRoute)
                                                                        {
                ActiveRoute = NewRoute;
                Invalidate(EInvalidateWidget::Paint); });

            ActiveRoute = NavService->GetCurrentRoute();
        }
    }

    ChildSlot
        [SNew(SBorder)
             .Padding(0)
             .BorderImage(Style.GetBrush("Convai.Color.surface.header"))
                 [SNew(SHorizontalBox)

                  + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(24, PadTop, 0, PadBot)
                            [SNew(SImage)
                                 .Image(Style.GetBrush("Convai.Logo"))]

                  + SHorizontalBox::Slot()
                        .FillWidth(1.f)
                        .HAlign(HAlign_Center)
                        .VAlign(VAlign_Center)
                        .Padding(0, PadTop, 0, PadBot)
                            [BuildNav()]

                  + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(0, PadTop, 24, PadBot)
                            [SNew(SWindowControlsPanel)
                                 .OnSettingsClicked(this, &SHeaderBar::OnSettingsClicked)
                                 .OnMinimizeClicked(this, &SHeaderBar::OnMinimizeClicked)
                                 .OnMaximizeClicked(this, &SHeaderBar::OnMaximizeClicked)
                                 .OnCloseClicked(this, &SHeaderBar::OnCloseClicked)
                                 .OnSignOutClicked(this, &SHeaderBar::OnSignOutClicked)
                                 .IsMaximized(this, &SHeaderBar::IsWindowMaximized)]]];
}

void SHeaderBar::SetupAnchorHoverBehavior(TSharedPtr<SButton> Button, FDropdownHoverState &HoverState)
{
    if (!Button.IsValid() || !HoverState.anchor.IsValid())
    {
        return;
    }

    Button->SetOnHovered(FSimpleDelegate::CreateLambda([this, &HoverState]()
                                                       {
        HoverState.bAnchorHovered = true;
        HoverState.ClearTicker();
        
        if (HoverState.anchor.IsValid() && !HoverState.anchor->IsOpen())
        {
            HoverState.anchor->SetIsOpen(true);
        } }));

    Button->SetOnUnhovered(FSimpleDelegate::CreateLambda([this, &HoverState]()
                                                         {
        HoverState.bAnchorHovered = false;
        HoverState.ClearTicker();
        
        HoverState.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([this, &HoverState](float /*DeltaTime*/) -> bool
            {
                HoverState.CloseIfNotHovered();
                HoverState.TickerHandle.Reset();
                return false;
            }),
            0.0f
        ); }));
}

bool SHeaderBar::AnchorValidAndOpen(const FName &Route) const
{
    if (Route == FName("Samples") && SamplesHoverState.anchor.IsValid())
    {
        return SamplesHoverState.anchor->IsOpen();
    }
    if (Route == FName("Features") && FeaturesHoverState.anchor.IsValid())
    {
        return FeaturesHoverState.anchor->IsOpen();
    }
    return false;
}

TSharedRef<SWidget> SHeaderBar::MakeNavItem(ConvaiEditor::Route::E Route, const FText &Label, const FSlateBrush *Icon)
{
    const FName RouteName(ConvaiEditor::Route::ToString(Route));

    TSharedRef<SButton> NavItem = SNew(SButton)
                                      .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
                                      .ContentPadding(FMargin(12, 0))
                                      .OnClicked_Lambda([this, Route]()
                                                        {
            auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
            if (NavResult.IsSuccess())
            {
                TSharedPtr<INavigationService> Nav = NavResult.GetValue();
                if (Nav.IsValid())
                {
                    Nav->Navigate(Route);
                }
                else
                {
                    UE_LOG(LogConvaiEditor, Error, TEXT("SHeaderBar: NavigationService resolved but is invalid"));
                }
            }
            else
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("SHeaderBar: failed to resolve NavigationService - %s"), *NavResult.GetError());
            }
            return FReply::Handled(); })
                                          [Icon != nullptr
                                               ? StaticCastSharedRef<SWidget>(
                                                     SNew(SImage)
                                                         .Image(Icon)
                                                         .ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([this, Route, RouteName]()
                                                                                                                {
                    const ISlateStyle& ST = FConvaiStyle::Get();
                    if (Route == ActiveRoute)                      return ST.GetColor("Convai.Color.navActive");
                    if (AnchorValidAndOpen(RouteName))             return ST.GetColor("Convai.Color.navActive");
                    if (NavWidgets.Contains(RouteName) && NavWidgets[RouteName].IsValid() && NavWidgets[RouteName]->IsHovered()) return ST.GetColor("Convai.Color.navHover");
                    return ST.GetColor("Convai.Color.navText"); })))
                                               : StaticCastSharedRef<SWidget>(
                                                     SNew(STextBlock)
                                                         .Text(Label)
                                                         .Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.nav"))
                                                         .ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([this, Route, RouteName]()
                                                                                                                {
                    const ISlateStyle& ST = FConvaiStyle::Get();
                    if (Route == ActiveRoute)                      return ST.GetColor("Convai.Color.navActive");
                    if (AnchorValidAndOpen(RouteName))             return ST.GetColor("Convai.Color.navActive");
                    if (NavWidgets.Contains(RouteName) && NavWidgets[RouteName].IsValid() && NavWidgets[RouteName]->IsHovered()) return ST.GetColor("Convai.Color.navHover");
                    return ST.GetColor("Convai.Color.navText"); })))];

    NavWidgets.Add(RouteName, NavItem);
    return NavItem;
}

TSharedRef<SWidget> SHeaderBar::BuildNav()
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    const float SeparatorThickness = ConvaiEditor::Constants::Layout::Components::Separator::Thickness;

    TSharedRef<SHorizontalBox> NavBox = SNew(SHorizontalBox);
    bool bFirstItem = true;

    auto AddNavItem = [&](ConvaiEditor::Route::E Route, const FText &Label, const FSlateBrush *Icon = nullptr)
    {
        if (!bFirstItem)
        {
            NavBox->AddSlot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                    [SNew(SVerticalDivider)
                         .DividerType(EDividerType::HeaderNav)
                         .Thickness(SeparatorThickness)
                         .Margin(FMargin(4, 0))
                         .MinDesiredHeight(30.f)];
        }
        bFirstItem = false;

        NavBox->AddSlot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0)
                [MakeNavItem(Route, Label, Icon)];
    };

    TSharedPtr<SButton> SamplesButton;
    NavBox->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0)
            [SAssignNew(SamplesHoverState.anchor, SMenuAnchor)
                 .Method(EPopupMethod::UseCurrentWindow)
                 .UseApplicationMenuStack(false)
                 .Placement(MenuPlacement_CenteredBelowAnchor)
                 .OnGetMenuContent(this, &SHeaderBar::BuildSamplesDropdown)
                 .OnMenuOpenChanged_Lambda([this](bool bOpen)
                                           { Invalidate(EInvalidateWidget::Layout); })
                     [SAssignNew(SamplesButton, SButton)
                          .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
                          .ContentPadding(FMargin(12, 0))
                              [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0)[SNew(STextBlock).Text(LOCTEXT("NavSamples", "Samples")).Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.nav")).ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([this]()
                                                                                                                                                                                                                                                                                                     {
                            const ISlateStyle& ST = FConvaiStyle::Get();
                            if (SamplesHoverState.anchor.IsValid() && SamplesHoverState.anchor->IsOpen())                 return ST.GetColor("Convai.Color.navActive");
                            if (NavWidgets.Contains(FName("Samples")) && NavWidgets[FName("Samples")].IsValid() && NavWidgets[FName("Samples")]->IsHovered()) return ST.GetColor("Convai.Color.navHover");
                            return ST.GetColor("Convai.Color.navText"); }))]]]];

    SetupAnchorHoverBehavior(SamplesButton, SamplesHoverState);

    NavWidgets.Add(FName("Samples"), SamplesButton);

    if (!bFirstItem)
    {
        NavBox->AddSlot()
            .AutoWidth()
            .VAlign(VAlign_Center)
                [SNew(SVerticalDivider)
                     .DividerType(EDividerType::HeaderNav)
                     .Thickness(SeparatorThickness)
                     .Margin(FMargin(4, 0))
                     .MinDesiredHeight(30.f)];
    }
    bFirstItem = false;

    TSharedPtr<SButton> FeaturesButton;
    NavBox->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0)
            [SAssignNew(FeaturesHoverState.anchor, SMenuAnchor)
                 .Method(EPopupMethod::UseCurrentWindow)
                 .UseApplicationMenuStack(false)
                 .Placement(MenuPlacement_CenteredBelowAnchor)
                 .OnGetMenuContent(this, &SHeaderBar::BuildFeaturesDropdown)
                 .OnMenuOpenChanged_Lambda([this](bool bOpen)
                                           { Invalidate(EInvalidateWidget::Layout); })
                     [SAssignNew(FeaturesButton, SButton)
                          .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
                          .ContentPadding(FMargin(12, 0))
                              [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0)[SNew(STextBlock).Text(LOCTEXT("NavFeatures", "Features")).Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.nav")).ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([this]()
                                                                                                                                                                                                                                                                                                       {
                            const ISlateStyle& ST = FConvaiStyle::Get();
                            if (FeaturesHoverState.anchor.IsValid() && FeaturesHoverState.anchor->IsOpen())                 return ST.GetColor("Convai.Color.navActive");
                            if (NavWidgets.Contains(FName("Features")) && NavWidgets[FName("Features")].IsValid() && NavWidgets[FName("Features")]->IsHovered()) return ST.GetColor("Convai.Color.navHover");
                            return ST.GetColor("Convai.Color.navText"); }))]]]];

    SetupAnchorHoverBehavior(FeaturesButton, FeaturesHoverState);

    NavWidgets.Add(FName("Features"), FeaturesButton);

    AddNavItem(ConvaiEditor::Route::E::Home, FText::GetEmpty(), Style.GetBrush("Convai.Icon.Home"));
    AddNavItem(ConvaiEditor::Route::E::Account, LOCTEXT("NavAccount", "Account"));
    AddNavItem(ConvaiEditor::Route::E::Support, LOCTEXT("NavSupport", "Support"));

    return NavBox;
}

TSharedRef<SWidget> SHeaderBar::BuildDropdownMenu(
    const TArray<FConvaiMenuEntry> &Entries,
    const FName &FontStyle,
    FDropdownHoverState &HoverState)
{
    // Use the Nav font style if not specified
    const FName EffectiveFontStyle =
        FontStyle.IsNone() ? FName("Convai.Font.dropdown") : FontStyle;

    return SNew(SHoverAwareMenuWrapper)
        .OnMenuHoverStart(FSimpleDelegate::CreateLambda([&HoverState]()
                                                        {
               HoverState.bMenuHovered = true;
               HoverState.ClearTicker(); }))
        .OnMenuHoverEnd(FSimpleDelegate::CreateLambda([&HoverState]()
                                                      {
               HoverState.bMenuHovered = false;
               HoverState.CloseIfNotHovered(); }))
            [SNew(SConvaiDropdown)
                 .Entries(Entries)
                 .FontStyle(EffectiveFontStyle)];
}

TSharedRef<SWidget> SHeaderBar::BuildNestedDropdownMenu(
    const TArray<FConvaiMenuEntry> &Entries,
    const FName &FontStyle,
    FDropdownHoverState &HoverState)
{
    // Use the Nav font style if not specified
    const FName EffectiveFontStyle =
        FontStyle.IsNone() ? FName("Convai.Font.dropdown") : FontStyle;

    return SNew(SHoverAwareMenuWrapper)
        .OnMenuHoverStart(FSimpleDelegate::CreateLambda([&HoverState]()
                                                        {
               HoverState.bMenuHovered = true;
               HoverState.ClearTicker(); }))
        .OnMenuHoverEnd(FSimpleDelegate::CreateLambda([&HoverState]()
                                                      {
               HoverState.bMenuHovered = false;
               HoverState.CloseIfNotHovered(); }))
            [SNew(SConvaiNestedDropdown)
                 .Entries(Entries)
                 .FontStyle(EffectiveFontStyle)
                 .NestingLevel(0) // Start at root level
    ];
}

TSharedRef<SWidget> SHeaderBar::BuildSamplesDropdown()
{
    const ISlateStyle &Style = FConvaiStyle::Get();

    return SNew(SHoverAwareMenuWrapper)
        .OnMenuHoverStart(FSimpleDelegate::CreateLambda([this]()
                                                        {
               SamplesHoverState.bMenuHovered = true;
               SamplesHoverState.ClearTicker(); }))
        .OnMenuHoverEnd(FSimpleDelegate::CreateLambda([this]()
                                                      {
               SamplesHoverState.bMenuHovered = false;
               SamplesHoverState.CloseIfNotHovered(); }))
            [SNew(SDevInfoBox)
                 .Emoji(TEXT("🚧"))
                 .InfoText(LOCTEXT("SamplesInDev", "Coming Soon! Sample projects and templates will be available here."))
                 .bWrapText(false)];
}

TSharedRef<SWidget> SHeaderBar::BuildFeaturesDropdown()
{
    const ISlateStyle &Style = FConvaiStyle::Get();

    return SNew(SHoverAwareMenuWrapper)
        .OnMenuHoverStart(FSimpleDelegate::CreateLambda([this]()
                                                        {
               FeaturesHoverState.bMenuHovered = true;
               FeaturesHoverState.ClearTicker(); }))
        .OnMenuHoverEnd(FSimpleDelegate::CreateLambda([this]()
                                                      {
               FeaturesHoverState.bMenuHovered = false;
               FeaturesHoverState.CloseIfNotHovered(); }))
            [SNew(SDevInfoBox)
                 .Emoji(TEXT("🚧"))
                 .InfoText(LOCTEXT("FeaturesInDev", "Coming Soon! Advanced features like Actions, Narrative Design, and more will be available here."))];
}

SHeaderBar::~SHeaderBar()
{
    SamplesHoverState.Shutdown();
    FeaturesHoverState.Shutdown();

    NavWidgets.Empty();

    if (RouteChangedHandle.IsValid())
    {
        auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
        if (NavResult.IsSuccess())
        {
            TSharedPtr<INavigationService> NavService = NavResult.GetValue();
            if (NavService.IsValid())
            {
                NavService->OnRouteChanged().Remove(RouteChangedHandle);
            }
        }
    }
}

void SHeaderBar::OnSettingsClicked()
{
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> Nav = NavResult.GetValue();
        if (Nav.IsValid())
        {
            Nav->Navigate(ConvaiEditor::Route::E::Settings);
        }
    }
}

void SHeaderBar::OnMinimizeClicked()
{
    TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
    if (Window.IsValid())
    {
        Window->Minimize();
    }
}

void SHeaderBar::OnMaximizeClicked()
{
    TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
    if (Window.IsValid())
    {
        if (Window->IsWindowMaximized())
        {
            Window->Restore();
        }
        else
        {
            Window->Maximize();
        }
    }
}

void SHeaderBar::OnCloseClicked()
{
    if (!FSlateApplication::IsInitialized())
    {
        return;
    }
    
    TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
    if (Window.IsValid())
    {
        Window->RequestDestroyWindow();
    }
}

void SHeaderBar::OnSignOutClicked()
{
    auto ConfigResult = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
    if (ConfigResult.IsSuccess())
    {
        TSharedPtr<IConfigurationService> ConfigService = ConfigResult.GetValue();
        ConfigService->ClearAuthentication();
    }

    if (FModuleManager::Get().IsModuleLoaded("WebBrowser"))
    {
        IWebBrowserSingleton *WebBrowserSingleton = IWebBrowserModule::Get().GetSingleton();
        if (WebBrowserSingleton)
        {
            TSharedPtr<IWebBrowserCookieManager> CookieManager = WebBrowserSingleton->GetCookieManager();
            if (CookieManager.IsValid())
            {
                CookieManager->DeleteCookies();
            }
        }
    }

    /*
    FString ProjectSavedDir = FPaths::ProjectSavedDir();
    TArray<FString> WebCacheDirs;
    IFileManager::Get().FindFiles(WebCacheDirs, *(ProjectSavedDir / TEXT("webcache*")), false, true);

    for (const FString &DirName : WebCacheDirs)
    {
        FString FullPath = ProjectSavedDir / DirName;
        IFileManager::Get().DeleteDirectory(*FullPath, false, true);
    }
    */

    {
        auto AuthManagerResult = FConvaiDIContainerManager::Get().Resolve<IAuthWindowManager>();
        if (AuthManagerResult.IsSuccess())
        {
            TSharedPtr<IAuthWindowManager> AuthManager = AuthManagerResult.GetValue();
            if (AuthManager.IsValid())
            {
                AuthManager->OnAuthCancelled();
            }
        }
    }

    if (FSlateApplication::IsInitialized())
    {
        TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
        if (Window.IsValid())
        {
            Window->RequestDestroyWindow();
        }
    }
}

bool SHeaderBar::IsWindowMaximized() const
{
    TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
    return Window.IsValid() && Window->IsWindowMaximized();
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION

#undef LOCTEXT_NAMESPACE
