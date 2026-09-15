/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiCharacterMetadata.h
 *
 * Data model for Convai character metadata.
 */

#pragma once

#include "CoreMinimal.h"

/** Data model for Convai character metadata. */
struct FConvaiCharacterMetadata
{
    FString CharacterID;
    FString CharacterName;
    bool bIsNarrativeDriven = false;
    bool bIsLongTermMemoryEnabled = false;

    FConvaiCharacterMetadata() = default;
    FConvaiCharacterMetadata(const FString &InCharacterID, const FString &InCharacterName, bool bInNarrativeDriven, bool bInLongTermMemoryEnabled)
        : CharacterID(InCharacterID), CharacterName(InCharacterName), bIsNarrativeDriven(bInNarrativeDriven), bIsLongTermMemoryEnabled(bInLongTermMemoryEnabled)
    {
    }
};