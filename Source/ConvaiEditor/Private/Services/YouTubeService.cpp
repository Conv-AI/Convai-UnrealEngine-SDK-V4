/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * YouTubeService.cpp
 *
 * Implementation of YouTube service.
 */

#include "Services/YouTubeService.h"
#include "ConvaiEditor.h"
#include "Async/HttpAsyncOperation.h"
#include "Async/Async.h"
#include "Utility/ConvaiURLs.h"
#include "Internationalization/Regex.h"
#include "Dom/JsonValue.h"

static const FString CONVAI_YOUTUBE_BASE_URL = TEXT("https://www.youtube.com");
static const FString CONVAI_DEFAULT_CHANNEL_HANDLE = TEXT("convai");
static const FString CONVAI_DEFAULT_CHANNEL_ID = TEXT("UCcYtXgiavJYMKSirsk6VNsw");

FYouTubeService::FYouTubeService()
    : bIsFetching(false), bIsShuttingDown(false), LastFetchTime(FDateTime::MinValue()), bIsInitialized(false)
{
}

void FYouTubeService::Startup()
{
    bIsInitialized = false;

    ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
    CircuitConfig.Name = TEXT("YouTubeFeed");
    CircuitConfig.FailureThreshold = 3;
    CircuitConfig.SuccessThreshold = 2;
    CircuitConfig.OpenTimeoutSeconds = 60.0f;
    CircuitConfig.bEnableLogging = false;
    CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

    ConvaiEditor::FRetryPolicyConfig RetryConfig;
    RetryConfig.Name = TEXT("YouTubeFeed");
    RetryConfig.MaxAttempts = 2;
    RetryConfig.BaseDelaySeconds = 2.0f;
    RetryConfig.MaxDelaySeconds = 10.0f;
    RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Fixed;
    RetryConfig.bEnableJitter = false;
    RetryConfig.bEnableLogging = false;
    RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
    RetryPolicy = MakeShared<ConvaiEditor::FRetryPolicy>(RetryConfig);
}

void FYouTubeService::Shutdown()
{
    UE_LOG(LogConvaiEditor, Log, TEXT("YouTubeService: Shutting down..."));

    // Set shutdown flag to prevent new fetches
    bIsShuttingDown = true;

    // Cancel active HTTP operation
    if (ActiveOperation.IsValid())
    {
        ActiveOperation->Cancel();
        ActiveOperation.Reset();
    }

    // Brief wait for cancellation
    FPlatformProcess::Sleep(0.05f);

    bIsFetching = false;
    bIsInitialized = false;
    CachedVideoInfo.Reset();

    CircuitBreaker.Reset();
    RetryPolicy.Reset();

    UE_LOG(LogConvaiEditor, Log, TEXT("YouTubeService: Shutdown complete"));
}

bool FYouTubeService::Initialize()
{
    if (bIsInitialized)
    {
        return true;
    }

    bIsInitialized = true;
    return true;
}

/** Fetches the latest video from the specified YouTube channel */
void FYouTubeService::FetchLatestVideo(const FString &ChannelName, FOnYouTubeVideoFetched OnSuccess, FOnYouTubeVideoFetchFailed OnFailure)
{
    // Check if shutting down
    if (bIsShuttingDown)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: fetch blocked - service shutting down"));
        OnFailure.ExecuteIfBound(TEXT("Service shutting down"));
        return;
    }

    const FDateTime Now = FDateTime::Now();
    const double MinutesSinceLastFetch = (Now - LastFetchTime).GetTotalMinutes();

    if (CachedVideoInfo.IsSet() && MinutesSinceLastFetch < CacheExpirationMinutes)
    {
        OnSuccess.ExecuteIfBound(CachedVideoInfo.GetValue());
        return;
    }

    if (CircuitBreaker && CircuitBreaker->IsOpen())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: service temporarily unavailable - circuit breaker open"));
        OnFailure.ExecuteIfBound(TEXT("YouTube service circuit breaker is open - service temporarily unavailable"));
        return;
    }

    if (bIsFetching)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: video fetch already in progress - request ignored"));
        OnFailure.ExecuteIfBound(TEXT("Fetch already in progress"));
        return;
    }

    bIsFetching = true;
    FetchLatestVideoFromRSSFeed(ChannelName, OnSuccess, OnFailure);
}

