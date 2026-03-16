/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiContentBrowserContextMenu.h
 *
 * Handles Content Browser context menu integration for Convai.
 */

#pragma once

#include "CoreMinimal.h"

struct FToolMenuSection;
class UToolMenu;

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

	/** Handle to the registered menu extension */
	static FDelegateHandle MenuExtensionHandle;

	/** Stores the package path for the current context menu action */
	static FString CurrentPackagePath;
};
