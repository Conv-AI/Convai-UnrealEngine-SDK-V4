// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Caller-owned knobs for one tagging pipeline run. The native core DLL owns
 * the endpoint route, prompt templates, and request encoding; retry, backoff,
 * and parallelism stay on this side of the boundary because the DLL call is
 * blocking and single-shot by design.
 */
struct FConvaiTaggingPipelineParams
{
	FString CharacterID;
	FString SceneDescription;
	FString DescriptionFocus;
	bool bFilterInsignificant = true;
	int32 MaxParallelRequests = 1;
	int32 MaxRetriesOn429 = 3;
	float RetryBackoffSeconds = 20.f;
	int32 TimeoutSeconds = 120;
	/** Optional endpoint base URL; a -ConvaiProdURL= process flag still wins inside the DLL. */
	FString BaseUrlOverride;
	/** Optional model name forwarded to the endpoint; empty keeps the server default. */
	FString ModelOverride;
};

/**
 * Vision requests through the native core's ABI v4 surface: the protected
 * prompt paths (Tagging, AssemblyDescription, SceneContextDescription) and
 * the caller-prompt Vision Request (raw response body back, no template
 * applied).
 *
 * Each request runs the blocking DLL call on its own background thread and
 * fires Completion exactly once on the game thread, including for validation
 * failures. HTTP 429 is retried internally per Params before completing.
 *
 * Auth is the header-name/value pair from UConvaiUtils::GetAuthHeaderAndKey();
 * the pair is passed in rather than fetched here so tests can inject
 * credentials. RequestVision takes CONVAI-API-KEY or API-AUTH-TOKEN; the
 * protected-prompt paths take an API key only (the core's autotag contract).
 *
 * Cells are supplied in contact-sheet order: array position N is the visually
 * numbered cell N+1. Every cell requires a non-empty EchoId (the native core
 * rejects the whole request otherwise); pass the real object id with
 * bContextOnly=true for cells that only provide visual context.
 */
class CONVAISCENETAGGING_API FConvaiVisionService
{
public:
	struct FCellMeta
	{
		FString EchoId;
		bool bContextOnly = false;
		FString PreviousProposalName;
		FString PreviousProposalDescription;
		FString RefinementNote;
	};

	struct FObjectResult
	{
		/** 1-based visual cell number within the sheet; 0 when the server omitted it. */
		int32 CellIndex = 0;
		FString EchoId;
		FString Name;
		FString Description;
		float Confidence = 0.0f;
	};

	struct FResult
	{
		bool bSuccess = false;
		/** HTTP status of the last attempt; 0 when no response was received. 429 means retries were exhausted. */
		int32 HttpStatus = 0;
		FString Error;
		TArray<FObjectResult> Objects;
	};

	struct FVisionImage
	{
		TArray<uint8> PngBytes;
		/** Unique basename within the request; the endpoint maps parts by it. */
		FString FileName;
	};

	/**
	 * Everything backend-bound for one Vision Request. The DLL is a pure
	 * transport on this path: Prompt travels verbatim, the protected template
	 * is never applied, and the response body comes back raw.
	 */
	struct FVisionRequest
	{
		FString Prompt;
		/** 1..MaxImagesPerRequest images. */
		TArray<FVisionImage> Images;
		/** Header-name/value pair from UConvaiUtils::GetAuthHeaderAndKey(). */
		TPair<FString, FString> AuthHeaderAndKey;
		/** Required by the vision endpoint. */
		FString CharacterID;
		int32 TimeoutSeconds = 120;
		int32 MaxParallelRequests = 1;
		int32 MaxRetriesOn429 = 3;
		float RetryBackoffSeconds = 20.f;
		/** Optional endpoint base URL; a -ConvaiProdURL= process flag still wins inside the DLL. */
		FString BaseUrlOverride;
		/** Optional model name forwarded to the endpoint; empty keeps the server default. */
		FString ModelOverride;
		/** Forwarded as form fields; pairs with an empty key or value are omitted. A new server parameter is a new pair here, not an SDK change. */
		TArray<TPair<FString, FString>> ExtraParams;
	};

	struct FRawResult
	{
		/** True only for an HTTP 2xx response. */
		bool bSuccess = false;
		/** HTTP status of the last attempt; 0 when no response was received. 429 means retries were exhausted. */
		int32 HttpStatus = 0;
		/** Complete response body, populated whenever a response arrived — including 4xx/5xx. */
		FString RawBody;
		FString Error;
	};

	/** Mirrors the current server-side per-request limit; the DLL itself is uncapped. */
	static constexpr int32 MaxImagesPerRequest = 3;

	/** Posts one numbered contact sheet; one result object per occupied cell. */
	static void RequestTagging(
		const TPair<FString, FString>& AuthHeaderAndKey,
		TArray<uint8>&& SheetPng,
		const FString& FileName,
		TArray<FCellMeta> Cells,
		const FConvaiTaggingPipelineParams& Params,
		TFunction<void(const FResult&)> Completion);

	/** Posts one whole-assembly image; on success the result carries exactly one object. */
	static void RequestAssemblyDescription(
		const TPair<FString, FString>& AuthHeaderAndKey,
		TArray<uint8>&& Png,
		const FString& EchoId,
		const FConvaiTaggingPipelineParams& Params,
		TFunction<void(const FResult&)> Completion);

	/** Posts one visible-environment image; on success the result carries exactly one scene-context object. */
	static void RequestSceneContextDescription(
		const TPair<FString, FString>& AuthHeaderAndKey,
		TArray<uint8>&& Png,
		const FString& EchoId,
		const FConvaiTaggingPipelineParams& Params,
		TFunction<void(const FResult&)> Completion);

	/** Posts a caller-supplied prompt with images; the raw response body is the caller's to parse. */
	static void RequestVision(
		FVisionRequest&& Request,
		TFunction<void(const FRawResult&)> Completion);
};
