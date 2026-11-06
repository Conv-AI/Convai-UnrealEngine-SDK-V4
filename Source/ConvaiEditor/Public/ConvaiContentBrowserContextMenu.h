/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiContentBrowserContextMenu.h
 *
 * Handles Content Browser context menu integration for Convai.
 */

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

struct FToolMenuSection;
class UToolMenu;
class UBlueprint;
class UFloatingPawnMovement;
class UCharacterMovementComponent;

/** Handles Content Browser context menu integration for Convai. */
class FConvaiContentBrowserContextMenu
{
public:
	/** Initialize and register the context menu */
	static void Register();

	/** Unregister the context menu */
	static void Unregister();

private:
	/** Callback to populate the context menu section */
	static void PopulateContextMenu(FToolMenuSection &InSection);

	/** Callback to populate the Convai submenu */
	static void MakeConvaiSubMenu(UToolMenu *Menu);

	/** Execute action for the Convai menu button */
	static void ExecuteConvaiAction();

	/** Creates a render target and saves it as a .uasset file */
	static void CreateAndSaveRenderTarget(const FString &PackagePath);

	/** Register/unregister the Blueprint asset right-click menu extension */
	static void RegisterBlueprintAssetMenu();
	static void UnregisterBlueprintAssetMenu();

	/** Dynamic populator for the Blueprint asset right-click menu */
	static void PopulateBlueprintAssetMenu(FToolMenuSection &InSection);

	/** Returns true if the asset is a Blueprint whose native parent class derives from AActor */
	static bool IsActorBlueprintAsset(const FAssetData &Asset);

	/** Apply the Convai pawn-movement setup to a single Blueprint */
	static void ApplyConvaiMovementToBlueprint(UBlueprint *Blueprint);

	/**
	 * Find or create a FloatingPawnMovement template/instance on the Blueprint.
	 * @param Blueprint			The blueprint to operate on.
	 * @param bOutAddedNewNode	Set to true when a new SCS node was added.
	 * @param bOutIsSCSOwned	Set to true if the returned object is a (compile-safe) SCS template
	 *                          owned by this Blueprint. False if it is a CDO instance for an inherited
	 *                          component, in which case the caller must avoid recompiling the BP.
	 */
	static UFloatingPawnMovement *FindOrAddFloatingPawnMovement(UBlueprint *Blueprint, bool &bOutAddedNewNode, bool &bOutIsSCSOwned);

	/** Set Convai default values on a FloatingPawnMovement template */
	static void ApplyFloatingPawnMovementDefaults(UFloatingPawnMovement *Comp);

	/** Find the inherited CharacterMovementComponent on the Blueprint's CDO */
	static UCharacterMovementComponent *GetBlueprintCharacterMovement(UBlueprint *Blueprint);

	/** Set Convai default values on a CharacterMovementComponent */
	static void ApplyCharacterMovementDefaults(UCharacterMovementComponent *Comp);

	/** Handle to the registered menu extension */
	static FDelegateHandle MenuExtensionHandle;

	/** Stores the package path for the current context menu action */
	static FString CurrentPackagePath;
};
