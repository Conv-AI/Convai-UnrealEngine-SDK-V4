/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * YouTubeService.h
 *
 * YouTube integration service using channel page parsing.
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
 * Fetches YouTube video information via channel page parsing.
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

    /** Prepares service for YouTube data fetch operations */
    virtual bool Initialize() override;

    /** Fetches latest video from specified channel */
    virtual void FetchLatestVideo(const FString &ChannelName, FOnYouTubeVideoFetched OnSuccess, FOnYouTubeVideoFetchFailed OnFailure) override;

    /** Returns cached video information if available */
    virtual TOptional<FYouTubeVideoInfo> GetCachedVideoInfo() const override;

    /** Returns true if currently fetching video data */
    virtual bool IsFetching() const override;

private:
    /** Fetches latest video by parsing channel videos page HTML. */
    void FetchLatestVideoFromChannelPage(
        const FString &ChannelName,
        FOnYouTubeVideoFetched OnSuccess,
        FOnYouTubeVideoFetchFailed OnFailure);

    /** Builds a channel videos URL from handle, URL, or default config. */
    FString BuildChannelVideosURL(const FString &ChannelName) const;

    /** Parses latest video metadata from YouTube channel videos HTML. */
    bool ParseLatestVideoFromChannelPage(const FString &HTMLContent, FYouTubeVideoInfo &OutVideoInfo) const;

    /** Parses latest video metadata from ytInitialData JSON payload inside channel HTML. */
    bool ParseLatestVideoFromInitialDataJSON(const FString &HTMLContent, FYouTubeVideoInfo &OutVideoInfo) const;

    /** Generates thumbnail URL from video ID */
    FString GenerateThumbnailURL(const FString &VideoID) const;

    /** Finds first regex capture group in input text. */
    bool ExtractRegexGroup(const FString &Content, const FString &PatternText, FString &OutValue) const;

    /** Decodes escaped JSON string fragments from HTML payloads. */
    FString DecodeEscapedJSONString(const FString &Input) const;

    /** Extracts a balanced JSON object block starting from token occurrence. */
    bool ExtractBalancedJSONObjectAfterToken(const FString &Content, const FString &StartToken, FString &OutJSON) const;

    /** Finds first videoRenderer object in parsed JSON tree. */
    bool FindFirstVideoRendererJSON(const TSharedPtr<FJsonValue> &Value, TSharedPtr<FJsonObject> &OutRenderer) const;

    /** Reads text from YouTube text object (simpleText or runs[]). */
    FString ExtractYouTubeTextObject(const TSharedPtr<FJsonObject> &TextObject) const;

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

    /** Circuit breaker for YouTube fetch operations */
    TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;

    /** Retry policy for transient failures */
    TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;

    /** Active HTTP operation for cancellation */
    TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> ActiveOperation;
};
