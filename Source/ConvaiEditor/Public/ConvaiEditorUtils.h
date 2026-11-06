/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiEditorUtils.h
 *
 * Utility functions for Convai Editor operations.
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ConvaiDefinitions.h"

#include "ConvaiEditorUtils.generated.h"

/** Utility functions for Convai Editor operations. */
UCLASS()
class CONVAIEDITOR_API UConvaiEditorUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Adds a speaker ID to the Convai system */
	UFUNCTION(BlueprintCallable, Category = "Convai|LTM")
	static void ConvaiAddSpeakerID(const FConvaiSpeakerInfo &Speaker);

	/** Removes a speaker ID from the Convai system */
	UFUNCTION(BlueprintCallable, Category = "Convai|LTM")
	static void ConvaiRemoveSpeakerID(const FString &SpeakerID);

	/** Refreshes Convai settings */
	static void RefreshConvaiSettings();

	/** Begins a transaction and retrieves currently selected assets */
	UFUNCTION(BlueprintCallable, Category = "Convai|Editor")
	static TArray<UObject *> BeginTransactionAndGetSelectedAssets(const FString &Context, const FText &Description);

	/** Saves loaded assets and ends the current transaction */
	UFUNCTION(BlueprintCallable, Category = "Convai|Editor")
	static void SaveLoadedAssetAndEndTransaction(const TArray<UObject *> &LoadedAssets);
};
