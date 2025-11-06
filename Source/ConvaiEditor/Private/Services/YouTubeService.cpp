/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * YouTubeService.cpp
 *
 * Implementation of YouTube RSS feed service.
 */

#include "Services/YouTubeService.h"
#include "ConvaiEditor.h"
#include "Async/HttpAsyncOperation.h"
#include "Async/Async.h"

static const FString CONVAI_CHANNEL_RSS_URL = TEXT("https://www.youtube.com/feeds/videos.xml?channel_id=UCcYtXgiavJYMKSirsk6VNsw");

FYouTubeService::FYouTubeService()
    : bIsFetching(false), LastFetchTime(FDateTime::MinValue()), bIsInitialized(false)
{
}

void FYouTubeService::Startup()
{
    bIsInitialized = false;

    ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
    CircuitConfig.Name = TEXT("YouTubeRSS");
    CircuitConfig.FailureThreshold = 3;
    CircuitConfig.SuccessThreshold = 2;
    CircuitConfig.OpenTimeoutSeconds = 60.0f;
    CircuitConfig.bEnableLogging = false;
    CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

    ConvaiEditor::FRetryPolicyConfig RetryConfig;
    RetryConfig.Name = TEXT("YouTubeRSS");
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
    bIsFetching = false;
    bIsInitialized = false;
    CachedVideoInfo.Reset();
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
        OnFailure.ExecuteIfBound(TEXT("YouTube RSS circuit breaker is open - service temporarily unavailable"));
        return;
    }

    if (bIsFetching)
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: video fetch already in progress - request ignored"));
        OnFailure.ExecuteIfBound(TEXT("Fetch already in progress"));
        return;
    }

    bIsFetching = true;

    ConvaiEditor::FHttpAsyncRequest HttpRequest(CONVAI_CHANNEL_RSS_URL);
    HttpRequest.WithVerb(TEXT("GET"))
        .WithHeader(TEXT("User-Agent"), TEXT("UnrealEngine/ConvaiPlugin"))
        .WithHeader(TEXT("Accept"), TEXT("application/rss+xml, application/xml, text/xml"))
        .WithTimeout(30.0f);

    TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> AsyncOp;

    if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
    {
        AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(
            HttpRequest,
            CircuitBreaker,
            RetryPolicy,
            nullptr);
    }
    else
    {
        AsyncOp = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, nullptr);
    }

    AsyncOp->OnComplete([this, OnSuccess, OnFailure, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
                        {
        bIsFetching = false;

        if (!Result.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService HTTP request failed: %s"), *Result.GetError());
            OnFailure.ExecuteIfBound(Result.GetError());
            return;
        }

        const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
        
        if (!HttpResponse.IsSuccess())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService HTTP error: %d"), HttpResponse.ResponseCode);
            OnFailure.ExecuteIfBound(FString::Printf(TEXT("HTTP error %d"), HttpResponse.ResponseCode));
            return;
        }

        if (HttpResponse.Body.IsEmpty())
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: empty response"));
            OnFailure.ExecuteIfBound(TEXT("Empty response"));
            return;
        }

        FString ResponsePreview = HttpResponse.Body.Left(500);
        UE_LOG(LogConvaiEditor, VeryVerbose, TEXT("YouTubeService: RSS response preview: %s"), *ResponsePreview);

        FYouTubeVideoInfo VideoInfo;
        if (ParseRSSFeed(HttpResponse.Body, VideoInfo))
        {
            if (VideoInfo.IsValid())
            {
                CachedVideoInfo = VideoInfo;
                LastFetchTime = FDateTime::Now();

                AsyncTask(ENamedThreads::GameThread, [OnSuccess, VideoInfo]()
                {
                    OnSuccess.ExecuteIfBound(VideoInfo);
                });
            }
            else
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: invalid video data"));
                AsyncTask(ENamedThreads::GameThread, [OnFailure]()
                {
                    OnFailure.ExecuteIfBound(TEXT("Invalid video data"));
                });
            }
        }
        else
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: failed to parse RSS feed"));
            AsyncTask(ENamedThreads::GameThread, [OnFailure]()
            {
                OnFailure.ExecuteIfBound(TEXT("Failed to parse RSS feed"));
            });
        } });

    AsyncOp->Start();
}

