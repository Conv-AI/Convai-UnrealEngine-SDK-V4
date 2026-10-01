/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ContentFeedService.cpp
 *
 * Implementation of content feed management service.
 */

#include "Services/ContentFeedService.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Async/Async.h"

FContentFeedService::FContentFeedService(
    TUniquePtr<IContentFeedProvider> InProvider,
    TUniquePtr<FContentFeedCacheManager> InCacheManager,
    EContentFeedType InContentType)
    : Provider(MoveTemp(InProvider)), CacheManager(MoveTemp(InCacheManager)), ContentType(InContentType)
{
    if (!Provider.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedService: Provider is null"));
    }

    if (!CacheManager.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedService: CacheManager is null"));
    }
}

TFuture<FContentFeedResult> FContentFeedService::GetContentAsync(bool bForceRefresh)
{
    if (!Provider.IsValid() || !CacheManager.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedService is not properly initialized."));
        return Async(EAsyncExecution::TaskGraphMainThread, []()
                     { return FContentFeedResult::Error(TEXT("Service not initialized")); });
    }

    if (bForceRefresh)
    {
        CacheManager->InvalidateCache();
        return FetchFromRemoteAsync();
    }

    if (ContentType == EContentFeedType::Announcements)
    {
        TOptional<FConvaiAnnouncementFeed> CachedFeed = CacheManager->GetCached();

        if (CachedFeed.IsSet())
        {
            TArray<FConvaiAnnouncementItem> Items = CachedFeed.GetValue().GetSortedAnnouncements();

            return Async(EAsyncExecution::TaskGraphMainThread, [Items]()
                         { return FContentFeedResult::Success(Items, true); });
        }
    }
    else
    {
        TOptional<FConvaiChangelogFeed> CachedFeed = CacheManager->GetCachedChangelogs();

        if (CachedFeed.IsSet())
        {
            TArray<FConvaiChangelogItem> Items = CachedFeed.GetValue().GetSortedChangelogs();

            return Async(EAsyncExecution::TaskGraphMainThread, [Items]()
                         { return FContentFeedResult::SuccessChangelog(Items, true); });
        }
    }

    return FetchFromRemoteAsync();
}

TFuture<FContentFeedResult> FContentFeedService::RefreshContentAsync()
{
    return GetContentAsync(true);
}

bool FContentFeedService::HasCachedData() const
{
    FScopeLock Lock(&ServiceMutex);

    if (!CacheManager.IsValid())
    {
        return false;
    }

    return CacheManager->IsCacheValid();
}

double FContentFeedService::GetCacheAge() const
{
    FScopeLock Lock(&ServiceMutex);

    if (!CacheManager.IsValid())
    {
        return -1.0;
    }

    return CacheManager->GetCacheAge();
}

TFuture<FContentFeedResult> FContentFeedService::FetchFromRemoteAsync()
{
    if (!Provider.IsValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error, TEXT("Provider is null in FetchFromRemoteAsync"));
        return Async(EAsyncExecution::TaskGraphMainThread, []()
                     { return FContentFeedResult::Error(TEXT("Provider not available")); });
    }

    if (!Provider->IsAvailable())
    {
        UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Content provider is not available"));
        return Async(EAsyncExecution::TaskGraphMainThread, []()
                     { return FContentFeedResult::Error(TEXT("Provider not available - check network connection")); });
    }

    TFuture<FContentFeedFetchResult> ProviderFuture = Provider->FetchContentAsync();

    return ProviderFuture.Then([this](TFuture<FContentFeedFetchResult> FetchFuture) -> FContentFeedResult
                               {
        FContentFeedFetchResult FetchResult = FetchFuture.Get();
		if (!FetchResult.bSuccess)
		{
			UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Remote content fetch failed"));
			
			return FContentFeedResult::Error(FetchResult.ErrorMessage);
		}

		if (ContentType == EContentFeedType::Announcements)
		{
			if (CacheManager.IsValid())
			{
				if (!CacheManager->SaveToCache(FetchResult.AnnouncementFeed))
				{
					UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Failed to update announcement cache"));
				}
			}

			TArray<FConvaiAnnouncementItem> Items = FetchResult.AnnouncementFeed.GetSortedAnnouncements();
			

			return FContentFeedResult::Success(Items, false);
		}
		else
		{
			if (CacheManager.IsValid())
			{
				if (!CacheManager->SaveToCache(FetchResult.ChangelogFeed))
				{
					UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Failed to update changelog cache"));
				}
			}

			TArray<FConvaiChangelogItem> Items = FetchResult.ChangelogFeed.GetSortedChangelogs();
			

			return FContentFeedResult::SuccessChangelog(Items, false);
		} });
}
