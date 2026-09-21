// Copyright 2025 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

namespace ConvaiAvatarStudio::BlueprintSetup
{
/**
 * Legacy import hint when no saved MetaHuman choice exists. Creation/setup uses the user's
 * explicit choice, not this heuristic. The hint checks for a body mesh on a MetaHuman skeleton.
 */
CONVAIAVATARSTUDIOEDITOR_API bool IsMetaHuman(const UBlueprint* Blueprint);

/** SDK assets that setup would introduce. Reads the selected Blueprint without changing it. */
CONVAIAVATARSTUDIOEDITOR_API bool GetSetupDependencies(UBlueprint* Blueprint, bool bIsMetaHuman, TArray<FName>& OutPackages, FString& OutError);

/**
 * Brings an Avatar's Entry Point blueprint up to what a published Avatar needs, in place.
 *
 * Adds BP_ConvaiChatbotComponent only when that wrapper (or a subclass) is absent, including
 * inherited components. A raw-only C++ chatbot refuses before any edit with migration guidance.
 * Adds FaceSync only when absent. The explicit MetaHuman choice enables missing/default animation
 * setup while preserving custom animation Blueprints. The Blueprint parent is never changed.
 *
 * Compiles once at the end when something changed, and never saves: the caller owns the package,
 * and tests run this against blueprints in the transient package that cannot be saved at all.
 *
 * @param OutChanges  One line per edit, empty when the blueprint already satisfied every rule.
 * @return false with OutError set when the blueprint must be fixed by hand.
 */
CONVAIAVATARSTUDIOEDITOR_API bool PrepareAvatarBlueprint(UBlueprint* Blueprint, bool bIsMetaHuman, FString& OutError, TArray<FString>& OutChanges,
	const TMap<FName, FName>* CopiedPackages = nullptr);
}
