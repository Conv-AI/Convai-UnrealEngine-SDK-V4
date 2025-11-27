/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SHomePage.cpp
 *
 * Implementation of the home page.
 */

#include "UI/Pages/SHomePage.h"
#include "MVVM/HomePageViewModel.h"
#include "MVVM/SamplesViewModel.h"
#include "MVVM/AnnouncementViewModel.h"
#include "MVVM/ChangelogViewModel.h"
#include "Models/ConvaiAnnouncementData.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"
#include "Services/NavigationService.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/Routes.h"
#include "UI/Pages/SWebBrowserPage.h"
#include "UI/Factories/PageFactoryManager.h"
#include "ConvaiEditor.h"
#include "UI/Widgets/SCard.h"
#include "UI/Widgets/SContentContainer.h"
#include "UI/Utility/ConvaiWidgetFactory.h"
#include "UI/Components/SDevInfoBox.h"
#include "UI/Utility/HoverColorHelper.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Brushes/SlateColorBrush.h"
#include "HAL/PlatformProcess.h"
#include "UI/Widgets/SConvaiScrollBox.h"
#include "HttpModule.h"

// Version-specific style includes
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#else
#include "EditorStyleSet.h"
#endif
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "RenderUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Utility/ConvaiURLs.h"
#include "Services/YouTubeTypes.h"
#include "UI/Pages/SCharacterDashboard.h"
#include "MVVM/CharacterDashboardViewModel.h"
#include "MVVM/ViewModel.h"
#include "Editor.h" // For GEditor
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "SHomePage"

void SHomePage::Construct(const FArguments &InArgs)
{
    CachedYouTubeThumbnailBrush = nullptr;
    CachedThumbnailURL = TEXT("");

    HomePageViewModel = FViewModelRegistry::Get().CreateScopedViewModel<FHomePageViewModel>();
    if (HomePageViewModel.IsValid())
    {
        HomePageViewModel->Initialize();
    }
    else
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: failed to create scoped HomePageViewModel - no active scope"));
    }

    DashboardViewModel = FViewModelRegistry::Get().CreateScopedViewModel<FCharacterDashboardViewModel>();
    if (DashboardViewModel.IsValid())
    {
        DashboardViewModel->Initialize();

        if (GEditor)
        {
            UWorld *EditorWorld = GEditor->GetEditorWorldContext().World();
            if (EditorWorld)
            {
                DashboardViewModel->RefreshCharacterList(EditorWorld);
            }
            else
            {
                UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: EditorWorld unavailable, character list refresh skipped"));
            }
        }
        else
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: GEditor unavailable, character list refresh skipped"));
        }
    }

    AnnouncementViewModel = FViewModelRegistry::Get().ResolveViewModel<FAnnouncementViewModel>();
    if (!AnnouncementViewModel.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: AnnouncementViewModel not found in registry"));
    }

    ChangelogViewModel = FViewModelRegistry::Get().ResolveViewModel<FChangelogViewModel>();
    if (!ChangelogViewModel.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: ChangelogViewModel not found in registry"));
    }
    else
    {
        ChangelogInvalidatedHandle = ChangelogViewModel->OnInvalidated().AddSP(this, &SHomePage::HandleChangelogViewModelInvalidated);
    }

    if (AnnouncementViewModel.IsValid())
    {
        AnnouncementInvalidatedHandle = AnnouncementViewModel->OnInvalidated().AddSP(this, &SHomePage::HandleAnnouncementViewModelInvalidated);
    }

    SBasePage::Construct(SBasePage::FArguments()
                             [CreateMainLayout()]);
}

SHomePage::~SHomePage()
{
    if (ChangelogViewModel.IsValid() && ChangelogInvalidatedHandle.IsValid())
    {
        ChangelogViewModel->OnInvalidated().Remove(ChangelogInvalidatedHandle);
        ChangelogInvalidatedHandle.Reset();
    }

    if (AnnouncementViewModel.IsValid() && AnnouncementInvalidatedHandle.IsValid())
    {
        AnnouncementViewModel->OnInvalidated().Remove(AnnouncementInvalidatedHandle);
        AnnouncementInvalidatedHandle.Reset();
    }
}

TSharedRef<SWidget> SHomePage::CreateMainLayout()
{
    const float ContentSpacing = ConvaiEditor::Constants::Layout::Spacing::Content;
    const FMargin ContentPadding = FMargin(ContentSpacing);

    const FVector2D CardDimensions = ConvaiEditor::Constants::Layout::Components::HomePageCard::Dimensions;
    const float CardSpacing = ConvaiEditor::Constants::Layout::Spacing::HomePageCardSpacing;

    const float CardsMinWidth = (CardDimensions.X * 2) + CardSpacing;
    const float SidebarWidth = ConvaiEditor::Constants::Layout::Window::HomePageSidebarWidth;
    const float SidebarSpacing = ConvaiEditor::Constants::Layout::Spacing::HomePageSidebarSpacing;

    return SNew(SBox)
        .Clipping(EWidgetClipping::ClipToBounds)
            [SNew(SScaleBox)
                 .Stretch(EStretch::ScaleToFit)
                 .StretchDirection(EStretchDirection::Both)
                     [SNew(SHorizontalBox) + SHorizontalBox::Slot()
                                                 .FillWidth(1.0f)
                                                 .HAlign(HAlign_Center)
                                                 .VAlign(VAlign_Center)
                                                 .Padding(ContentPadding)
                                                     [SNew(SHorizontalBox)

                                                      + SHorizontalBox::Slot()
                                                            .AutoWidth()
                                                            .VAlign(VAlign_Top)
                                                                [SNew(SBox)
                                                                     .WidthOverride(CardsMinWidth)
                                                                         [CreateActionCardsSection()]]

                                                      + SHorizontalBox::Slot()
                                                            .AutoWidth()
                                                            .VAlign(VAlign_Top)
                                                            .Padding(SidebarSpacing, 0, 0, 0)
                                                                [SNew(SBox)
                                                                     .WidthOverride(SidebarWidth)
                                                                         [CreateRightSidebar()]]]]];
}

