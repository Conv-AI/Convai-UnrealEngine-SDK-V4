/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SHomePage.h
 *
 * Main dashboard page with action cards and announcements.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Pages/SBasePage.h"
#include "MVVM/HomePageViewModel.h"
#include "Services/YouTubeService.h"

class SWidget;
class STextBlock;
class SScrollBox;
class SImage;
class SBorder;
class SBox;
class SGridPanel;
class SScaleBox;
class SCard;
class SContentContainer;
struct FSlateBrush;
struct FYouTubeVideoInfo;

#define LOCTEXT_NAMESPACE "SHomePage"

/**
 * Main dashboard page with action cards and announcements.
 */
class CONVAIEDITOR_API SHomePage : public SBasePage
{
public:
    SLATE_BEGIN_ARGS(SHomePage) {}
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);
    virtual ~SHomePage();

    virtual TSharedPtr<FViewModelBase> GetViewModel() const override
    {
        return HomePageViewModel;
    }

    static FName StaticClass()
    {
        static FName TypeName = FName("SHomePage");
        return TypeName;
    }

    virtual bool IsA(const FName &TypeName) const override
    {
        return TypeName == StaticClass() || SBasePage::IsA(TypeName);
    }

private:
    TSharedRef<SWidget> CreateMainLayout();
    TSharedRef<SWidget> CreateActionCardsSection();
    TSharedRef<SWidget> CreateRightSidebar();
    TSharedRef<SWidget> CreateActionCard(
        const FText &Title,
        const FText &Description,
        const FSlateBrush *BackgroundImage,
        FOnClicked OnClickedDelegate);
    TSharedRef<SWidget> CreateConfigurationsCardWithComingSoon();
    TSharedRef<SWidget> CreateYouTubeVideoCard();
    TSharedRef<SWidget> CreateAnnouncementsSection();
    TSharedRef<SWidget> CreateChangelogsSection();
    TSharedRef<SWidget> CreateYouTubeThumbnailTestSection();
    TSharedRef<SWidget> CreateCharactersInLevelSection();
    TSharedRef<SWidget> CreateAnnouncementItem(const FText &Title, const FText &Description, bool bIsNew = false);
    TSharedRef<SWidget> CreateDynamicAnnouncementItem(const struct FConvaiAnnouncementItem &Item);
    TSharedRef<SWidget> CreateChangelogItemsList(const TArray<FString> &Changes, const FTextBlockStyle &TextStyle, int32 MaxItems = 0);
    TSharedRef<SWidget> CreateCharacterItem(const FText &CharacterName, bool bHasActiveFeatures = true);
    TSharedRef<SWidget> CreateCharacterScrollBox();

    FReply OnDashboardCardClicked();
    FReply OnConfigurationsCardClicked();
    FReply OnVideoCardClicked();
    FReply OnExperiencesCardClicked();
    void OnYouTubeVideoCardClicked(const FYouTubeVideoInfo &VideoInfo);

    const FSlateBrush *GetYouTubeThumbnailBrush(const FString &ThumbnailURL) const;
    const FSlateBrush *GetYouTubeThumbnailBrushCached() const;

    TSharedPtr<class FHomePageViewModel> HomePageViewModel;
    TSharedPtr<class FCharacterDashboardViewModel> DashboardViewModel;
    TSharedPtr<class FAnnouncementViewModel> AnnouncementViewModel;
    TSharedPtr<class FChangelogViewModel> ChangelogViewModel;

    mutable const FSlateBrush *CachedYouTubeThumbnailBrush;
    mutable FString CachedThumbnailURL;
    bool bShowConfigComingSoonInfo = false;

    TSharedPtr<SVerticalBox> ChangelogContentBox;
    TSharedPtr<SVerticalBox> AnnouncementContentBox;

    void HandleChangelogViewModelInvalidated();
    void HandleAnnouncementViewModelInvalidated();
    void RefreshChangelogContent();
    void RefreshAnnouncementContent();

    FDelegateHandle ChangelogInvalidatedHandle;
    FDelegateHandle AnnouncementInvalidatedHandle;
};

#undef LOCTEXT_NAMESPACE
