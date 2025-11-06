/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IContentFeedService.h
 *
 * Interface for content feed service.
 */

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Models/ConvaiAnnouncementData.h"

enum class EContentFeedType : uint8
{
	Announcements,
	Changelogs
};

/** Result of GetContentFeed operation. */
struct FContentFeedResult
{
	/** Whether operation succeeded */
	bool bSuccess;

	/** Content type */
	EContentFeedType ContentType;

	/** Fetched announcement items (valid if ContentType == Announcements) */
	TArray<FConvaiAnnouncementItem> AnnouncementItems;

	/** Fetched changelog items (valid if ContentType == Changelogs) */
	TArray<FConvaiChangelogItem> ChangelogItems;

	/** Whether data came from cache */
	bool bFromCache;

	/** Error message if failed */
	FString ErrorMessage;

	static FContentFeedResult Success(const TArray<FConvaiAnnouncementItem> &InItems, bool bWasFromCache)
	{
		FContentFeedResult Result;
		Result.bSuccess = true;
		Result.ContentType = EContentFeedType::Announcements;
		Result.AnnouncementItems = InItems;
		Result.bFromCache = bWasFromCache;
		return Result;
	}

	static FContentFeedResult SuccessChangelog(const TArray<FConvaiChangelogItem> &InItems, bool bWasFromCache)
	{
		FContentFeedResult Result;
		Result.bSuccess = true;
		Result.ContentType = EContentFeedType::Changelogs;
		Result.ChangelogItems = InItems;
		Result.bFromCache = bWasFromCache;
		return Result;
	}

	static FContentFeedResult Error(const FString &ErrorMsg)
	{
		FContentFeedResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = ErrorMsg;
		Result.bFromCache = false;
		return Result;
	}

	// Helper to get item count regardless of type
	int32 GetItemCount() const
	{
		return ContentType == EContentFeedType::Announcements ? AnnouncementItems.Num() : ChangelogItems.Num();
	}

private:
	FContentFeedResult()
		: bSuccess(false), bFromCache(false), ContentType(EContentFeedType::Announcements)
	{
	}
};

/** Interface for content feed service. */
class IContentFeedService
{
public:
	virtual ~IContentFeedService() = default;

	/** Get content feed from cache or remote */
	virtual TFuture<FContentFeedResult> GetContentAsync(bool bForceRefresh = false) = 0;

	/** Force refresh content from remote */
	virtual TFuture<FContentFeedResult> RefreshContentAsync() = 0;

	/** Check if cached data is available */
	virtual bool HasCachedData() const = 0;

	/** Get cache age in seconds */
	virtual double GetCacheAge() const = 0;
};
