// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ConvaiVisionService.h"

/**
 * Request Snapshots: per-call debug folders under
 * Saved/ConvaiSceneAutoTagger/Requests/ pairing the exact images handed to the
 * native core with the inputs that accompanied them (request.json) and the
 * outcome that came back (response.json). The composed prompt is not part of a
 * snapshot on the protected paths — the core owns the template; the manifest
 * records the caller's inputs plus the core's vision protocol id.
 *
 * Callers gate on UConvaiSceneAutoTaggerSettings::bSaveDebugCaptures; these
 * functions always write. Credentials never reach disk: the autotag saver
 * accepts the auth header name only, and SaveVisionRequest drops the pair's
 * value. Write failures are logged and never fail the request.
 */
namespace ConvaiVisionRequestSnapshot
{
	/** Writes the sheet PNG and request.json; returns the snapshot folder, or empty when it could not be created. */
	CONVAISCENETAGGING_API FString SaveAutotagRequest(
		const TCHAR* PathKind,
		const FString& AuthHeaderName,
		const FString& FileName,
		const TArray<uint8>& PngBytes,
		const TArray<FConvaiVisionService::FCellMeta>& Cells,
		const FConvaiTaggingPipelineParams& Params);

	/** Writes every image PNG and request.json; returns the snapshot folder, or empty when it could not be created. */
	CONVAISCENETAGGING_API FString SaveVisionRequest(const FConvaiVisionService::FVisionRequest& Request);

	/** Writes response.json into an existing snapshot folder; no-op when SnapshotDir is empty. */
	CONVAISCENETAGGING_API void SaveResult(const FString& SnapshotDir, const FConvaiVisionService::FResult& Result);

	/** Writes response.json into an existing snapshot folder; no-op when SnapshotDir is empty. */
	CONVAISCENETAGGING_API void SaveRawResult(const FString& SnapshotDir, const FConvaiVisionService::FRawResult& Result);
}