TSharedRef<SWidget> SHomePage::CreateActionCardsSection()
{
    const float CardSpacing = ConvaiEditor::Constants::Layout::Spacing::HomePageCardSpacing;
    const FVector2D CardDimensions = ConvaiEditor::Constants::Layout::Components::HomePageCard::Dimensions;

    const float VerticalSpacing = CardSpacing * 0.6f;

    return SNew(SVerticalBox)

           + SVerticalBox::Slot()
                 .AutoHeight()
                 .VAlign(VAlign_Top)
                 .Padding(0, 0, 0, VerticalSpacing)
                     [SNew(SHorizontalBox)

                      + SHorizontalBox::Slot()
                            .AutoWidth()
                            .Padding(0, 0, CardSpacing * 0.5f, 0)
                                [FConvaiWidgetFactory::CreateSizedBox(
                                    CreateActionCard(
                                        LOCTEXT("DashboardTitle", "Dashboard"),
                                        LOCTEXT("DashboardDesc", "View overview and analytics"),
                                        FConvaiStyle::Get().GetBrush("Convai.HomePage.Dashboard"),
                                        FOnClicked::CreateSP(this, &SHomePage::OnDashboardCardClicked)),
                                    CardDimensions)]

                      + SHorizontalBox::Slot()
                            .AutoWidth()
                            .Padding(CardSpacing * 0.5f, 0, 0, 0)
                                [FConvaiWidgetFactory::CreateSizedBox(
                                    CreateConfigurationsCardWithComingSoon(),
                                    CardDimensions)]]

           + SVerticalBox::Slot()
                 .AutoHeight()
                 .VAlign(VAlign_Top)
                 .Padding(0, VerticalSpacing, 0, 0)
                     [SNew(SHorizontalBox)

                      + SHorizontalBox::Slot()
                            .AutoWidth()
                            .Padding(0, 0, CardSpacing * 0.5f, 0)
                                [FConvaiWidgetFactory::CreateSizedBox(
                                    CreateYouTubeVideoCard(),
                                    CardDimensions)]

                      + SHorizontalBox::Slot()
                            .AutoWidth()
                            .Padding(CardSpacing * 0.5f, 0, 0, 0)
                                [FConvaiWidgetFactory::CreateSizedBox(
                                    CreateActionCard(
                                        LOCTEXT("ExperiencesTitle", "Convai Experiences"),
                                        LOCTEXT("ExperiencesDesc", "Explore sample experiences and demos"),
                                        FConvaiStyle::Get().GetBrush("Convai.HomePage.Experiences"),
                                        FOnClicked::CreateSP(this, &SHomePage::OnExperiencesCardClicked)),
                                    CardDimensions)]];
}

TSharedRef<SWidget> SHomePage::CreateRightSidebar()
{
    const float ContentSpacing = ConvaiEditor::Constants::Layout::Spacing::Content;

    return SNew(SVerticalBox)

           // Announcements section
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(0, 0, 0, ContentSpacing)
                     [CreateAnnouncementsSection()]

           // Changelogs section
           + SVerticalBox::Slot()
                 .AutoHeight()
                 .Padding(0, 0, 0, ContentSpacing)
                     [CreateChangelogsSection()]

           // YouTube Thumbnail Test section removed - thumbnail now integrated into main card

           // Characters in level section
           + SVerticalBox::Slot()
                 .AutoHeight()
                     [CreateCharactersInLevelSection()];
}

TSharedRef<SWidget> SHomePage::CreateActionCard(
    const FText &Title,
    const FText &Description,
    const FSlateBrush *BackgroundImage,
    FOnClicked OnClickedDelegate)
{
    const FVector2D CardDimensions = ConvaiEditor::Constants::Layout::Components::HomePageCard::Dimensions;

    TSharedPtr<FSampleItem> FakeSampleItem = MakeShared<FSampleItem>();
    FakeSampleItem->Name = Title;
    FakeSampleItem->Tags.Add(Description.ToString());

    if (BackgroundImage == FConvaiStyle::Get().GetBrush("Convai.HomePage.Dashboard"))
    {
        FakeSampleItem->ImagePath = ConvaiEditor::Constants::Images::HomePage::Dashboard;
    }
    else if (BackgroundImage == FConvaiStyle::Get().GetBrush("Convai.HomePage.Configurations"))
    {
        FakeSampleItem->ImagePath = ConvaiEditor::Constants::Images::HomePage::Configurations;
    }
    else if (BackgroundImage == FConvaiStyle::Get().GetBrush("Convai.HomePage.Experiences"))
    {
        FakeSampleItem->ImagePath = ConvaiEditor::Constants::Images::HomePage::ConvaiExperiences;
    }
    else if (BackgroundImage == FConvaiStyle::Get().GetBrush("Convai.Support.YoutubeTutorials"))
    {
        FakeSampleItem->ImagePath = ConvaiEditor::Constants::Images::Support::YoutubeTutorials;
    }
    else if (BackgroundImage == FConvaiStyle::Get().GetBrush("Convai.Support.Documentation"))
    {
        FakeSampleItem->ImagePath = ConvaiEditor::Constants::Images::Support::Documentation;
    }
    else if (BackgroundImage == FConvaiStyle::Get().GetBrush("Convai.Support.ConvaiDeveloperForum"))
    {
        FakeSampleItem->ImagePath = ConvaiEditor::Constants::Images::Support::ConvaiDeveloperForum;
    }

    return SNew(SCard)
        .SampleItem(FakeSampleItem)
        .DisplayMode(ECardDisplayMode::HomepageSimple)
        .CustomTitleFontSize(24.0f)
        .OnClicked(OnClickedDelegate);
}

