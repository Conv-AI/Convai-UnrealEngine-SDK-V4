/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ColorTokensLoader.h
 *
 * Loads color tokens from theme JSON into Slate style sets.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Styling/SlateStyle.h"
#include "TokenLoaderBase.h"

/**
 * Loads color tokens from theme JSON into Slate style sets.
 */
class FColorTokensLoader : public FTokenLoaderBase
{
public:
    /** Loads color tokens from JSON into the style set */
    static void Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style);

private:
    static void ProcessColorObject(const TSharedPtr<FJsonObject> &JsonObj, TSharedPtr<FSlateStyleSet> Style,
                                   const FString &CurrentPath, TSet<FString> &RegisteredKeys);
    static FLinearColor ParseHexColor(const FString &HexString);
};
