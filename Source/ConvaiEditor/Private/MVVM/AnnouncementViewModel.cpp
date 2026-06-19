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

FAnnouncementViewModel::FAnnouncementViewModel(TSharedPtr<IContentFeedService> InService, bool bInEnableStaleWhileRevalidate)
    : IsLoading(false), HasError(false), ErrorMessage(TEXT("")), AnnouncementCount(0), Service(InService),
      bEnableStaleWhileRevalidate(bInEnableStaleWhileRevalidate)
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

    Future.Then([this, bForceRefresh](TFuture<FContentFeedResult> ResultFuture)
                {
        FContentFeedResult Result = ResultFuture.Get();
        AsyncTask(ENamedThreads::GameThread, [this, Result, bForceRefresh]()
        {
            if (Result.bSuccess)
            {
                OnAnnouncementsLoaded(Result.AnnouncementItems, Result.bFromCache);

                if (!bForceRefresh && bEnableStaleWhileRevalidate && Result.bFromCache)
                {
                    StartBackgroundRefresh();
                }
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

void FAnnouncementViewModel::StartBackgroundRefresh()
{
    if (bBackgroundRefreshInFlight || !Service.IsValid())
    {
        return;
    }

    bBackgroundRefreshInFlight = true;

    TFuture<FContentFeedResult> RefreshFuture = Service->GetContentAsync(true);
    RefreshFuture.Then([this](TFuture<FContentFeedResult> ResultFuture)
                       {
        const FContentFeedResult Result = ResultFuture.Get();
        AsyncTask(ENamedThreads::GameThread, [this, Result]()
        {
            bBackgroundRefreshInFlight = false;

            if (!Result.bSuccess)
            {
                UE_LOG(LogConvaiEditorConfig, Verbose, TEXT("Background announcements refresh failed - %s"), *Result.ErrorMessage);
                return;
            }

            const TArray<FConvaiAnnouncementItem> FilteredItems = FContentFilteringUtility::FilterAnnouncements(Result.AnnouncementItems);
            if (AreAnnouncementsEqual(Announcements, FilteredItems))
            {
                return;
            }

            Announcements = FilteredItems;
            AnnouncementCount.Set(Announcements.Num());
            HasError.Set(false);
            ErrorMessage.Set(TEXT(""));
            BroadcastInvalidated();
        }); });
}

bool FAnnouncementViewModel::AreAnnouncementsEqual(const TArray<FConvaiAnnouncementItem> &A, const TArray<FConvaiAnnouncementItem> &B) const
{
    if (A.Num() != B.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        const FConvaiAnnouncementItem &L = A[Index];
        const FConvaiAnnouncementItem &R = B[Index];
        if (L.ID != R.ID ||
            L.Title != R.Title ||
            L.Description != R.Description ||
            L.URL != R.URL ||
            L.Date != R.Date ||
            L.Priority != R.Priority)
        {
            return false;
        }
    }

    return true;
}

void FAnnouncementViewModel::ClearAnnouncements()
{
    bBackgroundRefreshInFlight = false;
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