TSharedRef<SWidget> SHomePage::CreateConfigurationsCardWithComingSoon()
{
    const FVector2D CardDimensions = ConvaiEditor::Constants::Layout::Components::HomePageCard::Dimensions;

    TSharedRef<SWidget> ConfigCard = CreateActionCard(
        LOCTEXT("ConfigurationsTitle", "Configurations"),
        LOCTEXT("ConfigurationsDesc", "Manage settings and preferences"),
        FConvaiStyle::Get().GetBrush("Convai.HomePage.Configurations"),
        FOnClicked::CreateSP(this, &SHomePage::OnConfigurationsCardClicked));

    return SNew(SOverlay)

           + SOverlay::Slot()
                 .HAlign(HAlign_Fill)
                 .VAlign(VAlign_Fill)
                     [ConfigCard]

           + SOverlay::Slot()
                 .HAlign(HAlign_Fill)
                 .VAlign(VAlign_Center)
                 .Padding(FMargin(2, 0, 2, 0))
                     [SNew(SBox)
                          .MinDesiredWidth(CardDimensions.X - 4.0f)
                          .Visibility_Lambda([this]() -> EVisibility
                                             { return bShowConfigComingSoonInfo ? EVisibility::Visible : EVisibility::Collapsed; })
                              [SNew(SDevInfoBox)
                                   .Emoji(TEXT("🚧"))
                                   .InfoText(LOCTEXT("ConfigurationsComingSoon", "Coming Soon! Advanced configuration options will be available here."))
                                   .WrapTextAt(380.0f)]];
}

TSharedRef<SWidget> SHomePage::CreateAnnouncementsSection()
{
    const FText SectionTitle = LOCTEXT("AnnouncementsTitle", "Announcements");
    const float ContentSpacing = ConvaiEditor::Constants::Layout::Spacing::Content;

    // Create and cache the content box for dynamic updates
    TSharedRef<SWidget> Container = SNew(SContentContainer)
                                        .Title(SectionTitle)
                                        .ContentPadding(FMargin(16, 12))
                                        .BackgroundColor(FConvaiStyle::RequireColor("Convai.Color.surface.header")) // Theme header background (#0C0C0C)
                                        .BorderColor(FConvaiStyle::RequireColor("Convai.Color.border.accent"))      // Theme accent border (#1FB755)
                                        .BorderRadius(12.0f)
                                            [SNew(SBox)
                                                 [SNew(SScrollBox)
                                                      .Style(&FConvaiStyle::GetScrollBoxStyle(false))
                                                      .ScrollBarStyle(&FConvaiStyle::GetScrollBarStyle())
                                                      .ScrollBarAlwaysVisible(false)
                                                      .AllowOverscroll(EAllowOverscroll::No)

                                                  + SScrollBox::Slot()
                                                        [SAssignNew(AnnouncementContentBox, SVerticalBox)]]];

    // Populate initial content
    RefreshAnnouncementContent();

    return Container;
}

void SHomePage::RefreshAnnouncementContent()
{
    if (!AnnouncementContentBox.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: AnnouncementContentBox is invalid, cannot refresh"));
        return;
    }

    // Clear existing content
    AnnouncementContentBox->ClearChildren();

    const float ContentSpacing = ConvaiEditor::Constants::Layout::Spacing::Content;

    // Check if we have ViewModel and announcements
    if (AnnouncementViewModel.IsValid() && AnnouncementViewModel->HasAnnouncements())
    {
        const TArray<FConvaiAnnouncementItem> &Announcements = AnnouncementViewModel->GetAnnouncements();

        const int32 MaxDisplayCount = FMath::Min(
            ConvaiEditor::Constants::Layout::Components::HomePageSidebar::MaxAnnouncementsDisplay,
            Announcements.Num());

        for (int32 i = 0; i < MaxDisplayCount; ++i)
        {
            const FConvaiAnnouncementItem &Item = Announcements[i];

            if (i == 0)
            {
                AnnouncementContentBox->AddSlot()
                    .AutoHeight()
                        [CreateDynamicAnnouncementItem(Item)];
            }
            else
            {
                AnnouncementContentBox->AddSlot()
                    .AutoHeight()
                    .Padding(0, ContentSpacing * 0.5f, 0, 0)
                        [CreateDynamicAnnouncementItem(Item)];
            }
        }
    }
    else if (AnnouncementViewModel.IsValid() && AnnouncementViewModel->IsLoading.Get())
    {
        // Show loading message
        AnnouncementContentBox->AddSlot()
            .AutoHeight()
                [SNew(STextBlock)
                     .Text(LOCTEXT("AnnouncementsLoading", "Loading announcements..."))
                     .TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
                     .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.secondary"))];
    }
    else if (AnnouncementViewModel.IsValid() && AnnouncementViewModel->HasError.Get())
    {
        // Show professional error message with icon
        AnnouncementContentBox->AddSlot()
            .AutoHeight()
            .Padding(FMargin(0, 10))
                [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 8, 0))[SNew(SImage)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                                                                                                                                  .Image(FAppStyle::GetBrush("Icons.WarningWithColor.Large"))
#else
                                                                                                                                  .Image(FEditorStyle::GetBrush("Icons.WarningWithColor.Large"))
#endif
                                                                                                                                  .DesiredSizeOverride(FVector2D(16, 16))] +
                 SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("AnnouncementsConnectivityIssue", "Unable to load latest announcements")).TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText")).ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4, 0, 0))[SNew(STextBlock).Text(LOCTEXT("AnnouncementsRetryMessage", "Please check your internet connection.\nContent will auto-refresh when connectivity is restored.")).TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText")).Font(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::ExtraSmall)).ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.secondary")).AutoWrapText(true)]]];
    }
    else
    {
        // Fallback to placeholder (no ViewModel or no data yet)
        AnnouncementContentBox->AddSlot()
            .AutoHeight()
                [SNew(STextBlock)
                     .Text(LOCTEXT("AnnouncementsNoData", "No announcements available"))
                     .TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
                     .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.secondary"))];
    }
}

