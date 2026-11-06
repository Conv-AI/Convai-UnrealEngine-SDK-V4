/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ContentFeedService.h
 *
 * Content feed service implementation.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/IContentFeedService.h"
#include "Services/IContentFeedProvider.h"
#include "Services/ContentFeedCacheManager.h"

/** Coordinates provider and cache for content feeds. */
class FContentFeedService : public IContentFeedService
{
public:
	FContentFeedService(
		TUniquePtr<IContentFeedProvider> InProvider,
		TUniquePtr<FContentFeedCacheManager> InCacheManager,
		EContentFeedType InContentType = EContentFeedType::Announcements);

	/** Destructor */
	virtual ~FContentFeedService() override = default;

	// IContentFeedService interface
	virtual TFuture<FContentFeedResult> GetContentAsync(bool bForceRefresh = false) override;
	virtual TFuture<FContentFeedResult> RefreshContentAsync() override;
	virtual bool HasCachedData() const override;
	virtual double GetCacheAge() const override;

private:
	/** Content feed provider (injected dependency) */
	TUniquePtr<IContentFeedProvider> Provider;

	/** Cache manager (injected dependency) */
	TUniquePtr<FContentFeedCacheManager> CacheManager;

	/** Content type this service handles */
	EContentFeedType ContentType;

	/** Mutex for thread-safe operations */
	mutable FCriticalSection ServiceMutex;

	/** Fetch from remote and update cache */
	TFuture<FContentFeedResult> FetchFromRemoteAsync();
};
