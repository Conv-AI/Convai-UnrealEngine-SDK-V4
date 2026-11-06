// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerVisionProtocol.h"

namespace
{
constexpr TCHAR PromptVersion[] = TEXT("vision-autotag-abi3-cells");
constexpr int32 MaxNameLength = 64;
constexpr int32 MaxSceneContextLength = 800;
constexpr int32 MaxDescriptionFocusLength = 400;
constexpr int32 MaxRefinementNoteLength = 300;

FString NormalizeBoundedUserText(const FString& Value, const int32 MaximumLength)
{
	FString Normalized;
	Normalized.Reserve(FMath::Min(Value.Len(), MaximumLength));
	bool bPreviousWasWhitespace = true;
	for (int32 CharacterIndex = 0; CharacterIndex < Value.Len(); ++CharacterIndex)
	{
		const TCHAR Character = Value[CharacterIndex];
		const bool bUnsafeControl = (Character < 0x20 && !FChar::IsWhitespace(Character))
			|| (Character >= 0x7F && Character <= 0x9F);
		const bool bUnsafeDirectionality = Character == 0x061C
			|| Character == 0x200E || Character == 0x200F
			|| (Character >= 0x202A && Character <= 0x202E)
			|| (Character >= 0x2066 && Character <= 0x2069);
		if (bUnsafeControl || bUnsafeDirectionality)
		{
			continue;
		}
		if (FChar::IsWhitespace(Character))
		{
			if (!bPreviousWasWhitespace && Normalized.Len() < MaximumLength)
			{
				Normalized.AppendChar(TEXT(' '));
			}
			bPreviousWasWhitespace = true;
			continue;
		}

		const bool bHighSurrogate = Character >= 0xD800 && Character <= 0xDBFF;
		const bool bLowSurrogate = Character >= 0xDC00 && Character <= 0xDFFF;
		if (bHighSurrogate)
		{
			const bool bHasLowSurrogate = CharacterIndex + 1 < Value.Len()
				&& Value[CharacterIndex + 1] >= 0xDC00
				&& Value[CharacterIndex + 1] <= 0xDFFF;
			if (!bHasLowSurrogate || Normalized.Len() + 2 > MaximumLength)
			{
				continue;
			}
			Normalized.AppendChar(Character);
			Normalized.AppendChar(Value[++CharacterIndex]);
			bPreviousWasWhitespace = false;
			continue;
		}
		if (bLowSurrogate)
		{
			continue;
		}
		if (Normalized.Len() >= MaximumLength)
		{
			break;
		}
		Normalized.AppendChar(Character);
		bPreviousWasWhitespace = false;
	}
	Normalized.TrimStartAndEndInline();
	return Normalized;
}

bool IsGeneratedWordCharacter(const TCHAR Character)
{
	return FChar::IsAlnum(Character) || Character == TEXT('_');
}

TArray<FString> TokenizeGeneratedText(const FString& Value, const bool bLowercase = true)
{
	TArray<FString> Tokens;
	FString Token;
	for (const TCHAR SourceCharacter : Value)
	{
		const TCHAR Character = bLowercase ? FChar::ToLower(SourceCharacter) : SourceCharacter;
		if (IsGeneratedWordCharacter(Character))
		{
			Token.AppendChar(Character);
		}
		else if (!Token.IsEmpty())
		{
			Tokens.Add(MoveTemp(Token));
			Token.Reset();
		}
	}
	if (!Token.IsEmpty())
	{
		Tokens.Add(MoveTemp(Token));
	}
	return Tokens;
}

}

const TCHAR* ConvaiSceneAutoTagger::VisionProtocol::GetPromptVersion()
{
	return PromptVersion;
}

int32 ConvaiSceneAutoTagger::VisionProtocol::GetMaximumSceneContextLength()
{
	return MaxSceneContextLength;
}

FString ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(const FString& SceneContext)
{
	return NormalizeBoundedUserText(SceneContext, MaxSceneContextLength);
}

int32 ConvaiSceneAutoTagger::VisionProtocol::GetMaximumDescriptionFocusLength()
{
	return MaxDescriptionFocusLength;
}

FString ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(
	const FString& DescriptionFocus)
{
	return NormalizeBoundedUserText(DescriptionFocus, MaxDescriptionFocusLength);
}

int32 ConvaiSceneAutoTagger::VisionProtocol::GetMaximumRefinementNoteLength()
{
	return MaxRefinementNoteLength;
}

FString ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
	const FString& RefinementNote)
{
	return NormalizeBoundedUserText(RefinementNote, MaxRefinementNoteLength);
}

bool ConvaiSceneAutoTagger::VisionProtocol::HasMaterialProposalChange(
	const FSceneAutoTaggerObjectPromptContext& SubmittedContext,
	const FSceneAutoTaggerVisionItem& Result)
{
	const auto NormalizeProposalText = [](const FString& Value)
	{
		return FString::Join(TokenizeGeneratedText(Value), TEXT(" "));
	};
	return NormalizeProposalText(SubmittedContext.PreviousName)
		!= NormalizeProposalText(Result.Name)
		|| NormalizeProposalText(SubmittedContext.PreviousDescription)
			!= NormalizeProposalText(Result.Description);
}