TSharedRef<SWidget> SHomePage::CreateChangelogsSection()
{
    const FText SectionTitle = LOCTEXT("ChangelogsTitle", "Changelogs");
    const float ContentSpacing = ConvaiEditor::Constants::Layout::Spacing::Content;

    FTextBlockStyle ChangelogTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    ChangelogTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"));
    ChangelogTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::Small));

    FTextBlockStyle VersionTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    VersionTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.action.hover"));
    VersionTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::Regular));

    FTextBlockStyle LinkTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    FLinearColor LinkColor = FConvaiStyle::RequireColor("Convai.Color.border.light");
    LinkTextStyle.SetColorAndOpacity(LinkColor);
    LinkTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::ExtraSmall));

    // Create and cache the content box for dynamic updates
    TSharedRef<SWidget> Container = SNew(SContentContainer)
                                        .Title(SectionTitle)
                                        .ContentPadding(FMargin(16, 12))
                                        .BackgroundColor(FConvaiStyle::RequireColor("Convai.Color.surface.header")) // Theme header background (#0C0C0C)
                                        .BorderColor(FConvaiStyle::RequireColor("Convai.Color.border.accent"))      // Theme accent border (#1FB755)
                                        .BorderRadius(12.0f)
                                            [SAssignNew(ChangelogContentBox, SVerticalBox)];

    // Populate initial content
    RefreshChangelogContent();

    return Container;
}

void SHomePage::RefreshChangelogContent()
{
    if (!ChangelogContentBox.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: ChangelogContentBox is invalid, cannot refresh"));
        return;
    }

    // Clear existing content
    ChangelogContentBox->ClearChildren();

    FTextBlockStyle ChangelogTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    ChangelogTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"));
    ChangelogTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::Small));

    FTextBlockStyle VersionTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    VersionTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.action.hover"));
    VersionTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::Regular));

    FTextBlockStyle LinkTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    FLinearColor LinkColor = FConvaiStyle::RequireColor("Convai.Color.border.light");
    LinkTextStyle.SetColorAndOpacity(LinkColor);
    LinkTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::ExtraSmall));

    // Check if we have ViewModel and changelogs
    if (ChangelogViewModel.IsValid() && ChangelogViewModel->HasChangelogs())
    {
        const TArray<FConvaiChangelogItem> &Changelogs = ChangelogViewModel->GetChangelogs();

        if (Changelogs.Num() > 0)
        {
            const FConvaiChangelogItem &LatestChangelog = Changelogs[0];

            ChangelogContentBox->AddSlot()
                .AutoHeight()
                    [SNew(STextBlock)
                         .Text(FText::FromString(LatestChangelog.Version))
                         .TextStyle(&VersionTextStyle)];

            ChangelogContentBox->AddSlot()
                .AutoHeight()
                .Padding(0, 8, 0, 0)
                    [SNew(SBox)
                         .HeightOverride(ConvaiEditor::Constants::Layout::Components::HomePageSidebar::ChangelogContentHeight)
                             [SNew(SScrollBox)
                                  .Style(&FConvaiStyle::GetScrollBoxStyle(false))
                                  .ScrollBarStyle(&FConvaiStyle::GetScrollBarStyle())
                                  .ScrollBarAlwaysVisible(false)
                                  .ScrollBarThickness(FVector2D(
                                      ConvaiEditor::Constants::Layout::Components::HomePageSidebar::ThinScrollBarThickness,
                                      ConvaiEditor::Constants::Layout::Components::HomePageSidebar::ThinScrollBarThickness))
                                  .AllowOverscroll(EAllowOverscroll::No)
                                  .ConsumeMouseWheel(EConsumeMouseWheel::WhenScrollingPossible)

                              + SScrollBox::Slot()
                                    [CreateChangelogItemsList(LatestChangelog.Changes, ChangelogTextStyle)]]];

            TSharedPtr<STextBlock> ChangelogLinkText;
            TSharedPtr<SBorder> UnderlineBorder;
            TSharedPtr<SImage> ChangelogLinkIcon;

            TSharedRef<SHorizontalBox> ChangelogLinkContent = SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SVerticalBox)

                                                                                                                                              + SVerticalBox::Slot().AutoHeight()[SAssignNew(ChangelogLinkText, STextBlock).Text(LOCTEXT("ViewFullChangelogs", "View Full Change Logs")).TextStyle(&LinkTextStyle)]

                                                                                                                                              + SVerticalBox::Slot().AutoHeight()[SAssignNew(UnderlineBorder, SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(LinkColor).Padding(FMargin(0))[SNew(SBox).HeightOverride(1.0f)]]]

                                                              + SHorizontalBox::Slot()
                                                                    .AutoWidth()
                                                                    .VAlign(VAlign_Center)
                                                                    .Padding(4, 0, 0, 0)
                                                                        [SAssignNew(ChangelogLinkIcon, SImage)
                                                                             .Image(FConvaiStyle::Get().GetBrush("Convai.Icon.OpenExternally"))
                                                                             .DesiredSizeOverride(FVector2D(10, 10))];

            TSharedPtr<SButton> ChangelogButton;
            TSharedRef<SButton> ChangelogButtonRef = SAssignNew(ChangelogButton, SButton)
                                                         .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
                                                         .OnClicked_Lambda([URL = LatestChangelog.URL]() -> FReply
                                                                           {
                    if (!URL.IsEmpty())
                    {
                        FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
                    }
                    return FReply::Handled(); })
                                                         .ContentPadding(FMargin(0))
                                                         .Cursor(EMouseCursor::Hand)
                                                             [ChangelogLinkContent];

            TWeakPtr<SButton> WeakChangelogButton = ChangelogButton;
            ChangelogLinkText->SetColorAndOpacity(
                FHoverColorHelper::CreateHoverAwareColorExplicit(
                    WeakChangelogButton,
                    LinkColor,
                    FConvaiStyle::Get().GetColor("Convai.Color.navHover")));

            UnderlineBorder->SetBorderBackgroundColor(
                FHoverColorHelper::CreateHoverAwareColorExplicit(
                    WeakChangelogButton,
                    LinkColor,
                    FConvaiStyle::Get().GetColor("Convai.Color.navHover")));

            ChangelogLinkIcon->SetColorAndOpacity(
                FHoverColorHelper::CreateHoverAwareColorExplicit(
                    WeakChangelogButton,
                    LinkColor,
                    FConvaiStyle::Get().GetColor("Convai.Color.navHover")));

            ChangelogContentBox->AddSlot()
                .AutoHeight()
                .HAlign(HAlign_Right)
                .Padding(0, 8, 0, 0)
                    [ChangelogButtonRef];
        }
    }
    else if (ChangelogViewModel.IsValid() && ChangelogViewModel->IsLoading.Get())
    {
        ChangelogContentBox->AddSlot()
            .AutoHeight()
                [SNew(STextBlock)
                     .Text(LOCTEXT("ChangelogsLoading", "Loading changelogs..."))
                     .TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
                     .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.secondary"))];
    }
    else if (ChangelogViewModel.IsValid() && ChangelogViewModel->HasError.Get())
    {
        ChangelogContentBox->AddSlot()
            .AutoHeight()
            .Padding(FMargin(0, 10))
                [SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 8, 0))[SNew(SImage)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                                                                                                                                  .Image(FAppStyle::GetBrush("Icons.WarningWithColor.Large"))
#else
                                                                                                                                  .Image(FEditorStyle::GetBrush("Icons.WarningWithColor.Large"))
#endif
                                                                                                                                  .DesiredSizeOverride(FVector2D(16, 16))] +
                 SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("ChangelogsConnectivityIssue", "Unable to load latest changelogs")).TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText")).ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"))] + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4, 0, 0))[SNew(STextBlock).Text(LOCTEXT("ChangelogsRetryMessage", "Please check your internet connection.\nContent will auto-refresh when connectivity is restored.")).TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText")).Font(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::ExtraSmall)).ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.secondary")).AutoWrapText(true)]]];
    }
    else
    {
        ChangelogContentBox->AddSlot()
            .AutoHeight()
                [SNew(STextBlock)
                     .Text(LOCTEXT("ChangelogsNoData", "No changelogs available"))
                     .TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
                     .ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.secondary"))];
    }
}

