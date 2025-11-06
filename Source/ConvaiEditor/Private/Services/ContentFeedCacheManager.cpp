/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ContentFeedCacheManager.cpp
 *
 * Implementation of content feed caching service.
 */

#include "Services/ContentFeedCacheManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Logging/ConvaiEditorConfigLog.h"

FContentFeedCacheManager::FContentFeedCacheManager(const FConfig &InConfig)
	: Config(InConfig), CacheTimestamp(FDateTime::MinValue())
{
	if (Config.bEnableDiskCache)
	{
		FScopeLock Lock(&CacheMutex);

		// Load appropriate cache type based on configuration
		if (Config.ContentType == EContentFeedCacheType::Announcements)
		{
			AnnouncementMemoryCache = LoadFromDisk();
		}
		else if (Config.ContentType == EContentFeedCacheType::Changelogs)
		{
			ChangelogMemoryCache = LoadChangelogsFromDisk();
		}
	}
}

FContentFeedCacheManager::~FContentFeedCacheManager()
{
	FScopeLock Lock(&CacheMutex);

	if (Config.bEnableDiskCache)
	{
		if (AnnouncementMemoryCache.IsSet())
		{
			SaveToDisk(AnnouncementMemoryCache.GetValue());
		}
		else if (ChangelogMemoryCache.IsSet())
		{
			SaveToDisk(ChangelogMemoryCache.GetValue());
		}
	}
}

TOptional<FConvaiAnnouncementFeed> FContentFeedCacheManager::GetCached()
{
	FScopeLock Lock(&CacheMutex);

	if (AnnouncementMemoryCache.IsSet() && IsCacheFresh())
	{
		return AnnouncementMemoryCache;
	}

	if (Config.bEnableDiskCache)
	{
		AnnouncementMemoryCache = LoadFromDisk();

		if (AnnouncementMemoryCache.IsSet() && IsCacheFresh())
		{
			return AnnouncementMemoryCache;
		}
	}

	return TOptional<FConvaiAnnouncementFeed>();
}

TOptional<FConvaiChangelogFeed> FContentFeedCacheManager::GetCachedChangelogs()
{
	FScopeLock Lock(&CacheMutex);

	if (ChangelogMemoryCache.IsSet() && IsCacheFresh())
	{
		return ChangelogMemoryCache;
	}

	if (Config.bEnableDiskCache)
	{
		ChangelogMemoryCache = LoadChangelogsFromDisk();

		if (ChangelogMemoryCache.IsSet() && IsCacheFresh())
		{
			return ChangelogMemoryCache;
		}
	}

	return TOptional<FConvaiChangelogFeed>();
}

bool FContentFeedCacheManager::SaveToCache(const FConvaiAnnouncementFeed &Feed)
{
	if (!Feed.IsValid())
	{
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Attempted to cache invalid announcement feed"));
		return false;
	}

	FScopeLock Lock(&CacheMutex);

	AnnouncementMemoryCache = Feed;
	CacheTimestamp = FDateTime::UtcNow();

	if (Config.bEnableDiskCache)
	{
		return SaveToDisk(Feed);
	}

	return true;
}

bool FContentFeedCacheManager::SaveToCache(const FConvaiChangelogFeed &Feed)
{
	if (!Feed.IsValid())
	{
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Attempted to cache invalid changelog feed"));
		return false;
	}

	FScopeLock Lock(&CacheMutex);

	ChangelogMemoryCache = Feed;
	CacheTimestamp = FDateTime::UtcNow();

	if (Config.bEnableDiskCache)
	{
		return SaveToDisk(Feed);
	}

	return true;
}

void FContentFeedCacheManager::InvalidateCache()
{
	FScopeLock Lock(&CacheMutex);

	AnnouncementMemoryCache.Reset();
	ChangelogMemoryCache.Reset();
	CacheTimestamp = FDateTime::MinValue();

	if (Config.bEnableDiskCache)
	{
		FString CachePath = GetCacheFilePath();
		IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		if (PlatformFile.FileExists(*CachePath))
		{
			if (PlatformFile.DeleteFile(*CachePath))
			{
			}
			else
			{
				UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Failed to delete cache file"));
			}
		}
	}
}

bool FContentFeedCacheManager::IsCacheValid() const
{
	FScopeLock Lock(&CacheMutex);
	return (AnnouncementMemoryCache.IsSet() || ChangelogMemoryCache.IsSet()) && IsCacheFresh();
}

double FContentFeedCacheManager::GetCacheAge() const
{
	FScopeLock Lock(&CacheMutex);

	if (CacheTimestamp == FDateTime::MinValue())
	{
		return -1.0;
	}

	return (FDateTime::UtcNow() - CacheTimestamp).GetTotalSeconds();
}

FString FContentFeedCacheManager::GetCacheFilePath() const
{
	return FPaths::Combine(GetCacheDirectory(), Config.CacheFileName);
}

