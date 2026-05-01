// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiDefinitions.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiUtils.h"
#include "Internationalization/Regex.h"

DEFINE_LOG_CATEGORY(ConvaiDefinitionsLog);

namespace
{
	// Render the lowercase wire word for a declared type. Only Auto elides — every
	// other type emits a hint so the wire format round-trips cleanly. (If Reference
	// elided, parse-back would demote it to Auto since there's no hint to recognize.)
	const TCHAR* TypeWireWord(EConvaiActionParamType T)
	{
		switch (T)
		{
		case EConvaiActionParamType::Reference: return TEXT("ref");
		case EConvaiActionParamType::String:    return TEXT("string");
		case EConvaiActionParamType::Number:    return TEXT("number");
		case EConvaiActionParamType::Bool:      return TEXT("bool");
		case EConvaiActionParamType::Enum:      return TEXT("enum");
		default:                                return nullptr; // Auto
		}
	}

	EConvaiActionParamType ParseTypeWireWord(const FString& Word)
	{
		const FString Lower = Word.ToLower();
		if (Lower == TEXT("string"))    return EConvaiActionParamType::String;
		if (Lower == TEXT("number"))    return EConvaiActionParamType::Number;
		if (Lower == TEXT("bool"))      return EConvaiActionParamType::Bool;
		if (Lower == TEXT("enum"))      return EConvaiActionParamType::Enum;
		if (Lower == TEXT("ref") ||
			Lower == TEXT("reference")) return EConvaiActionParamType::Reference;
		return EConvaiActionParamType::Auto;
	}

	// Pull the LLM-facing display labels out of a UEnum, skipping _MAX and Hidden values.
	TArray<FString> EnumChoiceLabels(const UEnum* E)
	{
		TArray<FString> Out;
		if (!E) return Out;
		const int32 Max = E->NumEnums();
		for (int32 i = 0; i < Max; ++i)
		{
			// Skip the auto-generated _MAX sentinel UE adds at the end.
			if (i == Max - 1 && E->ContainsExistingMax())
			{
				continue;
			}
#if WITH_EDITORONLY_DATA
			// UEnum::HasMetaData is editor-only (gated by WITH_EDITORONLY_DATA on 5.0-5.4
			// and WITH_METADATA on 5.5+, both of which compile out in cooked/runtime builds).
			if (E->HasMetaData(TEXT("Hidden"), i))
			{
				continue;
			}
#endif
			FString Label = E->GetDisplayNameTextByIndex(i).ToString();
			if (Label.IsEmpty())
			{
				Label = E->GetNameStringByIndex(i);
			}
			Out.Add(Label);
		}
		return Out;
	}
}

FString FConvaiAction::ToActionConfigString() const
{
	// 1. Name
	FString Out = Name;

	// 2. Param placeholders.
	for (const FConvaiActionParam& P : Parameters)
	{
		if (!P.Connector.IsEmpty())
		{
			Out += FString::Printf(TEXT(" %s"), *P.Connector);
		}

		FString Hint;
		if (P.Type == EConvaiActionParamType::Enum)
		{
			if (P.EnumType)
			{
				const TArray<FString> Labels = EnumChoiceLabels(P.EnumType);
				if (Labels.Num() > 0)
				{
					Hint += FString::Printf(TEXT(" [%s]"), *FString::Join(Labels, TEXT("|")));
				}
			}
			else
			{
				Hint += TEXT(" [ERROR: EnumType not set]");
			}
		}
		else if (P.Choices.Num() > 0)
		{
			Hint += FString::Printf(TEXT(" [%s]"), *FString::Join(P.Choices, TEXT("|")));
		}

		if (const TCHAR* TypeWord = TypeWireWord(P.Type))
		{
			Hint += FString::Printf(TEXT(": %s"), TypeWord);
		}

		Out += FString::Printf(TEXT(" {%s%s}"), *P.Name, *Hint);
	}

	// 3. Conditional description tail — build only out of non-empty bits.
	const bool bHasActionDesc = !Description.IsEmpty();

	TArray<FString> ParamSentences;
	for (const FConvaiActionParam& P : Parameters)
	{
		if (!P.Description.IsEmpty())
		{
			ParamSentences.Add(FString::Printf(TEXT("%s: %s"), *P.Name, *P.Description));
		}
	}

	if (bHasActionDesc || ParamSentences.Num() > 0)
	{
		Out += TEXT(" — ");
		if (bHasActionDesc)
		{
			Out += Description;
			if (ParamSentences.Num() > 0)
			{
				Out += TEXT(". ");
			}
		}
		Out += FString::Join(ParamSentences, TEXT(". "));
		Out += TEXT(".");
	}

	return Out;
}

