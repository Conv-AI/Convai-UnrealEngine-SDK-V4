/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HomePageViewModel.cpp
 *
 * Implementation of the home page view model.
 */

#include "MVVM/HomePageViewModel.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/IYouTubeService.h"
#include "Services/YouTubeTypes.h"
#include "Utility/ConvaiValidationUtils.h"
#include "Events/EventAggregator.h"
#include "Events/EventTypes.h"

FHomePageViewModel::FHomePageViewModel()
    : FViewModelBase()
{
    bIsLoadingAnnouncements.Set(false);
    bIsLoadingChangelogs.Set(false);
    bIsLoadingCharacters.Set(false);
    bIsLoadingYouTubeVideo.Set(false);
}

void FHomePageViewModel::Initialize()
{
    FViewModelBase::Initialize();

    FConvaiValidationUtils::ResolveServiceWithCallbacks<IYouTubeService>(
        TEXT("FHomePageViewModel::Initialize"),
        [this](TSharedPtr<IYouTubeService> Service)
        {
            YouTubeService = Service;
        },
        [](const FString &Error)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService resolution failed - %s"), *Error);
        });

    TWeakPtr<FHomePageViewModel> WeakViewModel = SharedThis(this);
    NetworkRestoredSubscription = ConvaiEditor::FEventAggregator::Get().Subscribe<ConvaiEditor::FNetworkRestoredEvent>(
        WeakViewModel,
        [WeakViewModel](const ConvaiEditor::FNetworkRestoredEvent &Event)
        {
            if (TSharedPtr<FHomePageViewModel> ViewModel = WeakViewModel.Pin())
            {
                ViewModel->ForceRefreshAllContent();
            }
        });

    LoadMockAnnouncements();
    LoadMockChangelogs();
    LoadMockCharacters();
    RefreshYouTubeVideo();
}

void FHomePageViewModel::Shutdown()
{
    NetworkRestoredSubscription.Unsubscribe();
    FViewModelBase::Shutdown();
}

void FHomePageViewModel::RefreshAnnouncements()
{
    bIsLoadingAnnouncements.Set(true);
    LoadMockAnnouncements();
    bIsLoadingAnnouncements.Set(false);
}

void FHomePageViewModel::RefreshChangelogs()
{
    bIsLoadingChangelogs.Set(true);
    LoadMockChangelogs();
    bIsLoadingChangelogs.Set(false);
}

void FHomePageViewModel::RefreshCharactersInLevel()
{
    bIsLoadingCharacters.Set(true);
    LoadMockCharacters();
    bIsLoadingCharacters.Set(false);
}

void FHomePageViewModel::LoadMockAnnouncements()
{
    TArray<FAnnouncement> MockAnnouncements;

    MockAnnouncements.Add(FAnnouncement(
        TEXT("Convai Unreal Engine 4.5.0 Version Released!"),
        TEXT("New features and improvements now available"),
        true));

    MockAnnouncements.Add(FAnnouncement(
        TEXT("Narrative Design Tutorial Released!"),
        TEXT("Learn how to create compelling AI-driven narratives"),
        true));

    MockAnnouncements.Add(FAnnouncement(
        TEXT("New Avatar Studio Features Available!"),
        TEXT("Enhanced character creation tools and assets"),
        true));

    MockAnnouncements.Add(FAnnouncement(
        TEXT("Community Showcase Event"),
        TEXT("Join us for the monthly community showcase"),
        false));

    MockAnnouncements.Add(FAnnouncement(
        TEXT("Performance Optimization Update"),
        TEXT("Improved performance for large-scale conversations"),
        false));

    Announcements.Set(MockAnnouncements);
}