TOptional<FYouTubeVideoInfo> FYouTubeService::GetCachedVideoInfo() const
{
    return CachedVideoInfo;
}

bool FYouTubeService::IsFetching() const
{
    return bIsFetching;
}

void FYouTubeService::FetchLatestVideoFromRSSFeed(
    const FString &ChannelName,
    FOnYouTubeVideoFetched OnSuccess,
    FOnYouTubeVideoFetchFailed OnFailure)
{
    const FString ChannelFeedURL = BuildChannelFeedURL(ChannelName);

    ConvaiEditor::FHttpAsyncRequest HttpRequest(ChannelFeedURL);
    HttpRequest.WithVerb(TEXT("GET"))
        .WithHeader(TEXT("User-Agent"), TEXT("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36"))
        .WithHeader(TEXT("Accept"), TEXT("application/atom+xml,application/xml,text/xml;q=0.9,*/*;q=0.8"))
        .WithTimeout(30.0f);

    if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
    {
        ActiveOperation = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(HttpRequest, CircuitBreaker, RetryPolicy, nullptr);
    }
    else
    {
        ActiveOperation = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, nullptr);
    }

    ActiveOperation->OnComplete([this, ChannelName, OnSuccess, OnFailure](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                                {
        ActiveOperation.Reset();

        if (bIsShuttingDown)
        {
            bIsFetching = false;
            UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: ignoring RSS feed response - service shutting down"));
            return;
        }

        auto FallbackToChannelPage = [this, ChannelName, OnSuccess, OnFailure](const FString &Reason)
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: RSS feed failed (%s), falling back to channel page"), *Reason);
            FetchLatestVideoFromChannelPage(ChannelName, OnSuccess, OnFailure);
        };

        if (!Result.IsSuccess())
        {
            FallbackToChannelPage(Result.GetError());
            return;
        }

        const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
        if (!HttpResponse.IsSuccess())
        {
            FallbackToChannelPage(FString::Printf(TEXT("HTTP error %d"), HttpResponse.ResponseCode));
            return;
        }

        if (HttpResponse.Body.IsEmpty())
        {
            FallbackToChannelPage(TEXT("Empty response"));
            return;
        }

        FYouTubeVideoInfo VideoInfo;
        if (ParseLatestVideoFromRSSFeed(HttpResponse.Body, VideoInfo) && VideoInfo.IsValid())
        {
            CachedVideoInfo = VideoInfo;
            LastFetchTime = FDateTime::Now();
            bIsFetching = false;

            AsyncTask(ENamedThreads::GameThread, [OnSuccess, VideoInfo]()
            {
                OnSuccess.ExecuteIfBound(VideoInfo);
            });
            return;
        }

        FallbackToChannelPage(TEXT("Failed to parse RSS feed"));
    });

    ActiveOperation->Start();
}

void FYouTubeService::FetchLatestVideoFromChannelPage(
    const FString &ChannelName,
    FOnYouTubeVideoFetched OnSuccess,
    FOnYouTubeVideoFetchFailed OnFailure)
{
    const FString ChannelVideosURL = BuildChannelVideosURL(ChannelName);

    ConvaiEditor::FHttpAsyncRequest HttpRequest(ChannelVideosURL);
    HttpRequest.WithVerb(TEXT("GET"))
        .WithHeader(TEXT("User-Agent"), TEXT("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36"))
        .WithHeader(TEXT("Accept"), TEXT("text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"))
        .WithTimeout(30.0f);

    if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
    {
        ActiveOperation = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(HttpRequest, CircuitBreaker, RetryPolicy, nullptr);
    }
    else
    {
        ActiveOperation = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, nullptr);
    }

    ActiveOperation->OnComplete([this, OnSuccess, OnFailure](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                                {
        ActiveOperation.Reset();

        if (bIsShuttingDown)
        {
            bIsFetching = false;
            UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: ignoring channel page response - service shutting down"));
            return;
        }

        auto FailRequest = [this, OnFailure](const FString &ErrorMessage)
        {
            bIsFetching = false;
            AsyncTask(ENamedThreads::GameThread, [OnFailure, ErrorMessage]()
            {
                OnFailure.ExecuteIfBound(ErrorMessage);
            });
        };

        if (!Result.IsSuccess())
        {
            const FString ErrorMessage = Result.GetError();
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService channel page request failed: %s"), *ErrorMessage);
            FailRequest(ErrorMessage);
            return;
        }

        const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
        if (!HttpResponse.IsSuccess())
        {
            const FString ErrorMessage = FString::Printf(TEXT("HTTP error %d"), HttpResponse.ResponseCode);
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService channel page request failed: %s"), *ErrorMessage);
            FailRequest(ErrorMessage);
            return;
        }

        if (HttpResponse.Body.IsEmpty())
        {
            FailRequest(TEXT("Empty response"));
            return;
        }

        FYouTubeVideoInfo VideoInfo;
        if (ParseLatestVideoFromChannelPage(HttpResponse.Body, VideoInfo) && VideoInfo.IsValid())
        {
            CachedVideoInfo = VideoInfo;
            LastFetchTime = FDateTime::Now();
            bIsFetching = false;

            AsyncTask(ENamedThreads::GameThread, [OnSuccess, VideoInfo]()
            {
                OnSuccess.ExecuteIfBound(VideoInfo);
            });
            return;
        }

        UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: failed to parse channel page"));
        FailRequest(TEXT("Failed to parse YouTube channel page"));
    });

    ActiveOperation->Start();
}