void SHomePage::HandleChangelogViewModelInvalidated()
{
    if (!IsInGameThread())
    {
        TWeakPtr<SHomePage> WeakPage = SharedThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakPage]()
                  {
            if (TSharedPtr<SHomePage> Page = WeakPage.Pin())
            {
                if (Page->ChangelogContentBox.IsValid())
                {
                    Page->RefreshChangelogContent();
                    FSlateApplication::Get().InvalidateAllWidgets(false);
                }
            } });
    }
    else
    {
        RefreshChangelogContent();
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().InvalidateAllWidgets(false);
        }
    }
}

void SHomePage::HandleAnnouncementViewModelInvalidated()
{
    // Ensure we're on the game thread for UI operations
    if (!IsInGameThread())
    {
        TWeakPtr<SHomePage> WeakPage = SharedThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakPage]()
                  {
            if (TSharedPtr<SHomePage> Page = WeakPage.Pin())
            {
                if (Page->AnnouncementContentBox.IsValid())
                {
                    Page->RefreshAnnouncementContent();
                    FSlateApplication::Get().InvalidateAllWidgets(false);
                }
            } });
    }
    else
    {
        RefreshAnnouncementContent();
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().InvalidateAllWidgets(false);
        }
    }
}

TSharedRef<SWidget> SHomePage::CreateCharactersInLevelSection()
{
    const FText SectionTitle = LOCTEXT("CharactersInLevelTitle", "Characters in the Level");
    const FText ActiveFeatures = LOCTEXT("ActiveFeatures", "Active Features");

    // Custom text styles
    FTextBlockStyle CharacterTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    CharacterTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"));
    CharacterTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 20));

    FTextBlockStyle HeaderTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    HeaderTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.border.light"));
    HeaderTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10));

    return SNew(SBox)
        .HeightOverride(298.0f)
            [SNew(SContentContainer)
                 .Title(SectionTitle)
                 .ContentPadding(FMargin(16, 12))
                 .BackgroundColor(FConvaiStyle::RequireColor("Convai.Color.surface.header"))
                 .BorderColor(FConvaiStyle::RequireColor("Convai.Color.border.accent"))
                 .BorderRadius(12.0f)
                     [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(0.5f).HAlign(HAlign_Center).Padding(0, 0, 30, 0)[SNew(STextBlock).Text_Lambda([this]() -> FText
                                                                                                                                                                                                                                       {
                            int32 CharacterCount = DashboardViewModel.IsValid() ? DashboardViewModel->GetCharacters().Num() : 0;
                            return FText::Format(LOCTEXT("CharacterCountFmt", "{0} Characters Active"), CharacterCount); })
                                                                                                                                                                                                              .TextStyle(&HeaderTextStyle)
                                                                                                                                                                                                              .Justification(ETextJustify::Center)] +
                                                                                                 SHorizontalBox::Slot().FillWidth(0.5f).HAlign(HAlign_Center).Padding(30, 0, 0, 0)[SNew(STextBlock).Text(ActiveFeatures).TextStyle(&HeaderTextStyle).Justification(ETextJustify::Center)]] +
                      SVerticalBox::Slot()
                          .AutoHeight()
                              [SNew(SBox)
                                   .HeightOverride(220.0f)
                                       [SNew(SCharacterDashboard)
                                            .ViewModel(DashboardViewModel)]]]];
}

