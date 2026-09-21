// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Zips a staged build directory. Blocking; call from a worker thread. */
class FConvaiProjectArchive
{
public:
	/** Fraction of total bytes written so far. */
	using FProgress = TFunction<void(float)>;

	/**
	 * Writes every file under SourceDirectory into OutputZip, keeping relative paths.
	 * Entries are stored deflated. Returns false with OutError set on any failure,
	 * including cancellation, and removes the partial archive.
	 */
	static bool ZipDirectory(const FString& SourceDirectory, const FString& OutputZip,
		const TSharedPtr<TAtomic<bool>>& CancelFlag, FProgress Progress, FString& OutError);

	/**
	 * Largest single file this writer accepts. FZipArchiveWriter takes each entry as one
	 * contiguous buffer, so an entry has to fit in memory and in a 32-bit array index.
	 */
	static constexpr int64 MaxEntryBytes = 1536ll * 1024 * 1024;
};
