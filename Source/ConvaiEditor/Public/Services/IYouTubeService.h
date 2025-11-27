/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IYouTubeService.h
 *
 * Interface for YouTube integration via RSS feeds.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Services/YouTubeTypes.h"

/**
 * Delegate for successful video retrieval.
 */
DECLARE_DELEGATE_OneParam(FOnYouTubeVideoFetched, const FYouTubeVideoInfo &);

/**
 * Delegate for failed video retrieval.
 */
DECLARE_DELEGATE_OneParam(FOnYouTubeVideoFetchFailed, const FString &);

/**
 * Interface for YouTube video fetching via RSS feeds.
 */
class CONVAIEDITOR_API IYouTubeService : public IConvaiService
{
public:
    virtual ~IYouTubeService() = default;

    /** Initializes the service */
    virtual bool Initialize() = 0;

    /** Shuts down and releases resources */
    virtual void Shutdown() = 0;

    /** Fetches latest video from specified channel */
    virtual void FetchLatestVideo(const FString &ChannelName, FOnYouTubeVideoFetched OnSuccess, FOnYouTubeVideoFetchFailed OnFailure) = 0;

    /** Returns cached video information if available */
    virtual TOptional<FYouTubeVideoInfo> GetCachedVideoInfo() const = 0;

    /** Returns true if currently fetching */
    virtual bool IsFetching() const = 0;

    static FName StaticType() { return TEXT("IYouTubeService"); }
};