TSharedRef<SWidget> SHomePage::CreateYouTubeThumbnailTestSection()
{
    const FText SectionTitle = LOCTEXT("YouTubeThumbnailTestTitle", "YouTube Thumbnail Test");

    return SNew(SContentContainer)
        .Title(SectionTitle)
        .ContentPadding(FMargin(16, 12))
        .BackgroundColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0C0C0C"))))
        .BorderColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1FB755"))))
        .BorderRadius(12.0f)
            [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[SNew(STextBlock).Text(LOCTEXT("YouTubeThumbnailTestInfo", "Testing YouTube thumbnail download:")).TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText")).ColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"))] + SVerticalBox::Slot().AutoHeight()[SNew(SBox).WidthOverride(320.0f).HeightOverride(180.0f)[SNew(SImage).Image_Lambda([this]() -> const FSlateBrush *
                                                                                                                                                                                                                                                                                                                                                                                                                                                                      {
                    // Get the latest YouTube video info
                    if (HomePageViewModel.IsValid())
                    {
                        TOptional<FYouTubeVideoInfo> VideoInfo = HomePageViewModel->GetLatestYouTubeVideo();
                        if (VideoInfo.IsSet() && !VideoInfo->ThumbnailURL.IsEmpty())
                        {
                            const FSlateBrush* ThumbnailBrush = GetYouTubeThumbnailBrush(VideoInfo->ThumbnailURL);
                            if (ThumbnailBrush)
                            {
                                return ThumbnailBrush;
                            }
                        }
                    }
                    
                    return FConvaiStyle::Get().GetBrush("Convai.Support.YoutubeTutorials"); })]]

             // Status text
             + SVerticalBox::Slot()
                   .AutoHeight()
                   .Padding(0, 8, 0, 0)
                       [SNew(STextBlock)
                            .Text_Lambda([this]() -> FText
                                         {
                if (HomePageViewModel.IsValid())
                {
                    TOptional<FYouTubeVideoInfo> VideoInfo = HomePageViewModel->GetLatestYouTubeVideo();
                    if (VideoInfo.IsSet())
                    {
                        return FText::FromString(FString::Printf(TEXT("Video: %s"), *VideoInfo->Title));
                    }
                }
                return LOCTEXT("NoVideoInfo", "No video info available"); })
                            .TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
                            .ColorAndOpacity(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("BCDBC7"))))
                            .AutoWrapText(true)]];
}

TSharedRef<SWidget> SHomePage::CreateAnnouncementItem(const FText &Title, const FText &Description, bool bIsNew)
{
    FTextBlockStyle AnnouncementTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    AnnouncementTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"));
    AnnouncementTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 11));

    return SNew(SHorizontalBox)

           + SHorizontalBox::Slot()
                 .FillWidth(1.0f)
                     [SNew(STextBlock)
                          .Text(Title)
                          .TextStyle(&AnnouncementTextStyle)
                          .AutoWrapText(true)]

           + SHorizontalBox::Slot()
                 .AutoWidth()
                 .Padding(8, 0, 0, 0)
                 .VAlign(VAlign_Top)
                     [SNew(SImage)
                          .Image(FConvaiStyle::Get().GetBrush("Convai.Icon.OpenExternally"))
                          .ColorAndOpacity(FConvaiStyle::RequireColor(FName("Convai.Color.action.hover"))) // Green color from theme
                          .DesiredSizeOverride(FVector2D(16, 16))];
}

TSharedRef<SWidget> SHomePage::CreateDynamicAnnouncementItem(const FConvaiAnnouncementItem &Item)
{
    FTextBlockStyle AnnouncementTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    AnnouncementTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", ConvaiEditor::Constants::Typography::Sizes::Small));

    TSharedPtr<STextBlock> TitleText;
    TSharedPtr<SImage> IconImage;

    TSharedRef<SHorizontalBox> ContentBox = SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.0f)[SAssignNew(TitleText, STextBlock).Text(FText::FromString(Item.Title)).TextStyle(&AnnouncementTextStyle).AutoWrapText(true)]

                                            + SHorizontalBox::Slot()
                                                  .AutoWidth()
                                                  .Padding(8, 0, 0, 0)
                                                  .VAlign(VAlign_Top)
                                                      [SAssignNew(IconImage, SImage)
                                                           .Image(FConvaiStyle::Get().GetBrush("Convai.Icon.OpenExternally"))
                                                           .DesiredSizeOverride(FVector2D(16, 16))];

    TSharedPtr<SButton> AnnouncementButton;

    TSharedRef<SButton> ButtonRef = SAssignNew(AnnouncementButton, SButton)
                                        .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder")) // No background button style
                                        .OnClicked_Lambda([URL = Item.URL]() -> FReply
                                                          {
            if (!URL.IsEmpty())
            {
                FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
            }
            return FReply::Handled(); })
                                        .ContentPadding(FMargin(0))
                                        .Cursor(EMouseCursor::Hand)
                                            [ContentBox];

    TWeakPtr<SButton> WeakButton = AnnouncementButton;

    TitleText->SetColorAndOpacity(
        FHoverColorHelper::CreateHoverAwareColorFromTheme(
            WeakButton,
            FName("Convai.Color.text.primary"),
            FName("Convai.Color.navHover")));

    IconImage->SetColorAndOpacity(
        FHoverColorHelper::CreateHoverAwareColorFromTheme(
            WeakButton,
            FName("Convai.Color.action.hover"),
            FName("Convai.Color.navHover")));

    return ButtonRef;
}

