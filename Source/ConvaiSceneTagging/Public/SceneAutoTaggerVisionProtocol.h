// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
struct FSceneAutoTaggerVisionItem
{
	int32 Index = INDEX_NONE;
	FString EchoId;
	FString Name;
	FString Description;
	float Confidence = 0.0f;
};

struct FSceneAutoTaggerVisionResponse
{
	TArray<FSceneAutoTaggerVisionItem> Items;
};

/** Per-cell, review-local guidance. The previous proposal is a draft to revise, never visual evidence. */
struct FSceneAutoTaggerObjectPromptContext
{
	FString EchoId;
	FString RefinementNote;
	FString PreviousName;
	FString PreviousDescription;
	bool bIncludePreviousProposal = false;
	bool bRefinementRetry = false;
	bool bContextOnly = false;
};

namespace ConvaiSceneAutoTagger::VisionProtocol
{
/**
 * Client protocol identity folded into the analysis cache key. The native core
 * DLL owns the prompt (its vision protocol id is tracked separately), so this
 * only tracks the client-side request shape.
 */
CONVAISCENETAGGING_API const TCHAR* GetPromptVersion();

CONVAISCENETAGGING_API int32 GetMaximumSceneContextLength();
CONVAISCENETAGGING_API FString NormalizeSceneContext(const FString& SceneContext);
CONVAISCENETAGGING_API int32 GetMaximumDescriptionFocusLength();
CONVAISCENETAGGING_API FString NormalizeDescriptionFocus(const FString& DescriptionFocus);
CONVAISCENETAGGING_API int32 GetMaximumRefinementNoteLength();
CONVAISCENETAGGING_API FString NormalizeRefinementNote(const FString& RefinementNote);

/**
 * True when a guided result changed the editable proposal by more than case,
 * punctuation, or whitespace. Confidence alone never consumes a one-shot note.
 */
CONVAISCENETAGGING_API bool HasMaterialProposalChange(
	const FSceneAutoTaggerObjectPromptContext& SubmittedContext,
	const FSceneAutoTaggerVisionItem& Result);
}
