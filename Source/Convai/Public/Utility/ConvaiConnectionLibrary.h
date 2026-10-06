// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Core/ConvaiConnectionManager.h"
#include "ConvaiConnectionLibrary.generated.h"

UCLASS()
class CONVAI_API UConvaiConnectionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Start a "warm" Convai session for CharacterID with no owner so a later
	 * Acquire of the same character reuses it without a fresh handshake.
	 *
	 * @param PrepTTLOverrideSeconds  Optional override for the warm window length.
	 *                                Pass <= 0 to use the project default
	 *                                (UConvaiUtils::GetPrepConnectionTTL()).
	 *                                Any positive value is clamped to [1, 600].
	 * @return                        Outcome of the request; maps directly to the
	 *                                HTTP status code for POST /connection/prep.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Connection",
			  meta = (WorldContext = "WorldContextObject"))
	static EC_PrepResult PrepareCharacterConnection(
		UObject* WorldContextObject,
		const FString& CharacterID,
		float PrepTTLOverrideSeconds = 0.0f);
};
