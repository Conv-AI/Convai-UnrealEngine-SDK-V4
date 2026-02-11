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

FRemoteContentFeedProvider::FRemoteContentFeedProvider(const FConfig &InConfig)
	: Config(InConfig)
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

	ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
	CircuitConfig.Name = FString::Printf(TEXT("ContentFeedCDN_%s"), *FileName);
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

	ConvaiEditor::FHttpAsyncRequest HttpRequest(Config.URL);
	HttpRequest.WithTimeout(Config.TimeoutSeconds)
		.WithHeader(TEXT("Accept"), TEXT("application/json"))
		.WithHeader(TEXT("Cache-Control"), TEXT("max-age=300"));

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

	TSharedPtr<TPromise<FContentFeedFetchResult>> Promise = MakeShared<TPromise<FContentFeedFetchResult>>();
	TFuture<FContentFeedFetchResult> Future = Promise->GetFuture();

	EContentType ContentType = Config.ContentType;

	AsyncOp->OnComplete([this, Promise, ContentType, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
						{
		if (!Result.IsSuccess())
		{
			UE_LOG(LogConvaiEditorConfig, Error, TEXT("ContentFeedProvider HTTP request failed: %s"), *Result.GetError());
			Promise->SetValue(FContentFeedFetchResult::Error(Result.GetError(), 0));
			return;
		}

		const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();

		FString ErrorMessage;
		if (ContentType == EContentType::Announcements)
		{
			FConvaiAnnouncementFeed Feed;
			if (ParseJsonResponse(HttpResponse.Body, Feed, ErrorMessage))
			{
				Promise->SetValue(FContentFeedFetchResult::Success(MoveTemp(Feed)));
			}
			else
			{
				Promise->SetValue(FContentFeedFetchResult::Error(ErrorMessage, HttpResponse.ResponseCode));
			}
		}
		else if (ContentType == EContentType::Changelogs)
		{
			FConvaiChangelogFeed Feed;
			if (ParseChangelogJsonResponse(HttpResponse.Body, Feed, ErrorMessage))
			{
				Promise->SetValue(FContentFeedFetchResult::SuccessChangelog(MoveTemp(Feed)));
			}
			else
			{
				Promise->SetValue(FContentFeedFetchResult::Error(ErrorMessage, HttpResponse.ResponseCode));
			}
		}
		else
		{
			Promise->SetValue(FContentFeedFetchResult::Error(TEXT("Unknown content type"), HttpResponse.ResponseCode));
		} });

	AsyncOp->Start();

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