TOptional<FYouTubeVideoInfo> FYouTubeService::GetCachedVideoInfo() const
{
    return CachedVideoInfo;
}

bool FYouTubeService::IsFetching() const
{
    return bIsFetching;
}

bool FYouTubeService::ParseRSSFeed(const FString &XMLContent, FYouTubeVideoInfo &OutVideoInfo)
{
    UE_LOG(LogConvaiEditor, VeryVerbose, TEXT("YouTubeService: Starting RSS feed parsing, content length: %d"), XMLContent.Len());

    int32 EntryStart = XMLContent.Find(TEXT("<entry"));
    if (EntryStart == INDEX_NONE)
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: no entry found in RSS feed"));
        return false;
    }

    UE_LOG(LogConvaiEditor, VeryVerbose, TEXT("YouTubeService: Found entry at position: %d"), EntryStart);

    FString RemainingContent = XMLContent.Mid(EntryStart);
    int32 RelativeEntryEnd = RemainingContent.Find(TEXT("</entry>"));
    int32 EntryEnd = (RelativeEntryEnd != INDEX_NONE) ? EntryStart + RelativeEntryEnd : INDEX_NONE;
    if (EntryEnd == INDEX_NONE)
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: entry closing tag not found in RSS feed"));
        return false;
    }

    FString EntryContent = XMLContent.Mid(EntryStart, EntryEnd - EntryStart + 8);

    FString Title, VideoID, Description, PublishedDate, Author;

    if (!ParseXMLElement(EntryContent, TEXT("title"), Title))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: title not found in RSS entry"));
    }
    else
    {
        UE_LOG(LogConvaiEditor, VeryVerbose, TEXT("YouTubeService: Found title: %s"), *Title);
    }

    if (!ParseXMLElement(EntryContent, TEXT("yt:videoId"), VideoID))
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("YouTubeService: video ID not found in RSS entry"));
    }
    else
    {
        UE_LOG(LogConvaiEditor, VeryVerbose, TEXT("YouTubeService: Found video ID: %s"), *VideoID);
    }

    if (!ParseXMLElement(EntryContent, TEXT("media:description"), Description))
    {
    }

    if (!ParseXMLElement(EntryContent, TEXT("published"), PublishedDate))
    {
    }

    if (!ParseXMLElement(EntryContent, TEXT("name"), Author))
    {
    }

    if (Title.IsEmpty() && VideoID.IsEmpty())
    {
        UE_LOG(LogConvaiEditor, Error, TEXT("YouTubeService: missing essential video data (title and video ID)"));
        return false;
    }

    OutVideoInfo.Title = ExtractCDATA(Title).TrimStartAndEnd();
    OutVideoInfo.VideoID = VideoID.TrimStartAndEnd();
    OutVideoInfo.Description = ExtractCDATA(Description).TrimStartAndEnd();
    OutVideoInfo.Author = Author.TrimStartAndEnd();

    if (!OutVideoInfo.VideoID.IsEmpty())
    {
        OutVideoInfo.VideoURL = FString::Printf(TEXT("https://www.youtube.com/watch?v=%s"), *OutVideoInfo.VideoID);
        OutVideoInfo.ThumbnailURL = GenerateThumbnailURL(OutVideoInfo.VideoID);
    }

    if (!PublishedDate.IsEmpty())
    {
        FDateTime::ParseIso8601(*PublishedDate, OutVideoInfo.PublicationDate);
    }

    return !OutVideoInfo.Title.IsEmpty() && !OutVideoInfo.VideoURL.IsEmpty() && !OutVideoInfo.ThumbnailURL.IsEmpty();
}

