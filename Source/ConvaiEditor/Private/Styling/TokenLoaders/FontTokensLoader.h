/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * FontTokensLoader.h
 *
 * Loads font tokens from constants into Slate style sets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Styling/SlateStyle.h"
#include "TokenLoaderBase.h"

/**
 * Loads font tokens from constants into Slate style sets.
 */
class FFontTokensLoader : public FTokenLoaderBase
{
public:
    /** Loads font tokens from constants into the style set */
    static void Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style);

private:
    static void LoadFontTokensFromConstants(TSharedPtr<FSlateStyleSet> Style);
    static void RegisterFontStyle(const TSharedPtr<FSlateStyleSet>& Style, const FString& StyleName,
                                  const FString& FamilyName, float FontSize,
                                  const TMap<FString, FString>& FontFamilies);
};
