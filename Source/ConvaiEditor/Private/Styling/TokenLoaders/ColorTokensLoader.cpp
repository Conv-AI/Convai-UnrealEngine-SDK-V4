/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ColorTokensLoader.cpp
 *
 * Implementation of color token loading from theme JSON.
 */

#include "Styling/TokenLoaders/ColorTokensLoader.h"
#include "Brushes/SlateColorBrush.h"
#include "Logging/ConvaiEditorThemeLog.h"

void FColorTokensLoader::Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style)
{
    if (!Tokens.IsValid() || !Style.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ColorTokensLoader: invalid parameters"));
        return;
    }

    const TSharedPtr<FJsonObject> *ColorTokens = nullptr;
    if (!Tokens->TryGetObjectField(TEXT("color"), ColorTokens) || !ColorTokens)
    {
        UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ColorTokensLoader: no color tokens found in theme"));
        return;
    }

    TSet<FString> RegisteredKeys;
    ProcessColorObject(*ColorTokens, Style, TEXT(""), RegisteredKeys);
}

void FColorTokensLoader::ProcessColorObject(const TSharedPtr<FJsonObject> &JsonObj, TSharedPtr<FSlateStyleSet> Style,
                                            const FString &CurrentPath, TSet<FString> &RegisteredKeys)
{
    if (!JsonObj.IsValid() || !Style.IsValid())
    {
        return;
    }

    for (const auto &Pair : JsonObj->Values)
    {
        const FString &Key = Pair.Key;
        const TSharedPtr<FJsonValue> &Value = Pair.Value;

        FString NewPath = CurrentPath.IsEmpty() ? Key : FString::Printf(TEXT("%s.%s"), *CurrentPath, *Key);

        if (Value->Type == EJson::Object)
        {
            ProcessColorObject(Value->AsObject(), Style, NewPath, RegisteredKeys);
        }
        else if (Value->Type == EJson::String)
        {
            FString HexColor = Value->AsString();
            FLinearColor Color = ParseHexColor(HexColor);

            FString ColorKey = BuildKey(TEXT("Color"), NewPath);
            FString BrushKey = BuildKey(TEXT("ColorBrush"), NewPath);

            bool bHasColorDuplicate = RegisteredKeys.Contains(ColorKey);
            bool bHasBrushDuplicate = RegisteredKeys.Contains(BrushKey);

            if (bHasColorDuplicate)
            {
                UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ColorTokensLoader: duplicate color key detected: %s. Skipping registration."), *ColorKey);
            }
            else
            {
                FName StyleKey = FName(*ColorKey);
                Style->Set(StyleKey, Color);
                RegisteredKeys.Add(ColorKey);
            }

            if (bHasBrushDuplicate)
            {
                UE_LOG(LogConvaiEditorTheme, Warning, TEXT("ColorTokensLoader: duplicate brush key detected: %s. Skipping registration."), *BrushKey);
            }
            else
            {
                FName BrushStyleKey = FName(*BrushKey);
                Style->Set(BrushStyleKey, new FSlateColorBrush(Color));
                RegisteredKeys.Add(BrushKey);
            }
        }
    }
}

FLinearColor FColorTokensLoader::ParseHexColor(const FString &HexString)
{
    return FLinearColor::FromSRGBColor(FColor::FromHex(HexString));
}
