/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * RemoteContentFeedProvider.h
 *
 * Remote content feed provider via HTTP.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/IContentFeedProvider.h"
#include "Async/HttpAsyncOperation.h"
#include "Async/CancellationToken.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"

/**
 * Fetches content feeds from remote CDN via HTTP.
 */
class FRemoteContentFeedProvider : public IContentFeedProvider
{
public:
	struct FConfig
	{
		/** Remote JSON URL (jsDelivr CDN or any HTTP endpoint) */
		FString URL;

		/** Content type to fetch */
		EContentType ContentType = EContentType::Announcements;

		/** Request timeout in seconds */
		float TimeoutSeconds = 10.0f;

		/** Maximum retry attempts */
		int32 MaxRetries = 2;

		/** Retry delay in seconds */
		float RetryDelaySeconds = 1.0f;

		/** Default constructor with sensible defaults */
		FConfig()
			: TimeoutSeconds(10.0f), MaxRetries(2), RetryDelaySeconds(1.0f)
		{
		}
	};

	/**
	 * Constructor
	 * @param InConfig Configuration for this provider
	 */
	explicit FRemoteContentFeedProvider(const FConfig &InConfig);

	/** Destructor */
	virtual ~FRemoteContentFeedProvider() override = default;

	// IContentFeedProvider interface
	virtual TFuture<FContentFeedFetchResult> FetchContentAsync() override;
	virtual FString GetProviderName() const override { return TEXT("RemoteContentFeedProvider"); }
	virtual bool IsAvailable() const override;

	/**
	 * Set cancellation token for the next fetch operation
	 * @param Token Cancellation token
	 */
	void SetCancellationToken(TSharedPtr<ConvaiEditor::FCancellationToken> Token) { CancellationToken = Token; }

private:
	/** Configuration */
	FConfig Config;

	/** Cancellation token for async operations */
	TSharedPtr<ConvaiEditor::FCancellationToken> CancellationToken;

	/**
	 * Parse JSON response into announcement feed
	 * @param JsonString Raw JSON string
	 * @param OutFeed Parsed feed (output)
	 * @param OutErrorMessage Error message if parsing fails (output)
	 * @return true if parsing succeeded
	 */
	bool ParseJsonResponse(const FString &JsonString, FConvaiAnnouncementFeed &OutFeed, FString &OutErrorMessage) const;

	/**
	 * Parse JSON response into changelog feed
	 * @param JsonString Raw JSON string
	 * @param OutFeed Parsed feed (output)
	 * @param OutErrorMessage Error message if parsing fails (output)
	 * @return true if parsing succeeded
	 */
	bool ParseChangelogJsonResponse(const FString &JsonString, FConvaiChangelogFeed &OutFeed, FString &OutErrorMessage) const;

	/**
	 * Validate URL is properly configured
	 * @return true if URL is valid
	 */
	bool IsConfigValid() const;

	//----------------------------------------
	// Error Recovery & Resilience
	//----------------------------------------

	/** Circuit breaker for CDN endpoint (shared across all requests) */
	TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;

	/** Retry policy for transient failures (shared across all requests) */
	TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;
};
