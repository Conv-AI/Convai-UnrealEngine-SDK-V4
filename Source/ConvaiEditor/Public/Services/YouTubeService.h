/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * YouTubeService.h
 *
 * YouTube integration service using RSS feeds.
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Http.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Services/IYouTubeService.h"
#include "Services/YouTubeTypes.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"

namespace ConvaiEditor
{
    template<typename T>
    class FAsyncOperation;
    struct FHttpAsyncResponse;
}

/**
 * Fetches YouTube video information via RSS feeds.
 */
class CONVAIEDITOR_API FYouTubeService : public IYouTubeService, public TSharedFromThis<FYouTubeService>
{
public:
    FYouTubeService();
    virtual ~FYouTubeService() = default;

    /** Initializes the service */
    virtual void Startup() override;

    /** Cleans up resources */
    virtual void Shutdown() override;

    /** Prepares service for RSS feed communication */
    virtual bool Initialize() override;

    /** Fetches latest video from specified channel */
    virtual void FetchLatestVideo(const FString &ChannelName, FOnYouTubeVideoFetched OnSuccess, FOnYouTubeVideoFetchFailed OnFailure) override;

    /** Returns cached video information if available */
    virtual TOptional<FYouTubeVideoInfo> GetCachedVideoInfo() const override;

    /** Returns true if currently fetching video data */
    virtual bool IsFetching() const override;

private:
    /** Parses RSS feed XML to extract video information */
    bool ParseRSSFeed(const FString &XMLContent, FYouTubeVideoInfo &OutVideoInfo);

    /** Extracts video ID from YouTube URL */
    FString ExtractVideoIDFromURL(const FString &URL) const;

    /** Generates thumbnail URL from video ID */
    FString GenerateThumbnailURL(const FString &VideoID) const;

    /** Parses XML element by name */
    bool ParseXMLElement(const FString &XMLContent, const FString &ElementName, FString &OutValue) const;

    /** Extracts content from CDATA sections */
    FString ExtractCDATA(const FString &Content) const;

    /** Cached video information */
    TOptional<FYouTubeVideoInfo> CachedVideoInfo;

    /** Whether fetch operation is in progress */
    bool bIsFetching;

    /** Whether service is shutting down */
    bool bIsShuttingDown;

    /** Cache expiration time in minutes */
    static constexpr float CacheExpirationMinutes = 30.0f;

    /** Last successful fetch timestamp */
    FDateTime LastFetchTime;

    bool bIsInitialized;

    /** Circuit breaker for YouTube RSS feed */
    TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;

    /** Retry policy for transient failures */
    TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;

    /** Active HTTP operation for cancellation */
    TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> ActiveOperation;
};
