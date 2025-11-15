/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ThemeManager.cpp
 *
 * Implementation of the theme manager for loading and applying UI themes.
 */

#include "Styling/ThemeManager.h"
#include "Styling/TokenLoaders/ColorTokensLoader.h"
#include "Styling/TokenLoaders/MetricTokensLoader.h"
#include "Styling/TokenLoaders/FontTokensLoader.h"
#include "Styling/TokenLoaders/IconTokensLoader.h"
#include "Styling/TokenLoaders/BrushTokensLoader.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Utility/ConvaiValidationUtils.h"
#include "Utility/ConvaiConstants.h"

FThemeManager::FThemeManager()
{
}

FThemeManager::~FThemeManager()
{
}

void FThemeManager::Startup()
{
    CurrentThemeId = TEXT("dark");
}

void FThemeManager::Shutdown()
{
    Style.Reset();
    CurrentThemeId.Empty();
    OnThemeChangedDelegate.Clear();
}

void FThemeManager::SetActiveTheme(const FString &ThemeId)
{
    if (CurrentThemeId == ThemeId && Style.IsValid())
    {
        return;
    }

    if (!LoadThemeFile(ThemeId))
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ThemeManager: failed to load theme '%s'"), *ThemeId);
        return;
    }

    CurrentThemeId = ThemeId;
    OnThemeChangedDelegate.Broadcast();
}

bool FThemeManager::LoadThemeFile(const FString &Id)
{
    const FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("Convai"))->GetBaseDir();
    const FString ThemesDir = FPaths::Combine(PluginBaseDir, ConvaiEditor::Constants::PluginResources::Themes);
    const FString ThemeFilePath = FPaths::Combine(ThemesDir, FString::Printf(TEXT("%s.json"), *Id));

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *ThemeFilePath))
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("ThemeManager: failed to read theme file: %s"), *ThemeFilePath);
        return false;
    }

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FConvaiValidationUtils::Check(FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid(), FString::Printf(TEXT("Failed to parse theme JSON: %s"), *ThemeFilePath)))
    {
        return false;
    }

    const FString &Context = ThemeFilePath;
    const TSharedPtr<FJsonObject> *InfoObject = nullptr;
    if (!FConvaiValidationUtils::GetJsonObjectField(JsonObject, TEXT("info"), &InfoObject, Context))
    {
        return false;
    }

    FString ThemeId;
    if (!FConvaiValidationUtils::GetJsonStringField(*InfoObject, TEXT("id"), ThemeId, Context))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> *TokensObject = nullptr;
    if (!FConvaiValidationUtils::GetJsonObjectField(JsonObject, TEXT("tokens"), &TokensObject, Context))
    {
        return false;
    }

    Style = MakeShared<FSlateStyleSet>(TEXT("ConvaiStyle"));
    const FString ResourceRootPath = FPaths::Combine(PluginBaseDir, ConvaiEditor::Constants::PluginResources::Root);
    Style->SetContentRoot(ResourceRootPath);

    FColorTokensLoader::Load(*TokensObject, Style);
    FMetricTokensLoader::Load(*TokensObject, Style);
    FFontTokensLoader::Load(*TokensObject, Style);
    FIconTokensLoader::Load(*TokensObject, Style);
    FBrushTokensLoader::Load(JsonObject, Style);

    return true;
}
