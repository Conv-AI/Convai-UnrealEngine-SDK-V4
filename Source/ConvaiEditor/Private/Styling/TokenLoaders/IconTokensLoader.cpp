/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IconTokensLoader.cpp
 *
 * Implementation of icon token loading from constants.
 */

#include "Styling/TokenLoaders/IconTokensLoader.h"
#include "Utility/ConvaiConstants.h"
#include "Logging/ConvaiEditorThemeLog.h"
#include "Interfaces/IPluginManager.h"
#include "Brushes/SlateImageBrush.h"
#include "Misc/Paths.h"

void FIconTokensLoader::Load(const TSharedPtr<FJsonObject> &Tokens, TSharedPtr<FSlateStyleSet> Style)
{
    if (!Style.IsValid())
    {
        UE_LOG(LogConvaiEditorTheme, Error, TEXT("IconTokensLoader: invalid Style parameter"));
        return;
    }

    LoadIconTokensFromConstants(Style);
}

void FIconTokensLoader::LoadIconTokensFromConstants(TSharedPtr<FSlateStyleSet> Style)
{
    using namespace ConvaiEditor::Constants;

    const FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("Convai"))->GetBaseDir();
    const FString ResourceRootPath = FPaths::Combine(*PluginBaseDir, ConvaiEditor::Constants::PluginResources::Root);

    RegisterIcon(Style, TEXT("Logo"), Icons::Logo, Layout::Icons::Logo, ResourceRootPath);
    RegisterIcon(Style, TEXT("Home"), Icons::Home, Layout::Icons::Home, ResourceRootPath);
    RegisterIcon(Style, TEXT("Settings"), Icons::Settings, Layout::Icons::Settings, ResourceRootPath);
    RegisterIcon(Style, TEXT("EyeVisible"), Icons::EyeVisible, Layout::Icons::VisibilityToggle, ResourceRootPath);
    RegisterIcon(Style, TEXT("EyeHidden"), Icons::EyeHidden, Layout::Icons::VisibilityToggle, ResourceRootPath);
    RegisterIcon(Style, TEXT("Actions"), Icons::Actions, Layout::Icons::Actions, ResourceRootPath);
    RegisterIcon(Style, TEXT("NarrativeDesign"), Icons::NarrativeDesign, Layout::Icons::NarrativeDesign, ResourceRootPath);
    RegisterIcon(Style, TEXT("LongTermMemory"), Icons::LongTermMemory, Layout::Icons::LongTermMemory, ResourceRootPath);
    RegisterIcon(Style, TEXT("OpenExternally"), Icons::OpenExternally, Layout::Icons::OpenExternally, ResourceRootPath);
    RegisterIcon(Style, TEXT("Toggle"), Icons::Toggle, Layout::Icons::Toggle, ResourceRootPath);
    RegisterIcon(Style, TEXT("Minimize"), Icons::Minimize, Layout::Icons::Minimize, ResourceRootPath);
    RegisterIcon(Style, TEXT("Maximize"), Icons::Maximize, Layout::Icons::Maximize, ResourceRootPath);
    RegisterIcon(Style, TEXT("Restore"), Icons::Restore, Layout::Icons::Restore, ResourceRootPath);
    RegisterIcon(Style, TEXT("Close"), Icons::Close, Layout::Icons::Close, ResourceRootPath);

    // Register plugin icon size for Unreal Engine's plugin system
    Style->Set(FName(TEXT("Convai.Size.icon.Icon128")), FVector2D(128.0f, 128.0f));
}

void FIconTokensLoader::RegisterIcon(TSharedPtr<FSlateStyleSet> Style, const FString &IconName,
                                     const FString &IconPath, const FVector2D &IconSize,
                                     const FString &ResourceRootPath)
{
    FString FullIconPath = FPaths::Combine(*ResourceRootPath, *IconPath);

    FSlateImageBrush *IconBrush = new FSlateImageBrush(FullIconPath, IconSize);

    FName StyleKey = FName(*BuildKey(TEXT("Icon"), IconName));
    Style->Set(StyleKey, IconBrush);
}
