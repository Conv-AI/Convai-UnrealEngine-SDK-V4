/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ContentFeedCacheManager.h
 *
 * Thread-safe cache manager for content feeds.
 */

#pragma once

#include "CoreMinimal.h"
#include "Models/ConvaiAnnouncementData.h"
#include "HAL/CriticalSection.h"

/** Content type for cache manager */
enum class EContentFeedCacheType : uint8
{
	Announcements,
	Changelogs
};

/**
 * Thread-safe cache manager with disk persistence.
 */
class FContentFeedCacheManager
{
public:
	struct FConfig
	{
		/** Content type for this cache */
		EContentFeedCacheType ContentType;

		/** Cache TTL in seconds (default: 1 hour) */
		double TTLSeconds = 3600.0;

		/** Cache file name (allows separate caches for different content types) */
		FString CacheFileName = TEXT("content_feed_cache.json");

		/** Whether to enable disk persistence */
		bool bEnableDiskCache = true;

		FConfig() = default;
	};

	/**
	 * Constructor
	 * @param InConfig Cache configuration
	 */
	explicit FContentFeedCacheManager(const FConfig &InConfig = FConfig());

	/** Destructor - ensures cache is written to disk */
	~FContentFeedCacheManager();

	/**
	 * Get cached announcement feed if valid
	 *
	 * Thread-safe operation.
	 * Checks memory cache, then disk cache if needed.
	 * Validates TTL before returning.
	 *
	 * @return Optional feed (has value if cache is valid and not expired)
	 */
	TOptional<FConvaiAnnouncementFeed> GetCached();

	/**
	 * Get cached changelog feed if valid
	 *
	 * Thread-safe operation.
	 * Checks memory cache, then disk cache if needed.
	 * Validates TTL before returning.
	 *
	 * @return Optional feed (has value if cache is valid and not expired)
	 */
	TOptional<FConvaiChangelogFeed> GetCachedChangelogs();

	/**
	 * Save announcement feed to cache
	 *
	 * Thread-safe operation.
	 * Saves to both memory and disk (if enabled).
	 * Updates cache timestamp.
	 *
	 * @param Feed Feed to cache
	 * @return true if saved successfully
	 */
	bool SaveToCache(const FConvaiAnnouncementFeed &Feed);

	/**
	 * Save changelog feed to cache
	 *
	 * Thread-safe operation.
	 * Saves to both memory and disk (if enabled).
	 * Updates cache timestamp.
	 *
	 * @param Feed Feed to cache
	 * @return true if saved successfully
	 */
	bool SaveToCache(const FConvaiChangelogFeed &Feed);

	/**
	 * Invalidate cache (force refresh on next get)
	 *
	 * Thread-safe operation.
	 * Clears memory cache and deletes disk cache file.
	 */
	void InvalidateCache();

	/**
	 * Check if cache exists and is valid (not expired)
	 *
	 * Thread-safe operation.
	 *
	 * @return true if cache exists and TTL is valid
	 */
	bool IsCacheValid() const;

	/**
	 * Get cache age in seconds
	 *
	 * @return Seconds since cache was last updated, or -1 if no cache
	 */
	double GetCacheAge() const;

	/**
	 * Get full path to cache file
	 * @return Absolute path to cache file
	 */
	FString GetCacheFilePath() const;

private:
	/** Configuration */
	FConfig Config;

	/** In-memory cached announcement feed */
	TOptional<FConvaiAnnouncementFeed> AnnouncementMemoryCache;

	/** In-memory cached changelog feed */
	TOptional<FConvaiChangelogFeed> ChangelogMemoryCache;

	/** Timestamp when cache was last updated */
	FDateTime CacheTimestamp;

	/** Mutex for thread-safe operations */
	mutable FCriticalSection CacheMutex;

	/**
	 * Load announcement cache from disk
	 * NOT thread-safe - caller must hold CacheMutex
	 * @return Optional feed from disk
	 */
	TOptional<FConvaiAnnouncementFeed> LoadFromDisk();

	/**
	 * Load changelog cache from disk
	 * NOT thread-safe - caller must hold CacheMutex
	 * @return Optional feed from disk
	 */
	TOptional<FConvaiChangelogFeed> LoadChangelogsFromDisk();

	/**
	 * Save announcement cache to disk
	 * NOT thread-safe - caller must hold CacheMutex
	 * @param Feed Feed to save
	 * @return true if saved successfully
	 */
	bool SaveToDisk(const FConvaiAnnouncementFeed &Feed);

	/**
	 * Save changelog cache to disk
	 * NOT thread-safe - caller must hold CacheMutex
	 * @param Feed Feed to save
	 * @return true if saved successfully
	 */
	bool SaveToDisk(const FConvaiChangelogFeed &Feed);

	/**
	 * Check if cache timestamp is within TTL
	 * NOT thread-safe - caller must hold CacheMutex
	 * @return true if cache is not expired
	 */
	bool IsCacheFresh() const;

	/**
	 * Get cache directory path
	 * @return Directory path for cache files
	 */
	FString GetCacheDirectory() const;
};