TSharedRef<SWidget> SHomePage::CreateChangelogItemsList(const TArray<FString> &Changes, const FTextBlockStyle &TextStyle, int32 MaxItems)
{
    TSharedPtr<SVerticalBox> ItemsBox;
    TSharedRef<SWidget> Container = SAssignNew(ItemsBox, SVerticalBox);

    const int32 ItemsToShow = (MaxItems > 0) ? FMath::Min(MaxItems, Changes.Num()) : Changes.Num();

    for (int32 i = 0; i < ItemsToShow; ++i)
    {
        const FString &Change = Changes[i];

        if (i > 0)
        {
            ItemsBox->AddSlot()
                .AutoHeight()
                .Padding(0, 4, 0, 0)
                    [SNew(STextBlock)
                         .Text(FText::FromString(FString::Printf(TEXT("• %s"), *Change)))
                         .TextStyle(&TextStyle)
                         .AutoWrapText(true)];
        }
        else
        {
            ItemsBox->AddSlot()
                .AutoHeight()
                    [SNew(STextBlock)
                         .Text(FText::FromString(FString::Printf(TEXT("• %s"), *Change)))
                         .TextStyle(&TextStyle)
                         .AutoWrapText(true)];
        }
    }

    return Container;
}

// Event handlers for action cards
FReply SHomePage::OnDashboardCardClicked()
{
    const FString DashboardURL = FConvaiURLs::GetDashboardURL();

    // Use external browser for UE versions older than 5.7 due to outdated CEF
#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*DashboardURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();
        NavService->Navigate(ConvaiEditor::Route::E::Dashboard);
    }
    else
    {
        FPlatformProcess::LaunchURL(*DashboardURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: failed to navigate to Dashboard - opening externally: %s"), *NavResult.GetError());
    }
#endif

    return FReply::Handled();
}

FReply SHomePage::OnConfigurationsCardClicked()
{
    bShowConfigComingSoonInfo = !bShowConfigComingSoonInfo;

    return FReply::Handled();
}

FReply SHomePage::OnVideoCardClicked()
{
    const FString YouTubeURL = FConvaiURLs::GetYouTubeURL();

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*YouTubeURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();

        auto PageFactoryManagerResult = FConvaiDIContainerManager::Get().Resolve<IPageFactoryManager>();
        if (PageFactoryManagerResult.IsSuccess())
        {
            PageFactoryManagerResult.GetValue()->UpdateWebBrowserURL(ConvaiEditor::Route::E::YouTubeVideo, YouTubeURL);
        }

        NavService->Navigate(ConvaiEditor::Route::E::YouTubeVideo);
    }
    else
    {
        FPlatformProcess::LaunchURL(*YouTubeURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: failed to navigate to YouTube channel - opening externally: %s"), *NavResult.GetError());
    }
#endif

    return FReply::Handled();
}

void SHomePage::OnYouTubeVideoCardClicked(const FYouTubeVideoInfo &VideoInfo)
{
    FString VideoURL = !VideoInfo.VideoURL.IsEmpty() ? VideoInfo.VideoURL : FConvaiURLs::GetYouTubeURL();

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*VideoURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();

        auto PageFactoryManagerResult = FConvaiDIContainerManager::Get().Resolve<IPageFactoryManager>();
        if (PageFactoryManagerResult.IsSuccess())
        {
            PageFactoryManagerResult.GetValue()->UpdateWebBrowserURL(ConvaiEditor::Route::E::YouTubeVideo, VideoURL);
        }

        NavService->Navigate(ConvaiEditor::Route::E::YouTubeVideo);
    }
    else
    {
        FPlatformProcess::LaunchURL(*VideoURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: failed to navigate to YouTube video - opening externally: %s"), *NavResult.GetError());
    }
#endif
}

TSharedRef<SWidget> SHomePage::CreateYouTubeVideoCard()
{
    // Create sample item for YouTube video card
    TSharedPtr<FSampleItem> VideoSampleItem = MakeShared<FSampleItem>();

    // Always show the static title for consistency
    VideoSampleItem->Name = LOCTEXT("VideoTitle", "The Latest YouTube Video");

    // Don't set image path - we'll use dynamic image instead
    VideoSampleItem->ImagePath = TEXT("");
    
    // Add a tag to ensure proper HomePage card dimensions (Tags.Num() == 1 triggers HomePage card size)
    VideoSampleItem->Tags.Add(TEXT("Video"));

    // Use enhanced SCard with dynamic image support
    return SNew(SCard)
        .SampleItem(VideoSampleItem)
        .DisplayMode(ECardDisplayMode::HomepageSimple)
        .CustomTitleFontSize(24.0f)
        .DynamicImageBrush(TAttribute<const FSlateBrush *>::CreateSP(this, &SHomePage::GetYouTubeThumbnailBrushCached))
        .OnClicked_Lambda([this]() -> FReply
                          {
            if (HomePageViewModel.IsValid())
            {
                TOptional<FYouTubeVideoInfo> VideoInfo = HomePageViewModel->GetLatestYouTubeVideo();
                if (VideoInfo.IsSet())
                {
                    OnYouTubeVideoCardClicked(VideoInfo.GetValue());
                    return FReply::Handled();
                }
            }
            return OnVideoCardClicked(); });
}

FReply SHomePage::OnExperiencesCardClicked()
{
    const FString ExperiencesURL = FConvaiURLs::GetExperiencesURL();

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7)
    FPlatformProcess::LaunchURL(*ExperiencesURL, nullptr, nullptr);
