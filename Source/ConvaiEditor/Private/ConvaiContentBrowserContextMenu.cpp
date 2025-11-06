/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiContentBrowserContextMenu.cpp
 *
 * Implementation of Content Browser context menu extensions for Convai tools.
 */

#include "ConvaiContentBrowserContextMenu.h"
#include "ToolMenus.h"
#include "ContentBrowserDataMenuContexts.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "FConvaiContentBrowserContextMenu"

FDelegateHandle FConvaiContentBrowserContextMenu::MenuExtensionHandle;
FString FConvaiContentBrowserContextMenu::CurrentPackagePath;

void FConvaiContentBrowserContextMenu::Register()
{
	UToolMenus *ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	UToolMenu *AddNewMenu = ToolMenus->ExtendMenu("ContentBrowser.AddNewContextMenu");
	if (AddNewMenu)
	{
		FToolMenuSection *GetContentSection = AddNewMenu->FindSection("ContentBrowserGetContent");
		if (!GetContentSection)
		{
			GetContentSection = &AddNewMenu->AddSection("ContentBrowserGetContent", LOCTEXT("GetContentMenuHeading", "Get Content"));
		}

		GetContentSection->AddDynamicEntry("ConvaiContent", FNewToolMenuSectionDelegate::CreateStatic(&FConvaiContentBrowserContextMenu::PopulateContextMenu));
	}
}

void FConvaiContentBrowserContextMenu::Unregister()
{
	UToolMenus *ToolMenus = UToolMenus::Get();
	if (ToolMenus)
	{
		ToolMenus->RemoveEntry("ContentBrowser.AddNewContextMenu", "ContentBrowserGetContent", "ConvaiContent");
	}
}

void FConvaiContentBrowserContextMenu::PopulateContextMenu(FToolMenuSection &InSection)
{
	UContentBrowserDataMenuContext_AddNewMenu *AddNewMenuContext = InSection.FindContext<UContentBrowserDataMenuContext_AddNewMenu>();
	if (AddNewMenuContext && AddNewMenuContext->bCanBeModified && AddNewMenuContext->bContainsValidPackagePath)
	{
		if (AddNewMenuContext->SelectedPaths.Num() > 0)
		{
			CurrentPackagePath = AddNewMenuContext->SelectedPaths[0].ToString();
		}

		InSection.AddSubMenu(
			"ConvaiSubMenu",
			LOCTEXT("ConvaiSubMenuLabel", "Convai"),
			LOCTEXT("ConvaiSubMenuTooltip", "Convai tools and options"),
			FNewToolMenuDelegate::CreateStatic(&FConvaiContentBrowserContextMenu::MakeConvaiSubMenu),
			FUIAction(),
			EUserInterfaceActionType::Button,
			false,
			FSlateIcon());
	}
}

void FConvaiContentBrowserContextMenu::MakeConvaiSubMenu(UToolMenu *Menu)
{
	if (!Menu)
	{
		return;
	}

	FToolMenuSection &ConvaiSection = Menu->AddSection("ConvaiActions", LOCTEXT("ConvaiActionsHeading", "Convai Actions"));

	ConvaiSection.AddMenuEntry(
		"ConvaiButton",
		LOCTEXT("ConvaiButtonLabel", "Vision  Render Target"),
		LOCTEXT("ConvaiButtonTooltip", "Vision  Render Target (placeholder)"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FConvaiContentBrowserContextMenu::ExecuteConvaiAction)));
}

void FConvaiContentBrowserContextMenu::ExecuteConvaiAction()
{
	CreateAndSaveRenderTarget(CurrentPackagePath);
}

void FConvaiContentBrowserContextMenu::CreateAndSaveRenderTarget(const FString &PackagePath)
{
	if (PackagePath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("ConvaiContentBrowserContextMenu: Package path is empty"));
		return;
	}

	FString CleanPath = PackagePath;
	if (CleanPath.StartsWith(TEXT("/All/")))
	{
		CleanPath = CleanPath.Mid(4);
	}

	if (!CleanPath.EndsWith(TEXT("/")))
	{
		CleanPath += TEXT("/");
	}

	FString AssetName = TEXT("VisionRenderTarget");
	FString FullPackagePath = CleanPath + AssetName;

	FString PackagePathOnly = FPackageName::GetLongPackagePath(*FullPackagePath);
	FString AssetNameOnly = FPackageName::GetShortName(*FullPackagePath);

	UTextureRenderTarget2D *NewRenderTarget = NewObject<UTextureRenderTarget2D>(
		CreatePackage(*FullPackagePath),
		*AssetNameOnly,
		RF_Public | RF_Standalone | RF_Transactional);

	if (!NewRenderTarget)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvaiContentBrowserContextMenu: Failed to create render target"));
		return;
	}

	NewRenderTarget->ResizeTarget(512, 512);
	NewRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	NewRenderTarget->ClearColor = FLinearColor::Black;
	NewRenderTarget->UpdateResourceImmediate(true);

	NewRenderTarget->MarkPackageDirty();

	UPackage *Package = NewRenderTarget->GetOutermost();
	if (Package)
	{
		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (UPackage::SavePackage(Package, NewRenderTarget, *PackageFileName, SaveArgs))
		{
			UE_LOG(LogTemp, Log, TEXT("ConvaiContentBrowserContextMenu: Successfully created and saved render target at %s"), *FullPackagePath);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("ConvaiContentBrowserContextMenu: Failed to save render target package"));
		}
	}
}

#undef LOCTEXT_NAMESPACE
