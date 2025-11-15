/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * MetricTokensLoader.h
 *
 * Loads metric tokens from constants into Slate style sets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Styling/SlateStyle.h"
#include "TokenLoaderBase.h"

/**
 * Loads metric tokens from constants into Slate style sets.
 */
class FMetricTokensLoader : public FTokenLoaderBase
{
public:
    /** Loads metric tokens from constants into the style set */
    static void Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style);

private:
    static void LoadSizeTokensFromConstants(TSharedPtr<FSlateStyleSet> Style);
    static void LoadSpacingTokensFromConstants(TSharedPtr<FSlateStyleSet> Style);
    static void LoadRadiusTokensFromConstants(TSharedPtr<FSlateStyleSet> Style);
};
