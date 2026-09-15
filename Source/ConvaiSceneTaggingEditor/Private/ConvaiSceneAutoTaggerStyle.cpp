// Copyright Epic Games, Inc. All Rights Reserved.

#include "ConvaiSceneAutoTaggerStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FConvaiSceneAutoTaggerStyle::StyleInstance = nullptr;

void FConvaiSceneAutoTaggerStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FConvaiSceneAutoTaggerStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FConvaiSceneAutoTaggerStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("ConvaiSceneAutoTaggerStyle"));
	return StyleSetName;
}

const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);

TSharedRef< FSlateStyleSet > FConvaiSceneAutoTaggerStyle::Create()
{
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("ConvaiSceneAutoTaggerStyle"));
	const TSharedPtr<IPlugin> ConvaiPlugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
	checkf(ConvaiPlugin.IsValid(), TEXT("The Convai plugin must be available before initializing Scene Auto Tagger styles."));
	Style->SetContentRoot(ConvaiPlugin->GetBaseDir() / TEXT("Resources/ConvaiSceneAutoTagger"));

	Style->Set("ConvaiSceneAutoTagger.OpenPluginWindow", new IMAGE_BRUSH_SVG(TEXT("SceneAutoTagger"), Icon20x20));

	return Style;
}

void FConvaiSceneAutoTaggerStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FConvaiSceneAutoTaggerStyle::Get()
{
	return *StyleInstance;
}
