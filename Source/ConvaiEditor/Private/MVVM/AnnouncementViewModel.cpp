/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AnnouncementViewModel.cpp
 *
 * Implementation of the announcement view model.
 */

#include "MVVM/AnnouncementViewModel.h"
#include "ConvaiEditor.h"
#include "Services/IContentFeedService.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Utility/ContentFilteringUtility.h"
#include "Async/Async.h"
#include "Events/EventAggregator.h"
#include "Events/EventTypes.h"

FAnnouncementViewModel::FAnnouncementViewModel(TSharedPtr<IContentFeedService> InService)
    : IsLoading(false), HasError(false), ErrorMessage(TEXT("")), AnnouncementCount(0), Service(InService)
{
    if (!Service.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("AnnouncementService is unavailable - AnnouncementViewModel disabled"));
    }
}

FAnnouncementViewModel::~FAnnouncementViewModel()
{
}

void FAnnouncementViewModel::Initialize()
{
    FViewModelBase::Initialize();

    TWeakPtr<FAnnouncementViewModel> WeakViewModel = SharedThis(this);
    NetworkRestoredSubscription = ConvaiEditor::FEventAggregator::Get().Subscribe<ConvaiEditor::FNetworkRestoredEvent>(
        WeakViewModel,
        [WeakViewModel](const ConvaiEditor::FNetworkRestoredEvent &Event)
        {
            if (TSharedPtr<FAnnouncementViewModel> ViewModel = WeakViewModel.Pin())
            {
                ViewModel->RefreshAnnouncements();
            }
        });

    if (Service.IsValid())
    {
        LoadAnnouncements(false);
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Cannot initialize AnnouncementViewModel - service unavailable"));
        OnAnnouncementsLoadFailed(TEXT("Announcement service not available"));
    }
}

void FAnnouncementViewModel::Shutdown()
{
    NetworkRestoredSubscription.Unsubscribe();

    ClearAnnouncements();

    FViewModelBase::Shutdown();
}

void FAnnouncementViewModel::RefreshAnnouncements()
{
    LoadAnnouncements(true);
}

void FAnnouncementViewModel::LoadAnnouncements(bool bForceRefresh)
{
    if (!Service.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Cannot load announcements - service unavailable"));
        OnAnnouncementsLoadFailed(TEXT("Service not available"));
        return;
    }

    IsLoading.Set(true);
    HasError.Set(false);
    ErrorMessage.Set(TEXT(""));

    TFuture<FContentFeedResult> Future = Service->GetContentAsync(bForceRefresh);

    Future.Then([this](TFuture<FContentFeedResult> ResultFuture)
                {
        FContentFeedResult Result = ResultFuture.Get();
        AsyncTask(ENamedThreads::GameThread, [this, Result]()
        {
            if (Result.bSuccess)
            {
                    OnAnnouncementsLoaded(Result.AnnouncementItems, Result.bFromCache);
            }
            else
            {
                OnAnnouncementsLoadFailed(Result.ErrorMessage);
            }
        }); });
}

void FAnnouncementViewModel::OnAnnouncementsLoaded(const TArray<FConvaiAnnouncementItem> &Items, bool bWasFromCache)
{
    TArray<FConvaiAnnouncementItem> FilteredItems = FContentFilteringUtility::FilterAnnouncements(Items);

    Announcements = FilteredItems;

    IsLoading.Set(false);
    HasError.Set(false);
    ErrorMessage.Set(TEXT(""));
    AnnouncementCount.Set(Announcements.Num());

    BroadcastInvalidated();
}

void FAnnouncementViewModel::OnAnnouncementsLoadFailed(const FString &Error)
{
    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Failed to load announcements - %s"), *Error);

    IsLoading.Set(false);
    HasError.Set(true);
    ErrorMessage.Set(Error);

    BroadcastInvalidated();
}

void FAnnouncementViewModel::ClearAnnouncements()
{
    Announcements.Empty();
    AnnouncementCount.Set(0);
    HasError.Set(false);
    ErrorMessage.Set(TEXT(""));
    IsLoading.Set(false);

    BroadcastInvalidated();
}

TArray<FConvaiAnnouncementItem> FAnnouncementViewModel::GetAnnouncementsByType(EAnnouncementType Type) const
{
    TArray<FConvaiAnnouncementItem> Filtered;

    for (const FConvaiAnnouncementItem &Item : Announcements)
    {
        if (Item.Type == Type)
        {
            Filtered.Add(Item);
        }
    }

    return Filtered;
}

double FAnnouncementViewModel::GetCacheAge() const
{
    if (!Service.IsValid())
    {
        return -1.0;
    }

    return Service->GetCacheAge();
}
