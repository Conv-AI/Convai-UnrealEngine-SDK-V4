/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * RemoteContentFeedProvider.cpp
 *
 * Implementation of remote content feed provider.
 */

#include "Services/RemoteContentFeedProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Logging/ConvaiEditorConfigLog.h"
#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Containers/Set.h"

FRemoteContentFeedProvider::FRemoteContentFeedProvider(const FConfig &InConfig)
	: Config(InConfig)
{
	FString SourceIdentifier = Config.SourceName;
	if (SourceIdentifier.IsEmpty())
	{
		FString FileName = Config.URL;
		int32 LastSlashIndex;
		if (Config.URL.FindLastChar('/', LastSlashIndex))
		{
			FileName = Config.URL.RightChop(LastSlashIndex + 1);
		}
		FileName = FileName.Replace(TEXT(".json"), TEXT(""));
		int32 QueryIndex;
		if (FileName.FindChar('?', QueryIndex))
		{
			FileName = FileName.Left(QueryIndex);
		}

		SourceIdentifier = FileName.IsEmpty() ? TEXT("unknown") : FileName;
	}

	ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
	CircuitConfig.Name = FString::Printf(TEXT("ContentFeedCDN_%s"), *SourceIdentifier);
	CircuitConfig.FailureThreshold = 3;
	CircuitConfig.SuccessThreshold = 2;
	CircuitConfig.OpenTimeoutSeconds = 30.0f;
	CircuitConfig.bEnableLogging = true;
	CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

	ConvaiEditor::FRetryPolicyConfig RetryConfig;
	RetryConfig.Name = TEXT("ContentFeedCDN");
	RetryConfig.MaxAttempts = Config.MaxRetries;
	RetryConfig.BaseDelaySeconds = Config.RetryDelaySeconds;
	RetryConfig.MaxDelaySeconds = 10.0f;
	RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Fixed;
	RetryConfig.bEnableJitter = false;
	RetryConfig.bEnableLogging = true;
	RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
	RetryPolicy = MakeShared<ConvaiEditor::FRetryPolicy>(RetryConfig);
}

bool FRemoteContentFeedProvider::IsAvailable() const
{
	return IsConfigValid() && FHttpModule::Get().IsHttpEnabled();
}

bool FRemoteContentFeedProvider::IsConfigValid() const
{
	return !Config.URL.IsEmpty() && Config.URL.StartsWith(TEXT("http"));
}

TArray<FString> FRemoteContentFeedProvider::BuildCandidateURLs() const
{
	TArray<FString> URLs;
	TSet<FString> SeenURLs;

	auto AddUniqueUrl = [&URLs, &SeenURLs](const FString &Candidate)
	{
		const FString Trimmed = Candidate.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("http")) || SeenURLs.Contains(Trimmed))
		{
			return;
		}

		SeenURLs.Add(Trimmed);
		URLs.Add(Trimmed);
	};

	AddUniqueUrl(Config.URL);
	for (const FString &FallbackURL : Config.FallbackURLs)
	{
		AddUniqueUrl(FallbackURL);
	}

	return URLs;
}