bool FConvaiAction::ParseFromActionConfigString(const FString& Source, FConvaiAction& Out)
{
	const FString Trimmed = Source.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return false;
	}

	// Split into [head] and [tail] on the first " — " (em-dash). Tail may be absent.
	FString Head, Tail;
	const FString Separator = TEXT(" — ");
	int32 SepIdx = INDEX_NONE;
	if (Trimmed.FindChar(TEXT('—'), SepIdx))
	{
		// Step back one char (the leading space) and forward two (space + em-dash + space).
		Head = Trimmed.Left(SepIdx).TrimEnd();
		Tail = Trimmed.RightChop(SepIdx + 1).TrimStart();
	}
	else
	{
		Head = Trimmed;
	}

	// Find every {...} placeholder in the head. We use the offsets to slice out the
	// connector text immediately preceding each placeholder (or — for the first one —
	// to identify the action name). Invariant: '}' must not appear inside choice values
	// (they're joined with '|') or inside the type hint, so a non-greedy [^}]* is safe.
	struct FPlaceholderHit { int32 OpenIdx; int32 CloseIdx; FString Inner; };
	TArray<FPlaceholderHit> Hits;
	{
		const FRegexPattern Pattern(TEXT("\\{([^}]*)\\}"));
		FRegexMatcher Matcher(Pattern, Head);
		while (Matcher.FindNext())
		{
			FPlaceholderHit H;
			H.OpenIdx  = Matcher.GetMatchBeginning();
			H.CloseIdx = Matcher.GetMatchEnding();
			H.Inner    = Matcher.GetCaptureGroup(1);
			Hits.Add(H);
		}
	}

	FConvaiAction Built;
	if (Hits.Num() == 0)
	{
		// No placeholders — head is just the action name.
		Built.Name = Head.TrimStartAndEnd();
		if (Built.Name.IsEmpty())
		{
			return false;
		}
	}
	else
	{
		// Action name = everything before the first placeholder. The space before the
		// placeholder belongs to the separator, so trim trailing whitespace.
		Built.Name = Head.Left(Hits[0].OpenIdx).TrimStartAndEnd();
		if (Built.Name.IsEmpty())
		{
			return false;
		}

		// Each placeholder gets parsed into a FConvaiActionParam. Connector text is
		// whatever sits between the previous placeholder (or the action name for the
		// first param) and this one's opening quote.
		int32 PrevEnd = Hits[0].OpenIdx + Built.Name.Len();
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			const FPlaceholderHit& H = Hits[i];

			FConvaiActionParam P;

			// Connector — text between PrevEnd and the placeholder open. The first
			// placeholder uses the gap after Name; subsequent ones, after the prior close.
			const int32 ConnectorStart = (i == 0) ? Built.Name.Len() : Hits[i - 1].CloseIdx;
			const int32 ConnectorLen   = H.OpenIdx - ConnectorStart;
			if (ConnectorLen > 0)
			{
				P.Connector = Head.Mid(ConnectorStart, ConnectorLen).TrimStartAndEnd();
			}

			// Inner: <name [choices]: type>
			FString Inner = H.Inner;

			// Pull off type hint after ": " (last colon, in case names contain colons).
			int32 ColonIdx = INDEX_NONE;
			Inner.FindLastChar(TEXT(':'), ColonIdx);
			if (ColonIdx != INDEX_NONE)
			{
				P.Type = ParseTypeWireWord(Inner.RightChop(ColonIdx + 1).TrimStartAndEnd());
				Inner = Inner.Left(ColonIdx).TrimEnd();
			}
			else
			{
				P.Type = EConvaiActionParamType::Auto;
			}

			// Pull off choices in [..|..|..].
			{
				const FRegexPattern ChoicePattern(TEXT("\\[([^\\]]+)\\]"));
				FRegexMatcher M(ChoicePattern, Inner);
				if (M.FindNext())
				{
					const FString ChoicesBlob = M.GetCaptureGroup(1);
					ChoicesBlob.ParseIntoArray(P.Choices, TEXT("|"), true);
					for (FString& C : P.Choices) { C.TrimStartAndEndInline(); }
					Inner = (Inner.Left(M.GetMatchBeginning()) + Inner.RightChop(M.GetMatchEnding())).TrimEnd();
				}
			}

			P.Name = Inner.TrimStartAndEnd();
			if (P.Name.IsEmpty())
			{
				return false;
			}
			Built.Parameters.Add(MoveTemp(P));
		}
	}

	// Tail: split sentences, route by `<paramName>: ...` prefix into per-param descriptions
	// or — when no name match — accumulate into the action's Description.
	if (!Tail.IsEmpty())
	{
		// Strip the trailing period if present.
		FString Body = Tail;
		Body.RemoveFromEnd(TEXT("."));

		TArray<FString> Sentences;
		Body.ParseIntoArray(Sentences, TEXT(". "), true);

		TArray<FString> ActionDescChunks;
		for (FString& S : Sentences)
		{
			S.TrimStartAndEndInline();
			if (S.IsEmpty()) continue;

			int32 ColonIdx = INDEX_NONE;
			if (S.FindChar(TEXT(':'), ColonIdx))
			{
				const FString Lhs = S.Left(ColonIdx).TrimStartAndEnd();
				const FString Rhs = S.RightChop(ColonIdx + 1).TrimStartAndEnd();
				bool bMatched = false;
				for (FConvaiActionParam& Pm : Built.Parameters)
				{
					if (Pm.Name.Equals(Lhs, ESearchCase::IgnoreCase))
					{
						Pm.Description = Rhs;
						bMatched = true;
						break;
					}
				}
				if (!bMatched)
				{
					ActionDescChunks.Add(S);
				}
			}
			else
			{
				ActionDescChunks.Add(S);
			}
		}

		Built.Description = FString::Join(ActionDescChunks, TEXT(". "));
	}

	Out = MoveTemp(Built);
	return true;
}

