/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * BrushTokensLoader.cpp
 *
 * Implementation of brush token loading from theme JSON.
 */

#include "Styling/TokenLoaders/BrushTokensLoader.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Utility/ConvaiConstants.h"

void FBrushTokensLoader::Load(const TSharedPtr<FJsonObject> &ThemeObject, TSharedPtr<FSlateStyleSet> Style)
{
    if (!ThemeObject.IsValid() || !Style.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("BrushTokensLoader: invalid parameters"));
        return;
    }

    const FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("Convai"))->GetBaseDir();
    const FString ResourceRootPath = FPaths::Combine(*PluginBaseDir, ConvaiEditor::Constants::PluginResources::Root);

    const TSharedPtr<FJsonObject> *BrushesObject = nullptr;
    if (ThemeObject->TryGetObjectField(TEXT("brushes"), BrushesObject) && BrushesObject)
    {
        ProcessBrushesObject(*BrushesObject, Style, ResourceRootPath);
    }
    else
    {
    }

    const TSharedPtr<FJsonObject> *TokensObject = nullptr;
    if (ThemeObject->TryGetObjectField(TEXT("tokens"), TokensObject) && TokensObject)
    {
        const TSharedPtr<FJsonObject> *ColorTokens = nullptr;
        if ((*TokensObject)->TryGetObjectField(TEXT("color"), ColorTokens) && ColorTokens)
        {
            GenerateBrushesFromColors(*ColorTokens, Style);
        }
    }
}

void FBrushTokensLoader::ProcessBrushesObject(const TSharedPtr<FJsonObject> &JsonObj, TSharedPtr<FSlateStyleSet> Style, const FString &ResourceRootPath)
{
    if (!JsonObj.IsValid() || !Style.IsValid())
    {
        return;
    }

    for (const auto &Pair : JsonObj->Values)
    {
        const FString &Key = Pair.Key;
        const TSharedPtr<FJsonValue> &Value = Pair.Value;

        if (Value->Type == EJson::String)
        {
            const FString BrushValue = Value->AsString();

            if (BrushValue.StartsWith(TEXT("#")))
            {
                FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(BrushValue));
                FSlateColorBrush *ColorBrush = new FSlateColorBrush(Color);
                FName StyleKey = FName(*Key);
                Style->Set(StyleKey, ColorBrush);
            }
            else if (BrushValue.Contains(TEXT("/")))
            {
                FString FullImagePath = FPaths::Combine(ResourceRootPath, *BrushValue);
                FVector2D ImageSize(16.0f, 16.0f);
                FString ImageName = FPaths::GetBaseFilename(BrushValue);

                if (BrushValue.Contains(TEXT("HomePage/")))
                {
                    FName CardSizeKey = FName(TEXT("Convai.Size.homePageCard.dimensions"));
                    bool bFound = false;
                    try
                    {
                        ImageSize = Style->GetVector(CardSizeKey);
                        bFound = true;
                    }
                    catch (...)
                    {
                    }

                    if (!bFound)
                    {
                    }
                }
                else if (BrushValue.Contains(TEXT("Support/")))
                {
                    FName SizeKey = FName(*FString::Printf(TEXT("Convai.Size.icon.%s"), *ImageName));
                    bool bFound = false;
                    try
                    {
                        ImageSize = Style->GetVector(SizeKey);
                        bFound = true;
                    }
                    catch (...)
                    {
                    }

                    if (!bFound)
                    {
                    }
                }
                else
                {
                    FName SizeKey = FName(*FString::Printf(TEXT("Convai.Size.icon.%s"), *ImageName));
                    bool bFound = false;
                    try
                    {
                        ImageSize = Style->GetVector(SizeKey);
                        bFound = true;
                    }
                    catch (...)
                    {
                    }

                    if (!bFound)
                    {
                    }
                }

                FSlateImageBrush *ImageBrush = new FSlateImageBrush(FullImagePath, ImageSize);
                FName StyleKey = FName(*Key);
                Style->Set(StyleKey, ImageBrush);
            }
            else
            {
                UE_LOG(LogConvaiEditorTheme, Warning, TEXT("BrushTokensLoader: unsupported brush value format: %s"), *BrushValue);
            }
        }
        else
        {
            UE_LOG(LogConvaiEditorTheme, Warning, TEXT("BrushTokensLoader: unsupported brush value type for key %s"), *Key);
        }
    }
}

