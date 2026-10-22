/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiVisionRenderTargetActions.cpp
 *
 * Implementation of asset actions for Convai Vision Render Target assets.
 */

#include "ConvaiVisionRenderTargetActions.h"
#include "AssetToolsModule.h"
#include "Modules/ModuleManager.h"
#include "Engine/TextureRenderTarget2D.h"

FText FConvaiVisionRenderTargetActions::GetName() const
{
	return FText::FromString("Vision Render Target");
}

FColor FConvaiVisionRenderTargetActions::GetTypeColor() const
{
	return FColor(0, 150, 200);
}

UClass *FConvaiVisionRenderTargetActions::GetSupportedClass() const
{
	return UTextureRenderTarget2D::StaticClass();
}

uint32 FConvaiVisionRenderTargetActions::GetCategories()
{
	IAssetTools &AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	return AssetTools.RegisterAdvancedAssetCategory(
		FName(TEXT("Convai")),
		FText::FromString("Convai"));
}

UConvaiVisionRenderTargetFactory::UConvaiVisionRenderTargetFactory()
{
	SupportedClass = UTextureRenderTarget2D::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject *UConvaiVisionRenderTargetFactory::FactoryCreateNew(UClass *Class, UObject *InParent, FName Name, EObjectFlags Flags, UObject *Context, FFeedbackContext *Warn)
{
	UTextureRenderTarget2D *NewRenderTarget = NewObject<UTextureRenderTarget2D>(InParent, Name, Flags);

	if (NewRenderTarget)
	{
		NewRenderTarget->ResizeTarget(DefaultSizeX, DefaultSizeY);
		NewRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		NewRenderTarget->ClearColor = DefaultClearColor;
		NewRenderTarget->UpdateResourceImmediate(true);
	}

	return NewRenderTarget;
}