FString FYouTubeService::BuildChannelFeedURL(const FString &ChannelName) const
{
    FString ChannelId = ChannelName.TrimStartAndEnd();

    if (ChannelId.IsEmpty() ||
        ChannelId.Equals(CONVAI_DEFAULT_CHANNEL_HANDLE, ESearchCase::IgnoreCase) ||
        ChannelId.Equals(TEXT("@convai"), ESearchCase::IgnoreCase) ||
        ChannelId.Contains(TEXT("youtube.com/@convai")))
    {
        ChannelId = CONVAI_DEFAULT_CHANNEL_ID;
    }
    else
    {
        FString ExtractedChannelId;
        if (ExtractRegexGroup(ChannelId, TEXT("channel_id=([A-Za-z0-9_-]+)"), ExtractedChannelId) ||
            ExtractRegexGroup(ChannelId, TEXT("/channel/([A-Za-z0-9_-]+)"), ExtractedChannelId))
        {
            ChannelId = ExtractedChannelId;
        }
    }

    if (!ChannelId.StartsWith(TEXT("UC")))
    {
        ChannelId = CONVAI_DEFAULT_CHANNEL_ID;
    }

    return FString::Printf(TEXT("https://www.youtube.com/feeds/videos.xml?channel_id=%s"), *ChannelId);
}

FString FYouTubeService::BuildChannelVideosURL(const FString &ChannelName) const
{
    FString BaseURL = ChannelName.TrimStartAndEnd();

    if (BaseURL.IsEmpty())
    {
        BaseURL = FConvaiURLs::GetYouTubeURL();
    }

    if (BaseURL.IsEmpty())
    {
        BaseURL = FString::Printf(TEXT("%s/@%s"), *CONVAI_YOUTUBE_BASE_URL, *CONVAI_DEFAULT_CHANNEL_HANDLE);
    }

    if (BaseURL.StartsWith(TEXT("@")))
    {
        BaseURL = FString::Printf(TEXT("%s/%s"), *CONVAI_YOUTUBE_BASE_URL, *BaseURL);
    }
    else if (!BaseURL.StartsWith(TEXT("http://")) && !BaseURL.StartsWith(TEXT("https://")))
    {
        BaseURL = FString::Printf(TEXT("%s/@%s"), *CONVAI_YOUTUBE_BASE_URL, *BaseURL);
    }

    while (BaseURL.EndsWith(TEXT("/")))
    {
        BaseURL.RemoveFromEnd(TEXT("/"));
    }

    if (!BaseURL.Contains(TEXT("/videos")))
    {
        BaseURL += TEXT("/videos");
    }

    if (!BaseURL.Contains(TEXT("?")))
    {
        BaseURL += TEXT("?view=0&sort=dd&shelf_id=0");
    }

    return BaseURL;
}

