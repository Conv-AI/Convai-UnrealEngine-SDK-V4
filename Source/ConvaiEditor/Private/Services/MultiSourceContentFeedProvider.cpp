/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * MultiSourceContentFeedProvider.cpp
 *
 * Implementation of multi-source content feed provider.
 */

#include "Services/MultiSourceContentFeedProvider.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Async/Async.h"

FMultiSourceContentFeedProvider::FMultiSourceContentFeedProvider(const FMultiSourceConfig &InConfig)
    : Config(InConfig)
{
    for (int32 i = 0; i < Config.SourceURLs.Num(); ++i)
    {
        const FString &URL = Config.SourceURLs[i];

        FRemoteContentFeedProvider::FConfig SourceConfig = Config.BaseConfig;
        SourceConfig.URL = URL;
        SourceConfig.ContentType = Config.ContentType;

        auto Provider = MakeUnique<FRemoteContentFeedProvider>(SourceConfig);
        SourceProviders.Add(MoveTemp(Provider));
    }
}

bool FMultiSourceContentFeedProvider::IsAvailable() const
{
    if (!IsConfigValid())
    {
        return false;
    }

    for (const auto &Provider : SourceProviders)
    {
        if (Provider.IsValid() && Provider->IsAvailable())
        {
            return true;
        }
    }

    return false;
}

FString FMultiSourceContentFeedProvider::GetProviderName() const
{
    return FString::Printf(TEXT("MultiSourceProvider(%d sources)"), Config.SourceURLs.Num());
}

bool FMultiSourceContentFeedProvider::IsConfigValid() const
{
    if (Config.SourceURLs.Num() == 0)
    {
        UE_LOG(LogConvaiEditorConfig, Error,
               TEXT("MultiSourceProvider configuration error: no source URLs specified"));
        return false;
    }

    if (SourceProviders.Num() != Config.SourceURLs.Num())
    {
        UE_LOG(LogConvaiEditorConfig, Error,
               TEXT("MultiSourceProvider configuration error: provider count mismatch"));
        return false;
    }

    return true;
}

TFuture<FContentFeedFetchResult> FMultiSourceContentFeedProvider::FetchContentAsync()
{
    if (!IsConfigValid())
    {
        UE_LOG(LogConvaiEditorConfig, Error,
               TEXT("MultiSourceProvider fetch failed: invalid configuration"));
        return Async(EAsyncExecution::TaskGraphMainThread, []()
                     { return FContentFeedFetchResult::Error(TEXT("Invalid multi-source configuration"), 0); });
    }

    TSharedPtr<TPromise<FContentFeedFetchResult>> FinalPromise =
        MakeShared<TPromise<FContentFeedFetchResult>>();

    TSharedPtr<TArray<FContentFeedFetchResult>> CollectedResults =
        MakeShared<TArray<FContentFeedFetchResult>>();
    CollectedResults->SetNum(SourceProviders.Num());

    TSharedPtr<FThreadSafeCounter> CompletedCounter = MakeShared<FThreadSafeCounter>();

    for (int32 i = 0; i < SourceProviders.Num(); ++i)
    {
        if (!SourceProviders[i].IsValid())
        {
            UE_LOG(LogConvaiEditorConfig, Warning,
                   TEXT("MultiSourceProvider: source %d provider is null, skipping"), i);
            (*CollectedResults)[i] = FContentFeedFetchResult::Error(
                TEXT("Provider is null"), 0);
            CompletedCounter->Increment();
            continue;
        }

        TFuture<FContentFeedFetchResult> SourceFuture =
            SourceProviders[i]->FetchContentAsync();

        SourceFuture.Then([this, i, CollectedResults, CompletedCounter, FinalPromise](TFuture<FContentFeedFetchResult> Result)
                          {
			FContentFeedFetchResult SourceResult = Result.Get();

			(*CollectedResults)[i] = SourceResult;

			if (!SourceResult.bSuccess)
			{
				UE_LOG(LogConvaiEditorConfig, Warning, 
					TEXT("MultiSourceProvider: source %d failed - %s"), 
					i, *SourceResult.ErrorMessage);
			}

			int32 CompletedCount = CompletedCounter->Increment();

			if (CompletedCount >= SourceProviders.Num())
			{
				AsyncTask(ENamedThreads::GameThread, [this, CollectedResults, FinalPromise]()
				{
					FContentFeedFetchResult MergedResult = MergeResults(*CollectedResults);
					FinalPromise->SetValue(MergedResult);
				});
			} });
    }

    return FinalPromise->GetFuture();
}