void FBrushTokensLoader::GenerateBrushesFromColors(const TSharedPtr<FJsonObject> &ColorTokens, TSharedPtr<FSlateStyleSet> Style)
{
    if (!ColorTokens.IsValid() || !Style.IsValid())
    {
        return;
    }

    GenerateSurfaceBrushes(ColorTokens, Style);

    TMap<FString, FLinearColor> FlattenedColors;
    FlattenColorObject(ColorTokens, FlattenedColors, TEXT(""));
}

void FBrushTokensLoader::GenerateSurfaceBrushes(const TSharedPtr<FJsonObject> &ColorTokens, TSharedPtr<FSlateStyleSet> Style)
{
    const TSharedPtr<FJsonObject> *SurfaceObject = nullptr;
    if (ColorTokens->TryGetObjectField(TEXT("surface"), SurfaceObject) && SurfaceObject)
    {
        for (const auto &Pair : (*SurfaceObject)->Values)
        {
            const FString &ColorKey = Pair.Key;
            const TSharedPtr<FJsonValue> &ColorValue = Pair.Value;

            if (ColorValue->Type == EJson::String)
            {
                const FString &ColorStr = ColorValue->AsString();
                if (ColorStr.StartsWith(TEXT("#")))
                {
                    FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(ColorStr));
                    FName BrushStyleKey = FName(*FString::Printf(TEXT("Convai.Color.surface.%s"), *ColorKey));
                    Style->Set(BrushStyleKey, new FSlateColorBrush(Color));
                }
            }
        }
    }

    const TSharedPtr<FJsonObject> *ComponentObject = nullptr;
    if (ColorTokens->TryGetObjectField(TEXT("component"), ComponentObject) && ComponentObject)
    {
        ProcessComponentBrushes(*ComponentObject, Style);
    }
}

void FBrushTokensLoader::ProcessComponentBrushes(const TSharedPtr<FJsonObject> &ComponentObject, TSharedPtr<FSlateStyleSet> Style)
{
    for (const auto &GroupPair : ComponentObject->Values)
    {
        const FString &GroupKey = GroupPair.Key;
        const TSharedPtr<FJsonValue> &GroupValue = GroupPair.Value;

        if (GroupValue->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> GroupObject = GroupValue->AsObject();

            for (const auto &ColorPair : GroupObject->Values)
            {
                const FString &ColorKey = ColorPair.Key;
                const TSharedPtr<FJsonValue> &ColorValue = ColorPair.Value;

                if (ColorValue->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject> NestedObject = ColorValue->AsObject();

                    for (const auto &NestedPair : NestedObject->Values)
                    {
                        const FString &NestedKey = NestedPair.Key;
                        const TSharedPtr<FJsonValue> &NestedValue = NestedPair.Value;

                        if (NestedValue->Type == EJson::String && NestedValue->AsString().StartsWith(TEXT("#")))
                        {
                            FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(NestedValue->AsString()));
                            FName BrushStyleKey = FName(*FString::Printf(TEXT("Convai.Color.component.%s.%s.%s"),
                                                                         *GroupKey, *ColorKey, *NestedKey));
                            Style->Set(BrushStyleKey, new FSlateColorBrush(Color));
                        }
                    }
                }
                else if (ColorValue->Type == EJson::String && ColorValue->AsString().StartsWith(TEXT("#")))
                {
                    FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(ColorValue->AsString()));
                    FName BrushStyleKey = FName(*FString::Printf(TEXT("Convai.Color.component.%s.%s"),
                                                                 *GroupKey, *ColorKey));
                    Style->Set(BrushStyleKey, new FSlateColorBrush(Color));
                }
            }
        }
    }
}

void FBrushTokensLoader::FlattenColorObject(const TSharedPtr<FJsonObject> &JsonObj, TMap<FString, FLinearColor> &OutColors, const FString &Prefix)
{
    if (!JsonObj.IsValid())
    {
        return;
    }

    for (const auto &Pair : JsonObj->Values)
    {
        const FString &Key = Pair.Key;
        const TSharedPtr<FJsonValue> &Value = Pair.Value;

        FString CurrentPath = Prefix.IsEmpty() ? Key : FString::Printf(TEXT("%s.%s"), *Prefix, *Key);

        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> NestedObj = Value->AsObject();
            if (NestedObj.IsValid())
            {
                FlattenColorObject(NestedObj, OutColors, CurrentPath);
            }
        }
        else if (Value->Type == EJson::String)
        {
            const FString &ColorStr = Value->AsString();

            if (ColorStr.StartsWith(TEXT("#")))
            {
                FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(ColorStr));
                FString FinalKey = Prefix.IsEmpty() ? Key : CurrentPath;
                OutColors.Add(FinalKey, Color);
            }
        }
    }
}
