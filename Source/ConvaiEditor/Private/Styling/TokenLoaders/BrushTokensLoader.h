/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * BrushTokensLoader.h
 *
 * Loads brush tokens from theme JSON into Slate style sets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Styling/SlateStyle.h"
#include "TokenLoaderBase.h"

/**
 * Loads brush tokens from theme JSON into Slate style sets.
 */
class FBrushTokensLoader : public FTokenLoaderBase
{
public:
    /** Loads brush tokens from JSON into the style set */
    static void Load(const TSharedPtr<FJsonObject> &ThemeObject, TSharedPtr<FSlateStyleSet> Style);

private:
    static void ProcessBrushesObject(const TSharedPtr<FJsonObject> &JsonObj, TSharedPtr<FSlateStyleSet> Style, const FString &ResourceRootPath);
    static void GenerateBrushesFromColors(const TSharedPtr<FJsonObject> &ColorTokens, TSharedPtr<FSlateStyleSet> Style);
    static void GenerateSurfaceBrushes(const TSharedPtr<FJsonObject> &ColorTokens, TSharedPtr<FSlateStyleSet> Style);
    static void ProcessComponentBrushes(const TSharedPtr<FJsonObject> &ComponentObject, TSharedPtr<FSlateStyleSet> Style);
    static void FlattenColorObject(const TSharedPtr<FJsonObject> &JsonObj, TMap<FString, FLinearColor> &OutColors, const FString &Prefix);
};