FContentFeedFetchResult FMultiSourceContentFeedProvider::MergeResults(
    const TArray<FContentFeedFetchResult> &Results)
{
    TArray<FConvaiAnnouncementItem> AllAnnouncements;
    TArray<FConvaiChangelogItem> AllChangelogs;
    TArray<FString> Errors;
    int32 SuccessCount = 0;

    for (int32 i = 0; i < Results.Num(); ++i)
    {
        const FContentFeedFetchResult &Result = Results[i];

        if (Result.bSuccess)
        {
            SuccessCount++;

            if (Config.ContentType == EContentType::Announcements)
            {
                AllAnnouncements.Append(Result.AnnouncementFeed.Announcements);
            }
            else
            {
                AllChangelogs.Append(Result.ChangelogFeed.Changelogs);
            }
        }
        else
        {
            Errors.Add(FString::Printf(TEXT("Source %d: %s"), i, *Result.ErrorMessage));
        }
    }

    if (SuccessCount == 0)
    {
        FString CombinedError = FString::Printf(
            TEXT("All %d sources failed: %s"),
            Results.Num(),
            *FString::Join(Errors, TEXT("; ")));

        UE_LOG(LogConvaiEditorConfig, Error,
               TEXT("MultiSourceProvider: all sources failed - %s"), *CombinedError);

        return FContentFeedFetchResult::Error(CombinedError, 0);
    }

    if (Config.bRequireAllSources && SuccessCount < Results.Num())
    {
        FString ErrorMsg = FString::Printf(
            TEXT("Required all sources but only %d/%d succeeded"),
            SuccessCount, Results.Num());

        UE_LOG(LogConvaiEditorConfig, Error,
               TEXT("MultiSourceProvider: required sources failed - %s"), *ErrorMsg);

        return FContentFeedFetchResult::Error(ErrorMsg, 0);
    }

    if (Config.ContentType == EContentType::Announcements)
    {
        if (Config.bDeduplicateByID)
        {
            int32 BeforeCount = AllAnnouncements.Num();
            AllAnnouncements = DeduplicateByID(AllAnnouncements);
            int32 AfterCount = AllAnnouncements.Num();
        }

        AllAnnouncements.Sort();

        if (Errors.Num() > 0)
        {
            UE_LOG(LogConvaiEditorConfig, Warning,
                   TEXT("MultiSourceProvider: partial success - %d/%d sources succeeded, %d announcements. Errors: %s"),
                   SuccessCount, Results.Num(), AllAnnouncements.Num(),
                   *FString::Join(Errors, TEXT("; ")));
        }

        FConvaiAnnouncementFeed Feed;
        Feed.Version = TEXT("1.0");
        Feed.LastUpdated = FDateTime::UtcNow();
        Feed.Announcements = AllAnnouncements;

        return FContentFeedFetchResult::Success(Feed);
    }
    else
    {
        if (Config.bDeduplicateByID)
        {
            int32 BeforeCount = AllChangelogs.Num();
            AllChangelogs = DeduplicateChangelogsByID(AllChangelogs);
            int32 AfterCount = AllChangelogs.Num();
        }

        AllChangelogs.Sort();

        if (Errors.Num() > 0)
        {
            UE_LOG(LogConvaiEditorConfig, Warning,
                   TEXT("MultiSourceProvider: partial success - %d/%d sources succeeded, %d changelogs. Errors: %s"),
                   SuccessCount, Results.Num(), AllChangelogs.Num(),
                   *FString::Join(Errors, TEXT("; ")));
        }

        FConvaiChangelogFeed Feed;
        Feed.Version = TEXT("1.0");
        Feed.LastUpdated = FDateTime::UtcNow();
        Feed.Changelogs = AllChangelogs;

        return FContentFeedFetchResult::SuccessChangelog(Feed);
    }
}

TArray<FConvaiAnnouncementItem> FMultiSourceContentFeedProvider::DeduplicateByID(
    const TArray<FConvaiAnnouncementItem> &Announcements)
{
    TMap<FString, FConvaiAnnouncementItem> UniqueMap;

    for (const FConvaiAnnouncementItem &Item : Announcements)
    {
        if (Item.ID.IsEmpty())
        {
            continue;
        }

        FConvaiAnnouncementItem *Existing = UniqueMap.Find(Item.ID);

        if (Existing == nullptr)
        {
            UniqueMap.Add(Item.ID, Item);
        }
        else
        {
            if (Item.Priority < Existing->Priority)
            {
                *Existing = Item;
            }
            else
            {
            }
        }
    }

    TArray<FConvaiAnnouncementItem> Result;
    UniqueMap.GenerateValueArray(Result);

    return Result;
}

TArray<FConvaiChangelogItem> FMultiSourceContentFeedProvider::DeduplicateChangelogsByID(
    const TArray<FConvaiChangelogItem> &Changelogs)
{
    TMap<FString, FConvaiChangelogItem> UniqueMap;

    for (const FConvaiChangelogItem &Item : Changelogs)
    {
        if (Item.ID.IsEmpty())
        {
            continue;
        }

        FConvaiChangelogItem *Existing = UniqueMap.Find(Item.ID);

        if (Existing == nullptr)
        {
            UniqueMap.Add(Item.ID, Item);
        }
        else
        {
            if (Item.Date > Existing->Date)
            {
                *Existing = Item;
            }
            else
            {
            }
        }
    }

    TArray<FConvaiChangelogItem> Result;
    UniqueMap.GenerateValueArray(Result);

    return Result;
}