bool FYouTubeService::ParseLatestVideoFromRSSFeed(const FString &XMLContent, FYouTubeVideoInfo &OutVideoInfo) const
{
    const FRegexPattern EntryPattern(TEXT("<entry>([\\s\\S]*?)</entry>"));
    FRegexMatcher EntryMatcher(EntryPattern, XMLContent);

    bool bFoundEntry = false;
    while (EntryMatcher.FindNext())
    {
        bFoundEntry = true;

        FYouTubeVideoInfo CandidateVideoInfo;
        if (!ParseVideoInfoFromRSSEntry(EntryMatcher.GetCaptureGroup(1), CandidateVideoInfo))
        {
            continue;
        }

        if (IsShortsURL(CandidateVideoInfo.VideoURL))
        {
            UE_LOG(LogConvaiEditor, Log, TEXT("YouTubeService: skipping YouTube Shorts entry: %s"), *CandidateVideoInfo.VideoURL);
            continue;
        }

        OutVideoInfo = CandidateVideoInfo;
        return true;
    }

    UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: %s in RSS feed"), bFoundEntry ? TEXT("no non-Shorts video found") : TEXT("no entry found"));
    return false;
}

bool FYouTubeService::ParseVideoInfoFromRSSEntry(const FString &Entry, FYouTubeVideoInfo &OutVideoInfo) const
{
    FString VideoID;
    if (!ExtractRegexGroup(Entry, TEXT("<yt:videoId>([^<]+)</yt:videoId>"), VideoID))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: no video ID found in RSS feed"));
        return false;
    }

    FString Title;
    ExtractRegexGroup(Entry, TEXT("<title>([\\s\\S]*?)</title>"), Title);

    FString VideoURL;
    ExtractRegexGroup(Entry, TEXT("<link[^>]+href=\"([^\"]+)\""), VideoURL);

    FString Description;
    ExtractRegexGroup(Entry, TEXT("<media:description>([\\s\\S]*?)</media:description>"), Description);

    FString Author;
    ExtractRegexGroup(Entry, TEXT("<author>[\\s\\S]*?<name>([\\s\\S]*?)</name>[\\s\\S]*?</author>"), Author);

    FString Published;
    ExtractRegexGroup(Entry, TEXT("<published>([^<]+)</published>"), Published);

    OutVideoInfo.VideoID = VideoID.TrimStartAndEnd();
    OutVideoInfo.Title = DecodeHTMLEntities(Title).TrimStartAndEnd();
    OutVideoInfo.Description = DecodeHTMLEntities(Description).TrimStartAndEnd();
    OutVideoInfo.Author = DecodeHTMLEntities(Author).TrimStartAndEnd();
    OutVideoInfo.VideoURL = DecodeHTMLEntities(VideoURL).TrimStartAndEnd();
    OutVideoInfo.ThumbnailURL = GenerateThumbnailURL(OutVideoInfo.VideoID);

    if (OutVideoInfo.VideoURL.IsEmpty())
    {
        OutVideoInfo.VideoURL = FString::Printf(TEXT("https://www.youtube.com/watch?v=%s"), *OutVideoInfo.VideoID);
    }

    FDateTime ParsedPublicationDate;
    if (!Published.IsEmpty() && FDateTime::ParseIso8601(*Published, ParsedPublicationDate))
    {
        OutVideoInfo.PublicationDate = ParsedPublicationDate;
    }
    else
    {
        OutVideoInfo.PublicationDate = FDateTime::Now();
    }

    return OutVideoInfo.IsValid();
}