const TMap<EEmotionIntensity, float> FConvaiEmotionState::ScoreMultipliers = 
{
	{EEmotionIntensity::None, 0.0},
	{EEmotionIntensity::LessIntense, 0.25},
	{EEmotionIntensity::Basic, 0.6},
	{EEmotionIntensity::MoreIntense, 1}
};

FConvaiConnectionParams FConvaiConnectionParams::Create(convai::ConvaiClient* InClient, const FString& InCharacterID, UConvaiConnectionSessionProxy* SessionProxy)
{
	FConvaiConnectionParams Params;
	Params.Client = InClient;
	Params.CharacterID = InCharacterID;
	Params.LLMProvider = UConvaiUtils::GetLLMProvider();
	
	// Get interface once and reuse it
	IConvaiConnectionInterface* Interface = nullptr;
	if (SessionProxy)
	{
		if (const TScriptInterface<IConvaiConnectionInterface> InterfaceScriptInterface = SessionProxy->GetConnectionInterface(); InterfaceScriptInterface.GetObject())
		{
			Interface = InterfaceScriptInterface.GetInterface();
		}
	}
	
	// Determine connection type
	Params.ConnectionType = UConvaiUtils::GetConnectionType();
	if (UConvaiUtils::IsAlwaysAllowVisionEnabled())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Always allow vision is enabled, using video connection type for character ID: %s"), *InCharacterID);
	}
	else if (Interface && Interface->IsVisionSupported())
	{
		Params.ConnectionType = TEXT("video");
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Vision is supported by proxy, using video connection type for character ID: %s"), *InCharacterID);
	}
	
	// Determine blendshape provider and format based on lip sync mode
	Params.BlendshapeProvider = TEXT("not_provided");
	Params.BlendshapeFormat = TEXT("");

	// Check if the lip sync component requires precomputed face data from the server
	// If not (e.g., OVR LipSync generates data locally from audio), skip setting blendshape provider

	if (const bool bRequiresPrecomputedFaceData = Interface ? Interface->RequiresPrecomputedFaceData() : true)
	{
		// Helper lambda to set provider and format for a specific mode
		auto SetBlendshapeParamsForMode = [&Params](const EC_LipSyncMode Mode)
		{
			switch (Mode)
			{
			case EC_LipSyncMode::VisemeBased:
				Params.BlendshapeProvider = TEXT("ovr");
				Params.BlendshapeFormat = TEXT("");
				break;
			case EC_LipSyncMode::BS_MHA:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("mha");
				break;
			case EC_LipSyncMode::BS_ARKit:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("arkit");
				break;
			case EC_LipSyncMode::BS_CC4_Extended:
				Params.BlendshapeProvider = TEXT("neurosync");
				Params.BlendshapeFormat = TEXT("cc4_extended");
				break;
			case EC_LipSyncMode::Off:
			default:
				Params.BlendshapeProvider = TEXT("not_provided");
				Params.BlendshapeFormat = TEXT("");
				break;
			}
		};

		switch (const EC_LipSyncMode GlobalMode = UConvaiUtils::GetLipSyncMode())
		{
		case EC_LipSyncMode::Off:
			Params.BlendshapeProvider = TEXT("not_provided");
			break;

		case EC_LipSyncMode::Auto:
			if (Interface)
			{
				SetBlendshapeParamsForMode(Interface->GetLipSyncMode());
			}
			break;

		case EC_LipSyncMode::VisemeBased:
		case EC_LipSyncMode::BS_MHA:
		case EC_LipSyncMode::BS_ARKit:
			SetBlendshapeParamsForMode(GlobalMode);
			break;
		}
	}
	
	// Set emotion provider based on blendshape provider
	Params.EmotionProvider = UConvaiUtils::GetEmotionsProvider();

	// Get End User ID, Metadata, and optional action_config from interface
	if (Interface)
	{
		Params.EndUserID = Interface->GetEndUserID();
		Params.EndUserMetadata = Interface->GetEndUserMetadata();
		Params.ActionConfigJson = Interface->GetActionConfigJson();
	}
	
	// If End User ID is not provided, use device unique identifier as fallback
	if (Params.EndUserID.IsEmpty())
	{
		Params.EndUserID = UConvaiUtils::GetDeviceUniqueIdentifier();
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("End User ID not provided, using Device ID: %s"), *Params.EndUserID);
	}
	else
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Using End User ID: %s"), *Params.EndUserID);
	}
	
	if (!Params.EndUserMetadata.IsEmpty())
	{
		CONVAI_LOG(ConvaiDefinitionsLog, Log, TEXT("Using End User Metadata: %s"), *Params.EndUserMetadata);
	}

	Params.ChunkSize = UConvaiUtils::GetChunkSize();
	Params.OutputFPS = UConvaiUtils::GetOutputFPS();
	Params.FramesBufferDuration = UConvaiUtils::GetFramesBufferDuration();
	
	return Params;
}