void FHomePageViewModel::LoadMockChangelogs()
{
    TArray<FChangelogEntry> MockChangelogs;

    TArray<FString> Changes400;
    Changes400.Add(TEXT("Hands Free Conversation"));
    Changes400.Add(TEXT("SDK Revamp"));
    Changes400.Add(TEXT("Bug Fixes"));
    Changes400.Add(TEXT("Sample Scenes"));
    Changes400.Add(TEXT("Editor Configuration Window"));

    MockChangelogs.Add(FChangelogEntry(TEXT("4.0.0"), Changes400));

    TArray<FString> Changes350;
    Changes350.Add(TEXT("New Character Animation System"));
    Changes350.Add(TEXT("Improved Voice Recognition"));
    Changes350.Add(TEXT("Enhanced UI/UX"));
    Changes350.Add(TEXT("Performance Optimizations"));

    MockChangelogs.Add(FChangelogEntry(TEXT("3.5.0"), Changes350));

    TArray<FString> Changes340;
    Changes340.Add(TEXT("Multi-language Support"));
    Changes340.Add(TEXT("New Avatar Templates"));
    Changes340.Add(TEXT("Bug Fixes"));

    MockChangelogs.Add(FChangelogEntry(TEXT("3.4.0"), Changes340));

    Changelogs.Set(MockChangelogs);
}

void FHomePageViewModel::LoadMockCharacters()
{
    TArray<FCharacterInLevel> MockCharacters;

    MockCharacters.Add(FCharacterInLevel(TEXT("Giovanni"), true, TEXT("Active")));
    MockCharacters.Add(FCharacterInLevel(TEXT("Mike"), true, TEXT("Active")));
    MockCharacters.Add(FCharacterInLevel(TEXT("Paulista"), true, TEXT("Active")));
    MockCharacters.Add(FCharacterInLevel(TEXT("Alice"), true, TEXT("Active")));
    MockCharacters.Add(FCharacterInLevel(TEXT("Isabelle"), true, TEXT("Active")));

    CharactersInLevel.Set(MockCharacters);
}

void FHomePageViewModel::RefreshYouTubeVideo()
{
    if (!YouTubeService.IsValid())
    {
        return;
    }

    bIsLoadingYouTubeVideo.Set(true);

    TOptional<FYouTubeVideoInfo> CachedVideo = YouTubeService->GetCachedVideoInfo();
    if (CachedVideo.IsSet())
    {
        LatestYouTubeVideo.Set(CachedVideo);
        bIsLoadingYouTubeVideo.Set(false);
        return;
    }

    TWeakPtr<FHomePageViewModel> WeakViewModel = SharedThis(this);

    YouTubeService->FetchLatestVideo(
        TEXT("convai"),
        FOnYouTubeVideoFetched::CreateLambda([WeakViewModel](const FYouTubeVideoInfo &VideoInfo)
                                             { 
                                                 if (TSharedPtr<FHomePageViewModel> ViewModel = WeakViewModel.Pin())
                                                 {
                                                     ViewModel->HandleYouTubeVideoFetched(VideoInfo);
                                                 } }),
        FOnYouTubeVideoFetchFailed::CreateLambda([WeakViewModel](const FString &ErrorMessage)
                                                 { 
                                                     if (TSharedPtr<FHomePageViewModel> ViewModel = WeakViewModel.Pin())
                                                     {
                                                         ViewModel->HandleYouTubeVideoFetchFailed(ErrorMessage);
                                                     } }));
}

void FHomePageViewModel::ForceRefreshAllContent()
{
    RefreshAnnouncements();
    RefreshYouTubeVideo();
}

void FHomePageViewModel::HandleYouTubeVideoFetched(const FYouTubeVideoInfo &VideoInfo)
{
    LatestYouTubeVideo.Set(VideoInfo);
    bIsLoadingYouTubeVideo.Set(false);
}

void FHomePageViewModel::HandleYouTubeVideoFetchFailed(const FString &ErrorMessage)
{
    bIsLoadingYouTubeVideo.Set(false);
    LatestYouTubeVideo.Set(TOptional<FYouTubeVideoInfo>());
}

void FHomePageViewModel::HandleAnnouncementsResponse(const TArray<FAnnouncement> &NewAnnouncements)
{
    Announcements.Set(NewAnnouncements);
}