bool FYouTubeService::ParseLatestVideoFromChannelPage(const FString &HTMLContent, FYouTubeVideoInfo &OutVideoInfo) const
{
    if (ParseLatestVideoFromInitialDataJSON(HTMLContent, OutVideoInfo) && OutVideoInfo.IsValid())
    {
        return true;
    }

    FString VideoID;
    const bool bFoundVideoId =
        ExtractRegexGroup(HTMLContent, TEXT("videoRenderer\":\\{\"videoId\":\"([A-Za-z0-9_-]{11})\""), VideoID) ||
        ExtractRegexGroup(HTMLContent, TEXT("videoRenderer\\\\\":\\\\{\\\\\"videoId\\\\\":\\\\\"([A-Za-z0-9_-]{11})\\\\\""), VideoID);

    if (!bFoundVideoId || VideoID.IsEmpty())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: no video ID found in channel page"));
        return false;
    }

    int32 RendererStart = HTMLContent.Find(FString::Printf(TEXT("videoRenderer\":{\"videoId\":\"%s\""), *VideoID), ESearchCase::CaseSensitive);
    if (RendererStart == INDEX_NONE)
    {
        RendererStart = HTMLContent.Find(FString::Printf(TEXT("videoRenderer\\\":\\{\\\"videoId\\\":\\\"%s\\\""), *VideoID), ESearchCase::CaseSensitive);
    }

    const FString SearchContent = (RendererStart != INDEX_NONE)
        ? HTMLContent.Mid(RendererStart, FMath::Min(18000, HTMLContent.Len() - RendererStart))
        : HTMLContent;

    FString RawTitle;
    const bool bFoundTitle =
        ExtractRegexGroup(SearchContent, TEXT("\"title\":\\{\"runs\":\\[\\{\"text\":\"([^\"]+)\""), RawTitle) ||
        ExtractRegexGroup(SearchContent, TEXT("\\\\\"title\\\\\":\\\\{\\\\\"runs\\\\\":\\\\[\\\\{\\\\\"text\\\\\":\\\\\"([^\\\\\"]+)"), RawTitle);

    if (!bFoundTitle || RawTitle.IsEmpty())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: no title found for latest video"));
        return false;
    }

    FString RawDescription;
    ExtractRegexGroup(SearchContent, TEXT("\"descriptionSnippet\":\\{\"runs\":\\[\\{\"text\":\"([^\"]*)\""), RawDescription) ||
        ExtractRegexGroup(SearchContent, TEXT("\\\\\"descriptionSnippet\\\\\":\\\\{\\\\\"runs\\\\\":\\\\[\\\\{\\\\\"text\\\\\":\\\\\"([^\\\\\"]*)"), RawDescription);

    FString RawAuthor;
    ExtractRegexGroup(SearchContent, TEXT("\"ownerText\":\\{\"runs\":\\[\\{\"text\":\"([^\"]+)\""), RawAuthor) ||
        ExtractRegexGroup(SearchContent, TEXT("\\\\\"ownerText\\\\\":\\\\{\\\\\"runs\\\\\":\\\\[\\\\{\\\\\"text\\\\\":\\\\\"([^\\\\\"]+)"), RawAuthor);

    OutVideoInfo.VideoID = VideoID.TrimStartAndEnd();
    OutVideoInfo.Title = DecodeEscapedJSONString(RawTitle).TrimStartAndEnd();
    OutVideoInfo.Description = DecodeEscapedJSONString(RawDescription).TrimStartAndEnd();
    OutVideoInfo.Author = DecodeEscapedJSONString(RawAuthor).TrimStartAndEnd();
    OutVideoInfo.VideoURL = FString::Printf(TEXT("https://www.youtube.com/watch?v=%s"), *OutVideoInfo.VideoID);
    OutVideoInfo.ThumbnailURL = GenerateThumbnailURL(OutVideoInfo.VideoID);
    OutVideoInfo.PublicationDate = FDateTime::Now();

    return OutVideoInfo.IsValid();
}

