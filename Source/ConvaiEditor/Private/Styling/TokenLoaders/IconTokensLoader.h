/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IconTokensLoader.h
 *
 * Loads icon tokens from constants into Slate style sets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Styling/SlateStyle.h"
#include "TokenLoaderBase.h"

/**
 * Loads icon tokens from constants into Slate style sets.
 */
class FIconTokensLoader : public FTokenLoaderBase
{
public:
    /** Loads icon tokens from constants into the style set */
    static void Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style);

private:
    static void LoadIconTokensFromConstants(TSharedPtr<FSlateStyleSet> Style);
    static void RegisterIcon(TSharedPtr<FSlateStyleSet> Style, const FString &IconName,
                             const FString &IconPath, const FVector2D &IconSize,
                             const FString &ResourceRootPath);
};
