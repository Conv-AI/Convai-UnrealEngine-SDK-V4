/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HomePageViewModel.h
 *
 * ViewModel for the homepage.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ViewModel.h"
#include "MVVM/ObservableProperty.h"
#include "Services/IYouTubeService.h"
#include "Services/YouTubeTypes.h"
#include "Events/EventAggregator.h"

/**
 * Data structure representing a single announcement item.
 */
struct CONVAIEDITOR_API FAnnouncement
{
    /** Title of the announcement */
    FString Title;

    /** Description or content of the announcement */
    FString Description;

    /** Whether this is a new announcement */
    bool bIsNew;

    /** Date of the announcement */
    FDateTime Date;

    FAnnouncement()
        : bIsNew(false), Date(FDateTime::Now())
    {
    }

    FAnnouncement(const FString &InTitle, const FString &InDescription, bool bInIsNew = false)
        : Title(InTitle), Description(InDescription), bIsNew(bInIsNew), Date(FDateTime::Now())
    {
    }

    bool operator==(const FAnnouncement &Other) const
    {
        return Title == Other.Title && Description == Other.Description && bIsNew == Other.bIsNew;
    }

    bool operator!=(const FAnnouncement &Other) const
    {
        return !(*this == Other);
    }
};

/**
 * Data structure representing a single changelog entry.
 */
struct CONVAIEDITOR_API FChangelogEntry
{
    /** Version number */
    FString Version;

    /** List of changes or features */
    TArray<FString> Changes;

    /** Date of the release */
    FDateTime Date;

    FChangelogEntry()
        : Date(FDateTime::Now())
    {
    }

    FChangelogEntry(const FString &InVersion, const TArray<FString> &InChanges)
        : Version(InVersion), Changes(InChanges), Date(FDateTime::Now())
    {
    }

    bool operator==(const FChangelogEntry &Other) const
    {
        return Version == Other.Version && Changes == Other.Changes;
    }

    bool operator!=(const FChangelogEntry &Other) const
    {
        return !(*this == Other);
    }
};

/**
 * Data structure representing a character in the current level.
 */
struct CONVAIEDITOR_API FCharacterInLevel
{
    /** Name of the character */
    FString Name;

    /** Whether the character has active features */
    bool bHasActiveFeatures;

    /** Character status (active, inactive, etc.) */
    FString Status;

    FCharacterInLevel()
        : bHasActiveFeatures(true), Status(TEXT("Active"))
    {
    }

    FCharacterInLevel(const FString &InName, bool bInHasActiveFeatures = true, const FString &InStatus = TEXT("Active"))
        : Name(InName), bHasActiveFeatures(bInHasActiveFeatures), Status(InStatus)
    {
    }

    bool operator==(const FCharacterInLevel &Other) const
    {
        return Name == Other.Name && bHasActiveFeatures == Other.bHasActiveFeatures && Status == Other.Status;
    }

    bool operator!=(const FCharacterInLevel &Other) const
    {
        return !(*this == Other);
    }
};

/**
 * ViewModel for the homepage.
 */
class CONVAIEDITOR_API FHomePageViewModel : public FViewModelBase
{
public:
    FHomePageViewModel();
    virtual ~FHomePageViewModel() = default;

    /** Initialize the ViewModel and load initial data */
    virtual void Initialize() override;

    /** Shutdown and cleanup resources */
    virtual void Shutdown() override;

    /** Get the list of announcements */
    const TArray<FAnnouncement> &GetAnnouncements() const { return Announcements.Get(); }

    /** Get the list of changelog entries */
    const TArray<FChangelogEntry> &GetChangelogs() const { return Changelogs.Get(); }

    /** Get the list of characters in level */
    const TArray<FCharacterInLevel> &GetCharactersInLevel() const { return CharactersInLevel.Get(); }

    /** Refresh announcements from API */
    void RefreshAnnouncements();

    /** Refresh changelogs */
    void RefreshChangelogs();

    /** Refresh characters in level */
    void RefreshCharactersInLevel();

    /** Refresh YouTube video information */
    void RefreshYouTubeVideo();

    /** Force refresh all content (manual trigger) */
    void ForceRefreshAllContent();

    /** Get the latest YouTube video info */
    TOptional<FYouTubeVideoInfo> GetLatestYouTubeVideo() const { return LatestYouTubeVideo.Get(); }

    /** Observable properties - using new Observable Pattern Extensions */
    ConvaiEditor::TObservableProperty<TArray<FAnnouncement>> Announcements;
    ConvaiEditor::TObservableProperty<TArray<FChangelogEntry>> Changelogs;
    ConvaiEditor::TObservableProperty<TArray<FCharacterInLevel>> CharactersInLevel;
    ConvaiEditor::TObservableProperty<TOptional<FYouTubeVideoInfo>> LatestYouTubeVideo;
    FObservableBool bIsLoadingAnnouncements;
    FObservableBool bIsLoadingChangelogs;
    FObservableBool bIsLoadingCharacters;
    FObservableBool bIsLoadingYouTubeVideo;

private:
    /** Load default/mock data for announcements */
    void LoadMockAnnouncements();

    /** Load default/mock data for changelogs */
    void LoadMockChangelogs();

    /** Load default/mock data for characters */
    void LoadMockCharacters();

    /** Handle API response for announcements (placeholder for future implementation) */
    void HandleAnnouncementsResponse(const TArray<FAnnouncement> &NewAnnouncements);

    /** Handle YouTube video fetch success */
    void HandleYouTubeVideoFetched(const FYouTubeVideoInfo &VideoInfo);

    /** Handle YouTube video fetch failure */
    void HandleYouTubeVideoFetchFailed(const FString &ErrorMessage);

    /** YouTube service instance */
    TSharedPtr<IYouTubeService> YouTubeService;

    /** Event subscription for network restoration (RAII-based automatic cleanup) */
    ConvaiEditor::FEventSubscription NetworkRestoredSubscription;
};