bool FYouTubeService::ParseLatestVideoFromInitialDataJSON(const FString &HTMLContent, FYouTubeVideoInfo &OutVideoInfo) const
{
    FString InitialDataJSON;
    if (!ExtractBalancedJSONObjectAfterToken(HTMLContent, TEXT("var ytInitialData = "), InitialDataJSON) &&
        !ExtractBalancedJSONObjectAfterToken(HTMLContent, TEXT("window[\"ytInitialData\"] = "), InitialDataJSON))
    {
        return false;
    }

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InitialDataJSON);
    TSharedPtr<FJsonObject> RootObject;
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: failed to deserialize ytInitialData JSON"));
        return false;
    }

    TSharedPtr<FJsonObject> VideoRenderer;
    if (!FindFirstVideoRendererJSON(MakeShared<FJsonValueObject>(RootObject), VideoRenderer) || !VideoRenderer.IsValid())
    {
        TSharedPtr<FJsonObject> LockupViewModel;
        if (!FindFirstLockupViewModelJSON(MakeShared<FJsonValueObject>(RootObject), LockupViewModel) || !LockupViewModel.IsValid())
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: no videoRenderer or lockupViewModel found in ytInitialData JSON"));
            return false;
        }

        FString ThumbnailURL;
        FindFirstStringMatchingRegexJSON(MakeShared<FJsonValueObject>(LockupViewModel), TEXT("(https://i\\.ytimg\\.com/vi/[A-Za-z0-9_-]{11}/[^\\\"]+)"), ThumbnailURL);
        ThumbnailURL = DecodeEscapedJSONString(ThumbnailURL).TrimStartAndEnd();

        FString VideoID;
        if (!ExtractRegexGroup(ThumbnailURL, TEXT("/vi/([A-Za-z0-9_-]{11})/"), VideoID))
        {
            FindFirstStringMatchingRegexJSON(MakeShared<FJsonValueObject>(LockupViewModel), TEXT("watch\\?v=([A-Za-z0-9_-]{11})"), VideoID);
        }

        FString Title;
        if (LockupViewModel->HasTypedField<EJson::Object>(TEXT("metadata")))
        {
            const TSharedPtr<FJsonObject> Metadata = LockupViewModel->GetObjectField(TEXT("metadata"));
            if (Metadata.IsValid() && Metadata->HasTypedField<EJson::Object>(TEXT("lockupMetadataViewModel")))
            {
                const TSharedPtr<FJsonObject> LockupMetadata = Metadata->GetObjectField(TEXT("lockupMetadataViewModel"));
                if (LockupMetadata.IsValid() && LockupMetadata->HasTypedField<EJson::Object>(TEXT("title")))
                {
                    LockupMetadata->GetObjectField(TEXT("title"))->TryGetStringField(TEXT("content"), Title);
                }
            }
        }

        if (VideoID.IsEmpty() || Title.IsEmpty())
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: lockupViewModel missing video ID or title"));
            return false;
        }

        OutVideoInfo.VideoID = VideoID.TrimStartAndEnd();
        OutVideoInfo.Title = DecodeHTMLEntities(DecodeEscapedJSONString(Title)).TrimStartAndEnd();
        OutVideoInfo.VideoURL = FString::Printf(TEXT("https://www.youtube.com/watch?v=%s"), *OutVideoInfo.VideoID);
        OutVideoInfo.ThumbnailURL = !ThumbnailURL.IsEmpty() ? ThumbnailURL : GenerateThumbnailURL(OutVideoInfo.VideoID);
        OutVideoInfo.PublicationDate = FDateTime::Now();
        if (IsShortsURL(OutVideoInfo.VideoURL))
        {
            UE_LOG(LogConvaiEditor, Log, TEXT("YouTubeService: skipping YouTube Shorts entry: %s"), *OutVideoInfo.VideoURL);
            return false;
        }
        return OutVideoInfo.IsValid();
    }

    FString VideoID;
    if (!VideoRenderer->TryGetStringField(TEXT("videoId"), VideoID) || VideoID.IsEmpty())
    {
        return false;
    }

    FString Title;
    if (VideoRenderer->HasTypedField<EJson::Object>(TEXT("title")))
    {
        Title = ExtractYouTubeTextObject(VideoRenderer->GetObjectField(TEXT("title")));
    }

    if (Title.IsEmpty())
    {
        return false;
    }

    FString Description;
    if (VideoRenderer->HasTypedField<EJson::Object>(TEXT("descriptionSnippet")))
    {
        Description = ExtractYouTubeTextObject(VideoRenderer->GetObjectField(TEXT("descriptionSnippet")));
    }

    FString Author;
    if (VideoRenderer->HasTypedField<EJson::Object>(TEXT("ownerText")))
    {
        Author = ExtractYouTubeTextObject(VideoRenderer->GetObjectField(TEXT("ownerText")));
    }
    else if (VideoRenderer->HasTypedField<EJson::Object>(TEXT("shortBylineText")))
    {
        Author = ExtractYouTubeTextObject(VideoRenderer->GetObjectField(TEXT("shortBylineText")));
    }

    FString PublishedText;
    if (VideoRenderer->HasTypedField<EJson::Object>(TEXT("publishedTimeText")))
    {
        PublishedText = ExtractYouTubeTextObject(VideoRenderer->GetObjectField(TEXT("publishedTimeText")));
    }

    OutVideoInfo.VideoID = VideoID.TrimStartAndEnd();
    OutVideoInfo.Title = DecodeEscapedJSONString(Title).TrimStartAndEnd();
    OutVideoInfo.Description = DecodeEscapedJSONString(Description).TrimStartAndEnd();
    OutVideoInfo.Author = DecodeEscapedJSONString(Author).TrimStartAndEnd();
    OutVideoInfo.VideoURL = FString::Printf(TEXT("https://www.youtube.com/watch?v=%s"), *OutVideoInfo.VideoID);
    OutVideoInfo.ThumbnailURL = GenerateThumbnailURL(OutVideoInfo.VideoID);
    OutVideoInfo.PublicationDate = FDateTime::Now();
    if (IsShortsURL(OutVideoInfo.VideoURL))
    {
        UE_LOG(LogConvaiEditor, Log, TEXT("YouTubeService: skipping YouTube Shorts entry: %s"), *OutVideoInfo.VideoURL);
        return false;
    }

    if (!PublishedText.IsEmpty())
    {
        OutVideoInfo.Duration = DecodeEscapedJSONString(PublishedText).TrimStartAndEnd();
    }

    return OutVideoInfo.IsValid();
}