/** Parses XML using simple string parsing for lightweight operation */
bool FYouTubeService::ParseXMLElement(const FString &XMLContent, const FString &ElementName, FString &OutValue) const
{
    const FString OpenTag = FString::Printf(TEXT("<%s>"), *ElementName);
    const FString CloseTag = FString::Printf(TEXT("</%s>"), *ElementName);

    int32 OpenIndex = XMLContent.Find(OpenTag);
    if (OpenIndex == INDEX_NONE)
    {
        const FString SelfClosingPattern = FString::Printf(TEXT("<%s"), *ElementName);
        int32 SelfClosingStart = XMLContent.Find(SelfClosingPattern);
        if (SelfClosingStart != INDEX_NONE)
        {
            FString RemainingTag = XMLContent.Mid(SelfClosingStart);
            int32 RelativeTagEnd = RemainingTag.Find(TEXT(">"));
            int32 TagEnd = (RelativeTagEnd != INDEX_NONE) ? SelfClosingStart + RelativeTagEnd : INDEX_NONE;
            if (TagEnd != INDEX_NONE)
            {
                FString TagContent = XMLContent.Mid(SelfClosingStart, TagEnd - SelfClosingStart + 1);
                return false;
            }
        }
        return false;
    }

    FString RemainingXML = XMLContent.Mid(OpenIndex);
    int32 RelativeCloseIndex = RemainingXML.Find(CloseTag);
    int32 CloseIndex = (RelativeCloseIndex != INDEX_NONE) ? OpenIndex + RelativeCloseIndex : INDEX_NONE;
    if (CloseIndex == INDEX_NONE)
    {
        return false;
    }

    int32 ContentStart = OpenIndex + OpenTag.Len();
    OutValue = XMLContent.Mid(ContentStart, CloseIndex - ContentStart);
    return true;
}

FString FYouTubeService::ExtractCDATA(const FString &Content) const
{
    const FString CDATAStart = TEXT("<![CDATA[");
    const FString CDATAEnd = TEXT("]]>");

    if (Content.Contains(CDATAStart))
    {
        int32 StartIndex = Content.Find(CDATAStart);
        if (StartIndex != INDEX_NONE)
        {
            int32 SearchStart = StartIndex + CDATAStart.Len();
            FString RemainingContent = Content.Mid(SearchStart);
            int32 RelativeEndIndex = RemainingContent.Find(CDATAEnd);
            int32 EndIndex = (RelativeEndIndex != INDEX_NONE) ? SearchStart + RelativeEndIndex : INDEX_NONE;
            if (EndIndex != INDEX_NONE)
            {
                int32 ContentStart = StartIndex + CDATAStart.Len();
                return Content.Mid(ContentStart, EndIndex - ContentStart);
            }
        }
    }
    return Content;
}

/** Extracts video ID from various YouTube URL formats */
FString FYouTubeService::ExtractVideoIDFromURL(const FString &URL) const
{
    int32 VIndex = URL.Find(TEXT("v="));
    if (VIndex != INDEX_NONE)
    {
        int32 StartIndex = VIndex + 2;
        FString RemainingURL = URL.Mid(StartIndex);
        int32 RelativeEndIndex = RemainingURL.Find(TEXT("&"));
        int32 EndIndex = (RelativeEndIndex != INDEX_NONE) ? StartIndex + RelativeEndIndex : INDEX_NONE;
        if (EndIndex == INDEX_NONE)
        {
            EndIndex = URL.Len();
        }
        return URL.Mid(StartIndex, EndIndex - StartIndex);
    }

    if (URL.Contains(TEXT("youtu.be/")))
    {
        int32 SlashIndex = URL.Find(TEXT("youtu.be/"));
        if (SlashIndex != INDEX_NONE)
        {
            int32 StartIndex = SlashIndex + 9;
            FString RemainingURL = URL.Mid(StartIndex);
            int32 RelativeEndIndex = RemainingURL.Find(TEXT("?"));
            int32 EndIndex = (RelativeEndIndex != INDEX_NONE) ? StartIndex + RelativeEndIndex : INDEX_NONE;
            if (EndIndex == INDEX_NONE)
            {
                EndIndex = URL.Len();
            }
            return URL.Mid(StartIndex, EndIndex - StartIndex);
        }
    }

    return FString();
}

FString FYouTubeService::GenerateThumbnailURL(const FString &VideoID) const
{
    return FString::Printf(TEXT("https://img.youtube.com/vi/%s/maxresdefault.jpg"), *VideoID);
}
