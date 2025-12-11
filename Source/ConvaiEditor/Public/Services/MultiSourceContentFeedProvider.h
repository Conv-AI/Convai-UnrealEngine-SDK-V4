/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * MultiSourceContentFeedProvider.h
 *
 * Multi-source content provider with merging capabilities.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/IContentFeedProvider.h"
#include "Services/RemoteContentFeedProvider.h"
#include "Async/Future.h"

/**
 * Fetches and merges content from multiple sources.
 */
class FMultiSourceContentFeedProvider : public IContentFeedProvider
{
public:
    struct FMultiSourceConfig
    {
        /** Array of source URLs to fetch from */
        TArray<FString> SourceURLs;

        /** Content type to fetch (announcements or changelogs) */
        EContentType ContentType = EContentType::Announcements;

        /** Base provider config (timeout, retries, etc.) */
        FRemoteContentFeedProvider::FConfig BaseConfig;

        /** Whether to require all sources to succeed (default: false - partial success OK) */
        bool bRequireAllSources = false;

        /** Whether to deduplicate by ID when merging (default: true) */
        bool bDeduplicateByID = true;

        FMultiSourceConfig() = default;

        explicit FMultiSourceConfig(const TArray<FString> &InURLs)
            : SourceURLs(InURLs)
        {
        }
    };

    /**
     * Constructor with multi-source configuration
     * @param InConfig Multi-source configuration
     */
    explicit FMultiSourceContentFeedProvider(const FMultiSourceConfig &InConfig);

    /** Destructor */
    virtual ~FMultiSourceContentFeedProvider() override = default;

    // IContentFeedProvider interface
    virtual bool IsAvailable() const override;
    virtual TFuture<FContentFeedFetchResult> FetchContentAsync() override;
    virtual FString GetProviderName() const override;

private:
    /** Multi-source configuration */
    FMultiSourceConfig Config;

    /** Array of individual providers (one per source) */
    TArray<TUniquePtr<FRemoteContentFeedProvider>> SourceProviders;

    /**
     * Fetch from a single source
     * @param SourceIndex Index of source in SourceProviders array
     * @return Future with fetch result from this source
     */
    TFuture<FContentFeedFetchResult> FetchSingleSourceAsync(int32 SourceIndex);

    /**
     * Merge multiple fetch results into one
     * Combines announcements from all sources, handles deduplication
     *
     * @param Results Array of fetch results (some may be errors)
     * @return Merged result
     */
    FContentFeedFetchResult MergeResults(const TArray<FContentFeedFetchResult> &Results);

    /**
     * Deduplicate announcements by ID
     * If same ID appears multiple times, keep the one with higher priority (lower number)
     *
     * @param Announcements Input array (may have duplicates)
     * @return Deduplicated array
     */
    TArray<FConvaiAnnouncementItem> DeduplicateByID(const TArray<FConvaiAnnouncementItem> &Announcements);

    /**
     * Deduplicate changelogs by ID
     * If same ID appears multiple times, keep the one with newer date
     *
     * @param Changelogs Input array (may have duplicates)
     * @return Deduplicated array
     */
    TArray<FConvaiChangelogItem> DeduplicateChangelogsByID(const TArray<FConvaiChangelogItem> &Changelogs);

    /**
     * Validate configuration
     */
    bool IsConfigValid() const;
};