bool FYouTubeService::ExtractRegexGroup(const FString &Content, const FString &PatternText, FString &OutValue) const
{
    if (Content.IsEmpty() || PatternText.IsEmpty())
    {
        return false;
    }

    const FRegexPattern Pattern(PatternText);
    FRegexMatcher Matcher(Pattern, Content);

    if (!Matcher.FindNext())
    {
        return false;
    }

    OutValue = Matcher.GetCaptureGroup(1);
    return !OutValue.IsEmpty();
}

FString FYouTubeService::DecodeEscapedJSONString(const FString &Input) const
{
    if (Input.IsEmpty())
    {
        return FString();
    }

    const FString Wrapped = FString::Printf(TEXT("\"%s\""), *Input);
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
    TSharedPtr<FJsonValue> ParsedValue;

    if (FJsonSerializer::Deserialize(Reader, ParsedValue) && ParsedValue.IsValid())
    {
        return ParsedValue->AsString();
    }

    FString Decoded = Input;
    Decoded.ReplaceInline(TEXT("\\u0026"), TEXT("&"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("\\u003c"), TEXT("<"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("\\u003e"), TEXT(">"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("\\\""), TEXT("\""), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("\\/"), TEXT("/"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("\\n"), TEXT("\n"), ESearchCase::CaseSensitive);

    return Decoded;
}

bool FYouTubeService::ExtractBalancedJSONObjectAfterToken(const FString &Content, const FString &StartToken, FString &OutJSON) const
{
    if (Content.IsEmpty() || StartToken.IsEmpty())
    {
        return false;
    }

    int32 TokenIndex = Content.Find(StartToken, ESearchCase::CaseSensitive);
    if (TokenIndex == INDEX_NONE)
    {
        return false;
    }

    int32 JsonStart = Content.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TokenIndex + StartToken.Len());
    if (JsonStart == INDEX_NONE)
    {
        return false;
    }

    int32 BraceDepth = 0;
    bool bInString = false;
    bool bEscapeNext = false;

    for (int32 Index = JsonStart; Index < Content.Len(); ++Index)
    {
        const TCHAR Char = Content[Index];

        if (bInString)
        {
            if (bEscapeNext)
            {
                bEscapeNext = false;
                continue;
            }

            if (Char == TEXT('\\'))
            {
                bEscapeNext = true;
                continue;
            }

            if (Char == TEXT('"'))
            {
                bInString = false;
            }

            continue;
        }

        if (Char == TEXT('"'))
        {
            bInString = true;
            continue;
        }

        if (Char == TEXT('{'))
        {
            ++BraceDepth;
            continue;
        }

        if (Char == TEXT('}'))
        {
            --BraceDepth;
            if (BraceDepth == 0)
            {
                OutJSON = Content.Mid(JsonStart, Index - JsonStart + 1);
                return !OutJSON.IsEmpty();
            }
        }
    }

    return false;
}

bool FYouTubeService::FindFirstVideoRendererJSON(const TSharedPtr<FJsonValue> &Value, TSharedPtr<FJsonObject> &OutRenderer) const
{
    if (!Value.IsValid())
    {
        return false;
    }

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid())
        {
            return false;
        }

        if (Object->HasTypedField<EJson::Object>(TEXT("videoRenderer")))
        {
            OutRenderer = Object->GetObjectField(TEXT("videoRenderer"));
            return OutRenderer.IsValid();
        }

        for (const auto &Field : Object->Values)
        {
            if (FindFirstVideoRendererJSON(Field.Value, OutRenderer))
            {
                return true;
            }
        }
    }
    else if (Value->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue> &ArrayValue : Value->AsArray())
        {
            if (FindFirstVideoRendererJSON(ArrayValue, OutRenderer))
            {
                return true;
            }
        }
    }

    return false;
}

