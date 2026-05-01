// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"
#include "ConvaiEnvironment.generated.h"

/**
 * Action affordance contract sent to the server at /connect time as `action_config`.
 * Designers populate Actions/Objects/Characters in the chatbot's Details panel; runtime
 * mutation goes through the Add/Remove/Clear methods on UConvaiChatbotComponent so the
 * local mirror stays in sync with what the WebRTC pipeline pushes to the server.
 *
 * Sent only when the owning chatbot has bEnableActions=true.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiEnvironmentData
{
	GENERATED_BODY()

	/** Allowed physical actions, each with optional description and typed parameters.
	 *  Defaults cover the common "go somewhere / follow / stop / wait" beats; designers
	 *  can prune, extend, or reword descriptions per character in the Details panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Action API")
	TArray<FConvaiAction> Actions = {
		FConvaiAction(TEXT("Move To"),     TEXT(""),
			{ FConvaiActionParam(TEXT("destination"), TEXT(""), EConvaiActionParamType::Reference) }),
		FConvaiAction(TEXT("Follow"),      TEXT("Follow a character"),
			{ FConvaiActionParam(TEXT("character"),   TEXT(""),   EConvaiActionParamType::Reference) }),
		FConvaiAction(TEXT("Stop Moving"), TEXT(""), {}),
		FConvaiAction(TEXT("Wait For"),    TEXT(""),
			{ FConvaiActionParam(TEXT("time in seconds"), TEXT(""), EConvaiActionParamType::Number) })
	};

	/** Allowed action targets that are objects in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Action API")
	TArray<FConvaiObjectEntry> Objects;

	/** Allowed action targets that are characters / NPCs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Action API")
	TArray<FConvaiObjectEntry> Characters;

	/** Current focus object the bot should treat as the antecedent for "this"/"that"/"it"/"there".
	 *  Server resolves this against action_config.objects only — see SetObjectInAttention on the chatbot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Action API")
	FConvaiObjectEntry CurrentAttentionObject;

	/** Returns the JSON serialization sent as the top-level `action_config` field on /connect.
	 *  Returns an empty string if the environment has no Actions configured. */
	FString ToActionConfigJson() const;

	const FConvaiObjectEntry* FindObject(const FString& ObjectName) const;
	const FConvaiObjectEntry* FindCharacter(const FString& CharacterName) const;
};
