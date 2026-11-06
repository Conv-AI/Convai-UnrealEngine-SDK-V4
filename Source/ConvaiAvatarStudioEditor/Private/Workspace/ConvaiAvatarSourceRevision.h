// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "Workspace/ConvaiAvatarWorkspace.h"

namespace ConvaiAvatarSourceRevision
{
	/** The captures fill their outputs only when they finish. One that fails or is cancelled leaves them as they were: Prepare
	 * captures straight into the record its failure exit saves. */
	bool CaptureSource(const TMap<FName, FName>& Packages, TMap<FString, FString>& Files,
		TMap<FName, FString>& LegacyHashes, const TFunction<bool()>& Cancelled, FString& Error);
	bool CapturePrepared(const FString& PluginDirectory, TMap<FString, FString>& Files,
		const TFunction<bool()>& Cancelled, FString& Error);
	void Compare(const FConvaiAvatarPreparedAsset& Previous, const FConvaiAvatarPreparedAsset& Current,
		const TMap<FString, FString>& PreparedFiles, FConvaiAvatarSourceReview& Review);
	/** The prepared-file baseline after Cloud Avatars wrote files: a file it changed takes its new hash when it matched the
	 * baseline before the write, or was in neither. A file the creator had edited keeps its old entry, so it still shows as
	 * edited, and an empty legacy baseline stays empty. */
	TMap<FString, FString> AdvancePreparedBaseline(const TMap<FString, FString>& Baseline,
		const TMap<FString, FString>& BeforeWrites, const TMap<FString, FString>& Now);

	/** Game thread. While the outermost scope is open, the captures reuse a file's hash as long as its normalized path, size and
	 * last-write time all still match, so a file saved between two reads is hashed again. It starts empty and is cleared when
	 * that scope closes: nothing carries over to another operation. A nested scope changes nothing. */
	class FHashMemoScope
	{
	public:
		FHashMemoScope();
		~FHashMemoScope();
		FHashMemoScope(const FHashMemoScope&) = delete;
		FHashMemoScope& operator=(const FHashMemoScope&) = delete;
	private:
		bool bOutermost = false;
	};

#if WITH_DEV_AUTOMATION_TESTS
	/** Test taps: the files, and their bytes, the captures have read to hash. */
	extern int64 HashedFiles;
	extern int64 HashedBytes;
#endif
}
