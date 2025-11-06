/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ChangelogViewModel.cpp
 *
 * Implementation of the changelog view model.
 */

#include "MVVM/ChangelogViewModel.h"
#include "ConvaiEditor.h"
#include "Services/IContentFeedService.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Utility/ContentFilteringUtility.h"
#include "Async/Async.h"
#include "Events/EventAggregator.h"
#include "Events/EventTypes.h"

FChangelogViewModel::FChangelogViewModel(TSharedPtr<IContentFeedService> InService)
    : Service(InService), IsLoading(false), HasError(false), ErrorMessage(TEXT("")), ChangelogCount(0)
{
    if (!Service.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ChangelogViewModel: Service is null - ViewModel will not function"));
    }
}

FChangelogViewModel::~FChangelogViewModel()
{
}

void FChangelogViewModel::Initialize()
{
    FViewModelBase::Initialize();

    TWeakPtr<FChangelogViewModel> WeakViewModel = SharedThis(this);
    NetworkRestoredSubscription = ConvaiEditor::FEventAggregator::Get().Subscribe<ConvaiEditor::FNetworkRestoredEvent>(
        WeakViewModel,
        [WeakViewModel](const ConvaiEditor::FNetworkRestoredEvent &Event)
        {
            if (TSharedPtr<FChangelogViewModel> ViewModel = WeakViewModel.Pin())
            {
                ViewModel->RefreshChangelogs();
            }
        });

    if (Service.IsValid())
    {
        LoadChangelogs(false);
    }
    else
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ChangelogViewModel: Cannot initialize - service unavailable"));
        OnChangelogsLoadFailed(TEXT("Changelog service not available"));
    }
}

void FChangelogViewModel::Shutdown()
{
    NetworkRestoredSubscription.Unsubscribe();

    ClearChangelogs();

    FViewModelBase::Shutdown();
}

void FChangelogViewModel::RefreshChangelogs()
{
    LoadChangelogs(true);
}

void FChangelogViewModel::LoadChangelogs(bool bForceRefresh)
{
    if (!Service.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ChangelogViewModel: Cannot load changelogs - service unavailable"));
        OnChangelogsLoadFailed(TEXT("Service not available"));
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
                OnChangelogsLoaded(Result.ChangelogItems, Result.bFromCache);
            }
            else
            {
                OnChangelogsLoadFailed(Result.ErrorMessage);
            }
        }); });
}

void FChangelogViewModel::OnChangelogsLoaded(const TArray<FConvaiChangelogItem> &Items, bool bWasFromCache)
{
    TArray<FConvaiChangelogItem> FilteredItems = FContentFilteringUtility::FilterChangelogs(Items);

    Changelogs = FilteredItems;

    IsLoading.Set(false);
    HasError.Set(false);
    ErrorMessage.Set(TEXT(""));
    ChangelogCount.Set(Changelogs.Num());

    BroadcastInvalidated();
}

void FChangelogViewModel::OnChangelogsLoadFailed(const FString &Error)
{
    UE_LOG(LogConvaiEditorConfig, Warning, TEXT("ChangelogViewModel: Failed to load changelogs - %s"), *Error);

    IsLoading.Set(false);
    HasError.Set(true);
    ErrorMessage.Set(Error);

    BroadcastInvalidated();
}

void FChangelogViewModel::ClearChangelogs()
{
    Changelogs.Empty();
    ChangelogCount.Set(0);
    HasError.Set(false);
    ErrorMessage.Set(TEXT(""));
    IsLoading.Set(false);

    BroadcastInvalidated();
}

double FChangelogViewModel::GetCacheAge() const
{
    if (!Service.IsValid())
    {
        return -1.0;
    }

    return Service->GetCacheAge();
}
