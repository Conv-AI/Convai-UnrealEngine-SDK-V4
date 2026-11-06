/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IContentFeedProvider.h
 *
 * Interface for content feed providers.
 */

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Models/ConvaiAnnouncementData.h"

enum class EContentType : uint8
{
	Announcements,
	Changelogs
};

/**
 * Result of a content feed fetch operation.
 */
struct FContentFeedFetchResult
{
	/** Whether the operation succeeded */
	bool bSuccess;

	/** Fetched announcement feed data (valid only if bSuccess is true) */
	FConvaiAnnouncementFeed AnnouncementFeed;

	/** Fetched changelog feed data (valid only if bSuccess is true) */
	FConvaiChangelogFeed ChangelogFeed;

	/** Error message (valid only if bSuccess is false) */
	FString ErrorMessage;

	/** HTTP response code (0 if network error) */
	int32 ResponseCode;

	/** Success constructor for announcements */
	static FContentFeedFetchResult Success(const FConvaiAnnouncementFeed &InFeed)
	{
		FContentFeedFetchResult Result;
		Result.bSuccess = true;
		Result.AnnouncementFeed = InFeed;
		Result.ResponseCode = 200;
		return Result;
	}

	/** Success constructor for changelogs */
	static FContentFeedFetchResult SuccessChangelog(const FConvaiChangelogFeed &InFeed)
	{
		FContentFeedFetchResult Result;
		Result.bSuccess = true;
		Result.ChangelogFeed = InFeed;
		Result.ResponseCode = 200;
		return Result;
	}

	/** Error constructor */
	static FContentFeedFetchResult Error(const FString &ErrorMsg, int32 Code = 0)
	{
		FContentFeedFetchResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = ErrorMsg;
		Result.ResponseCode = Code;
		return Result;
	}

	/** Default constructor (public for TArray compatibility) */
	FContentFeedFetchResult()
		: bSuccess(false), ResponseCode(0)
	{
	}
};

/**
 * Interface for content feed providers.
 */
class IContentFeedProvider
{
public:
	virtual ~IContentFeedProvider() = default;

	/** Fetch content feed asynchronously */
	virtual TFuture<FContentFeedFetchResult> FetchContentAsync() = 0;

	/** Get the provider name for logging/debugging */
	virtual FString GetProviderName() const = 0;

	/** Check if provider is currently available */
	virtual bool IsAvailable() const = 0;
};
