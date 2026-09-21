// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace ConvaiAvatarWorkspaceLinks
{
	/** Explicit project root keeps relocation fixtures isolated from the open editor project. */
	bool Ensure(const FString& ProjectRoot, const FString& PluginName, FString& Error, bool bForceJunction = false);
	bool EnsureAll(const FString& ProjectRoot, const FString& SelectedPlugin, FString& Error);
	FString ReceiptPath(const FString& ProjectRoot, const FString& PluginName);
#if PLATFORM_WINDOWS
	/** Low-level creation also used by disk-only fixtures to reproduce a copied absolute junction. */
	bool CreateLink(const FString& Source, const FString& Link, bool bForceJunction, FString& Error);
#endif
}