TOptional<FConvaiAnnouncementFeed> FContentFeedCacheManager::LoadFromDisk()
{
	FString CachePath = GetCacheFilePath();

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*CachePath))
	{
		return TOptional<FConvaiAnnouncementFeed>();
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *CachePath))
	{
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Failed to read cache file"));
		return TOptional<FConvaiAnnouncementFeed>();
	}

	FConvaiAnnouncementFeed Feed = FConvaiAnnouncementFeed::FromJsonString(JsonString);

	if (!Feed.IsValid())
	{
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Announcement cache file contains invalid data at: %s"), *CachePath);

		// Diagnostic information
		if (Feed.LastUpdated == FDateTime::MinValue())
		{
			UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Reason: LastUpdated is not set (MinValue)"));
		}

		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Announcement items count: %d"), Feed.Announcements.Num());

		if (Feed.Announcements.Num() > 0)
		{
			int32 ValidCount = 0;
			for (const auto &Item : Feed.Announcements)
			{
				if (Item.IsValid())
				{
					ValidCount++;
				}
			}
			UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Valid items: %d / %d"), ValidCount, Feed.Announcements.Num());

			if (ValidCount == 0 && Feed.Announcements.Num() > 0)
			{
				const auto &FirstItem = Feed.Announcements[0];
				UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  First item validation: ID='%s', Title='%s'"),
					   *FirstItem.ID, *FirstItem.Title);
			}
		}

		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Cache file size: %d bytes"), JsonString.Len());
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  First 200 chars: %s"), *JsonString.Left(200));

		if (PlatformFile.DeleteFile(*CachePath))
		{
			UE_LOG(LogConvaiEditorConfig, Log, TEXT("Successfully deleted corrupt cache file: %s"), *CachePath);
		}
		else
		{
			UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to delete corrupt cache file: %s"), *CachePath);
		}

		return TOptional<FConvaiAnnouncementFeed>();
	}

	CacheTimestamp = Feed.LastUpdated;

	return Feed;
}

TOptional<FConvaiChangelogFeed> FContentFeedCacheManager::LoadChangelogsFromDisk()
{
	FString CachePath = GetCacheFilePath();

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*CachePath))
	{
		return TOptional<FConvaiChangelogFeed>();
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *CachePath))
	{
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Failed to read changelog cache file"));
		return TOptional<FConvaiChangelogFeed>();
	}

	FConvaiChangelogFeed Feed = FConvaiChangelogFeed::FromJsonString(JsonString);

	if (!Feed.IsValid())
	{
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("Changelog cache file contains invalid data at: %s"), *CachePath);

		// Diagnostic information
		if (Feed.LastUpdated == FDateTime::MinValue())
		{
			UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Reason: LastUpdated is not set (MinValue)"));
		}

		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Changelog items count: %d"), Feed.Changelogs.Num());

		if (Feed.Changelogs.Num() > 0)
		{
			int32 ValidCount = 0;
			for (const auto &Item : Feed.Changelogs)
			{
				if (Item.IsValid())
				{
					ValidCount++;
				}
			}
			UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Valid items: %d / %d"), ValidCount, Feed.Changelogs.Num());

			if (ValidCount == 0 && Feed.Changelogs.Num() > 0)
			{
				const auto &FirstItem = Feed.Changelogs[0];
				UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  First item validation: ID='%s', Version='%s', Changes=%d"),
					   *FirstItem.ID, *FirstItem.Version, FirstItem.Changes.Num());
			}
		}

		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  Cache file size: %d bytes"), JsonString.Len());
		UE_LOG(LogConvaiEditorConfig, Warning, TEXT("  First 200 chars: %s"), *JsonString.Left(200));

		if (PlatformFile.DeleteFile(*CachePath))
		{
			UE_LOG(LogConvaiEditorConfig, Log, TEXT("Successfully deleted corrupt changelog cache file: %s"), *CachePath);
		}
		else
		{
			UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to delete corrupt changelog cache file: %s"), *CachePath);
		}

		return TOptional<FConvaiChangelogFeed>();
	}

	CacheTimestamp = Feed.LastUpdated;

	return Feed;
}

bool FContentFeedCacheManager::SaveToDisk(const FConvaiAnnouncementFeed &Feed)
{
	FString CachePath = GetCacheFilePath();
	FString CacheDir = GetCacheDirectory();

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*CacheDir))
	{
		if (!PlatformFile.CreateDirectoryTree(*CacheDir))
		{
			UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to create cache directory"));
			return false;
		}
	}

	FString JsonString = Feed.ToJsonString(true);

	if (!FFileHelper::SaveStringToFile(JsonString, *CachePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to write cache file"));
		return false;
	}

	return true;
}

bool FContentFeedCacheManager::SaveToDisk(const FConvaiChangelogFeed &Feed)
{
	FString CachePath = GetCacheFilePath();
	FString CacheDir = GetCacheDirectory();

	IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*CacheDir))
	{
		if (!PlatformFile.CreateDirectoryTree(*CacheDir))
		{
			UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to create cache directory"));
			return false;
		}
	}

	FString JsonString = Feed.ToJsonString(true);

	if (!FFileHelper::SaveStringToFile(JsonString, *CachePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogConvaiEditorConfig, Error, TEXT("Failed to write changelog cache file"));
		return false;
	}

	return true;
}

bool FContentFeedCacheManager::IsCacheFresh() const
{
	if (CacheTimestamp == FDateTime::MinValue())
	{
		return false;
	}

	double Age = (FDateTime::UtcNow() - CacheTimestamp).GetTotalSeconds();
	return Age <= Config.TTLSeconds;
}

FString FContentFeedCacheManager::GetCacheDirectory() const
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("ConvaiEditor"));
}