bool FYouTubeService::FindFirstLockupViewModelJSON(const TSharedPtr<FJsonValue> &Value, TSharedPtr<FJsonObject> &OutRenderer) const
{
    if (!Value.IsValid())
    {
        return false;
    }

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid())
        {
            return false;
        }

        if (Object->HasTypedField<EJson::Object>(TEXT("lockupViewModel")))
        {
            OutRenderer = Object->GetObjectField(TEXT("lockupViewModel"));
            return OutRenderer.IsValid();
        }

        for (const auto &Field : Object->Values)
        {
            if (FindFirstLockupViewModelJSON(Field.Value, OutRenderer))
            {
                return true;
            }
        }
    }
    else if (Value->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue> &ArrayValue : Value->AsArray())
        {
            if (FindFirstLockupViewModelJSON(ArrayValue, OutRenderer))
            {
                return true;
            }
        }
    }

    return false;
}

bool FYouTubeService::FindFirstStringMatchingRegexJSON(const TSharedPtr<FJsonValue> &Value, const FString &PatternText, FString &OutValue) const
{
    if (!Value.IsValid() || PatternText.IsEmpty())
    {
        return false;
    }

    if (Value->Type == EJson::String)
    {
        const FString StringValue = Value->AsString();
        const FRegexPattern Pattern(PatternText);
        FRegexMatcher Matcher(Pattern, StringValue);
        if (Matcher.FindNext())
        {
            OutValue = Matcher.GetCaptureGroup(1);
            return !OutValue.IsEmpty();
        }
    }
    else if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid())
        {
            return false;
        }

        for (const auto &Field : Object->Values)
        {
            if (FindFirstStringMatchingRegexJSON(Field.Value, PatternText, OutValue))
            {
                return true;
            }
        }
    }
    else if (Value->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue> &ArrayValue : Value->AsArray())
        {
            if (FindFirstStringMatchingRegexJSON(ArrayValue, PatternText, OutValue))
            {
                return true;
            }
        }
    }

    return false;
}

FString FYouTubeService::ExtractYouTubeTextObject(const TSharedPtr<FJsonObject> &TextObject) const
{
    if (!TextObject.IsValid())
    {
        return FString();
    }

    FString SimpleText;
    if (TextObject->TryGetStringField(TEXT("simpleText"), SimpleText) && !SimpleText.IsEmpty())
    {
        return SimpleText;
    }

    if (TextObject->HasTypedField<EJson::Array>(TEXT("runs")))
    {
        FString CombinedText;
        const TArray<TSharedPtr<FJsonValue>> &Runs = TextObject->GetArrayField(TEXT("runs"));
        for (const TSharedPtr<FJsonValue> &RunValue : Runs)
        {
            if (!RunValue.IsValid() || RunValue->Type != EJson::Object)
            {
                continue;
            }

            const TSharedPtr<FJsonObject> RunObject = RunValue->AsObject();
            if (!RunObject.IsValid())
            {
                continue;
            }

            FString RunText;
            if (RunObject->TryGetStringField(TEXT("text"), RunText))
            {
                CombinedText += RunText;
            }
        }

        if (!CombinedText.IsEmpty())
        {
            return CombinedText;
        }
    }

    return FString();
}

FString FYouTubeService::DecodeHTMLEntities(const FString &Input) const
{
    if (Input.IsEmpty())
    {
        return FString();
    }

    FString Decoded = Input;
    Decoded.ReplaceInline(TEXT("&amp;"), TEXT("&"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("&quot;"), TEXT("\""), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("&#34;"), TEXT("\""), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("&apos;"), TEXT("'"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("&#39;"), TEXT("'"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("&lt;"), TEXT("<"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("&gt;"), TEXT(">"), ESearchCase::CaseSensitive);
    Decoded.ReplaceInline(TEXT("\\u0026"), TEXT("&"), ESearchCase::CaseSensitive);
    return Decoded;
}

bool FYouTubeService::IsShortsURL(const FString &VideoURL) const
{
    return VideoURL.Contains(TEXT("/shorts/"), ESearchCase::IgnoreCase);
}

FString FYouTubeService::GenerateThumbnailURL(const FString &VideoID) const
{
    return FString::Printf(TEXT("https://img.youtube.com/vi/%s/maxresdefault.jpg"), *VideoID);
}