#else
    auto NavResult = FConvaiDIContainerManager::Get().Resolve<INavigationService>();
    if (NavResult.IsSuccess())
    {
        TSharedPtr<INavigationService> NavService = NavResult.GetValue();
        NavService->Navigate(ConvaiEditor::Route::E::Experiences);
    }
    else
    {
        FPlatformProcess::LaunchURL(*ExperiencesURL, nullptr, nullptr);
        UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: failed to navigate to Experiences - opening externally: %s"), *NavResult.GetError());
    }
#endif

    return FReply::Handled();
}

const FSlateBrush *SHomePage::GetYouTubeThumbnailBrush(const FString &ThumbnailURL) const
{
    if (ThumbnailCache.Contains(ThumbnailURL))
    {
        return ThumbnailCache[ThumbnailURL].Get();
    }

    if (PendingDownloads.Contains(ThumbnailURL) && PendingDownloads[ThumbnailURL])
    {
        return nullptr;
    }

    PendingDownloads.Add(ThumbnailURL, true);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(ThumbnailURL);
    Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("User-Agent"), TEXT("UnrealEngine/ConvaiPlugin"));

    // CRITICAL: Must use SharedThis() to safely capture 'this' in async HTTP callback
    TWeakPtr<SHomePage> WeakSelf = SharedThis(const_cast<SHomePage*>(this));
    
    Request->OnProcessRequestComplete().BindLambda([WeakSelf, ThumbnailURL](FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bInWasSuccessful)
                                                   {
        TSharedPtr<SHomePage> StrongSelf = WeakSelf.Pin();
        if (!StrongSelf.IsValid())
        {
            return;
        }
        
        if (!InRequest.IsValid())
        {
            StrongSelf->PendingDownloads.Add(ThumbnailURL, false);
            UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: invalid request pointer for YouTube thumbnail: %s"), *ThumbnailURL);
            return;
        }
        
        StrongSelf->PendingDownloads.Add(ThumbnailURL, false);
        
        if (!bInWasSuccessful || !InResponse.IsValid())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: failed to download YouTube thumbnail: %s"), *ThumbnailURL);
            return;
        }

        const int32 ResponseCode = InResponse->GetResponseCode();
        if (ResponseCode != 200)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: YouTube thumbnail download returned HTTP %d: %s"), ResponseCode, *ThumbnailURL);
            return;
        }

        TArray<uint8> ImageData = InResponse->GetContent();
        if (ImageData.Num() == 0)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: YouTube thumbnail download returned empty data: %s"), *ThumbnailURL);
            return;
        }

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        
        if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(ImageData.GetData(), ImageData.Num()))
        {
            ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
            if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(ImageData.GetData(), ImageData.Num()))
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: failed to decompress YouTube thumbnail image: %s"), *ThumbnailURL);
                return;
            }
        }

        TArray<uint8> RawImageData;
        if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawImageData))
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: failed to get raw image data from YouTube thumbnail: %s"), *ThumbnailURL);
            return;
        }

        TSharedPtr<FSlateBrush> DynamicBrush;
        
        UTexture2D* Texture = UTexture2D::CreateTransient(ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), PF_B8G8R8A8);
        if (Texture)
        {
            FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
            void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
            
            FMemory::Memcpy(Data, RawImageData.GetData(), RawImageData.Num());
            Mip.BulkData.Unlock();
            
            Texture->UpdateResource();
            
            DynamicBrush = MakeShared<FSlateBrush>();
            DynamicBrush->SetResourceObject(Texture);
            DynamicBrush->ImageSize = FVector2D(320, 180);
            DynamicBrush->DrawAs = ESlateBrushDrawType::Image;
        }
        else
        {
            FLinearColor PlaceholderColor = FConvaiStyle::RequireColor(FName("Convai.Color.action.hover"));
            DynamicBrush = MakeShared<FSlateColorBrush>(PlaceholderColor);
            UE_LOG(LogConvaiEditor, Warning, TEXT("SHomePage: failed to create YouTube thumbnail texture"));
        }

        if (DynamicBrush.IsValid())
        {
            StrongSelf->ThumbnailCache.Add(ThumbnailURL, DynamicBrush);
            
            AsyncTask(ENamedThreads::GameThread, []()
            {
                if (FSlateApplication::IsInitialized())
                {
                    FSlateApplication::Get().InvalidateAllWidgets(false);
                }
            });
        } });

    if (!Request->ProcessRequest())
    {
        PendingDownloads.Add(ThumbnailURL, false);
        UE_LOG(LogConvaiEditor, Error, TEXT("SHomePage: failed to process YouTube thumbnail download request"));
    }

    return nullptr;
}

const FSlateBrush *SHomePage::GetYouTubeThumbnailBrushCached() const
{
    if (!HomePageViewModel.IsValid())
    {
        return FConvaiStyle::GetTransparentBrush();
    }

    TOptional<FYouTubeVideoInfo> VideoInfo = HomePageViewModel->GetLatestYouTubeVideo();
    if (!VideoInfo.IsSet() || VideoInfo->ThumbnailURL.IsEmpty())
    {
        return FConvaiStyle::GetTransparentBrush();
    }

    if (CachedThumbnailURL == VideoInfo->ThumbnailURL && CachedYouTubeThumbnailBrush != nullptr)
    {
        return CachedYouTubeThumbnailBrush;
    }

    CachedThumbnailURL = VideoInfo->ThumbnailURL;
    CachedYouTubeThumbnailBrush = GetYouTubeThumbnailBrush(VideoInfo->ThumbnailURL);

    return CachedYouTubeThumbnailBrush ? CachedYouTubeThumbnailBrush : FConvaiStyle::GetTransparentBrush();
}

#undef LOCTEXT_NAMESPACE