TFuture<FContentFeedFetchResult> FRemoteContentFeedProvider::FetchContentAsync()
{
	if (!IsConfigValid())
	{
		UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedProvider configuration error: URL is empty or malformed"));
		return Async(EAsyncExecution::TaskGraphMainThread, []()
					 { return FContentFeedFetchResult::Error(TEXT("Invalid provider configuration"), 0); });
	}

	if (!FHttpModule::Get().IsHttpEnabled())
	{
		UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedProvider HTTP error: module not enabled"));
		return Async(EAsyncExecution::TaskGraphMainThread, []()
					 { return FContentFeedFetchResult::Error(TEXT("HTTP module not available"), 0); });
	}

	const TArray<FString> CandidateURLs = BuildCandidateURLs();
	if (CandidateURLs.Num() == 0)
	{
		UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedProvider configuration error: no valid candidate endpoints"));
		return Async(EAsyncExecution::TaskGraphMainThread, []()
					 { return FContentFeedFetchResult::Error(TEXT("No valid endpoints configured"), 0); });
	}

	TSharedPtr<TPromise<FContentFeedFetchResult>> Promise = MakeShared<TPromise<FContentFeedFetchResult>>();
	TFuture<FContentFeedFetchResult> Future = Promise->GetFuture();
	TSharedPtr<TArray<FString>> AttemptErrors = MakeShared<TArray<FString>>();
	TSharedPtr<int32> LastResponseCode = MakeShared<int32>(0);

	EContentType ContentType = Config.ContentType;
	TSharedPtr<TFunction<void(int32)>> TryEndpoint = MakeShared<TFunction<void(int32)>>();

	*TryEndpoint = [this, CandidateURLs, Promise, AttemptErrors, LastResponseCode, ContentType, TryEndpoint](int32 EndpointIndex)
	{
		if (EndpointIndex >= CandidateURLs.Num())
		{
			const FString CombinedErrors = FString::Join(*AttemptErrors, TEXT("; "));
			const FString ErrorMessage = FString::Printf(
				TEXT("All endpoints failed for '%s': %s"),
				*Config.URL,
				CombinedErrors.IsEmpty() ? TEXT("Unknown failure") : *CombinedErrors);
			Promise->SetValue(FContentFeedFetchResult::Error(ErrorMessage, *LastResponseCode));
			return;
		}

		const FString EndpointURL = CandidateURLs[EndpointIndex];
		ConvaiEditor::FHttpAsyncRequest HttpRequest(EndpointURL);
		HttpRequest.WithTimeout(Config.TimeoutSeconds)
			.WithHeader(TEXT("Accept"), TEXT("application/json"))
			.WithHeader(TEXT("Cache-Control"), TEXT("no-cache"))
			.WithHeader(TEXT("Pragma"), TEXT("no-cache"));

		TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> AsyncOp;
		if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
		{
			AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(
				HttpRequest,
				CircuitBreaker,
				RetryPolicy,
				CancellationToken);
		}
		else if (CircuitBreaker.IsValid())
		{
			AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithCircuitBreaker(
				HttpRequest,
				CircuitBreaker,
				CancellationToken);
		}
		else if (RetryPolicy.IsValid())
		{
			AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithRetry(
				HttpRequest,
				RetryPolicy,
				CancellationToken);
		}
		else
		{
			AsyncOp = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, CancellationToken);
		}

		AsyncOp->OnComplete([this, EndpointIndex, EndpointURL, Promise, AttemptErrors, LastResponseCode, ContentType, TryEndpoint, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
							{
			if (!Result.IsSuccess())
			{
				AttemptErrors->Add(FString::Printf(TEXT("[%s] request failed: %s"), *EndpointURL, *Result.GetError()));
				(*TryEndpoint)(EndpointIndex + 1);
				return;
			}

			const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
			*LastResponseCode = HttpResponse.ResponseCode;

			FString ParseErrorMessage;
			if (ContentType == EContentType::Announcements)
			{
				FConvaiAnnouncementFeed Feed;
				if (ParseJsonResponse(HttpResponse.Body, Feed, ParseErrorMessage))
				{
					UE_LOG(LogConvaiEditorConfig, Log,
						   TEXT("ContentFeedProvider selected endpoint: %s (code=%d, lastUpdated=%s)"),
						   *EndpointURL, HttpResponse.ResponseCode, *Feed.LastUpdated.ToIso8601());
					Promise->SetValue(FContentFeedFetchResult::Success(MoveTemp(Feed)));
					return;
				}
			}
			else if (ContentType == EContentType::Changelogs)
			{
				FConvaiChangelogFeed Feed;
				if (ParseChangelogJsonResponse(HttpResponse.Body, Feed, ParseErrorMessage))
				{
					UE_LOG(LogConvaiEditorConfig, Log,
						   TEXT("ContentFeedProvider selected endpoint: %s (code=%d, lastUpdated=%s)"),
						   *EndpointURL, HttpResponse.ResponseCode, *Feed.LastUpdated.ToIso8601());
					Promise->SetValue(FContentFeedFetchResult::SuccessChangelog(MoveTemp(Feed)));
					return;
				}
			}
			else
			{
				Promise->SetValue(FContentFeedFetchResult::Error(TEXT("Unknown content type"), HttpResponse.ResponseCode));
				return;
			}

			const FString ParseError = ParseErrorMessage.IsEmpty() ? TEXT("Payload parse failed") : ParseErrorMessage;
			AttemptErrors->Add(FString::Printf(
				TEXT("[%s] parse failed (code=%d): %s"),
				*EndpointURL,
				HttpResponse.ResponseCode,
				*ParseError));

			(*TryEndpoint)(EndpointIndex + 1); });

		AsyncOp->Start();
	};

	(*TryEndpoint)(0);
	return Future;
}

bool FRemoteContentFeedProvider::ParseJsonResponse(
	const FString &JsonString,
	FConvaiAnnouncementFeed &OutFeed,
	FString &OutErrorMessage) const
{
	if (JsonString.IsEmpty())
	{
		OutErrorMessage = TEXT("Empty JSON response");
		return false;
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OutErrorMessage = TEXT("Failed to parse JSON");
		return false;
	}

	OutFeed = FConvaiAnnouncementFeed::FromJson(JsonObject);

	if (!OutFeed.IsValid())
	{
		OutErrorMessage = TEXT("Parsed feed is invalid");
		return false;
	}

	return true;
}

bool FRemoteContentFeedProvider::ParseChangelogJsonResponse(
	const FString &JsonString,
	FConvaiChangelogFeed &OutFeed,
	FString &OutErrorMessage) const
{
	if (JsonString.IsEmpty())
	{
		OutErrorMessage = TEXT("Empty JSON response");
		return false;
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OutErrorMessage = TEXT("Failed to parse JSON");
		return false;
	}

	OutFeed = FConvaiChangelogFeed::FromJson(JsonObject);

	if (!OutFeed.IsValid())
	{
		OutErrorMessage = TEXT("Parsed changelog feed is invalid");
		return false;
	}

	return true;
}
