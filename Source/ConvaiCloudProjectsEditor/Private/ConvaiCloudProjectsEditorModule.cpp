// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiCloudProjectsController.h"

#include "Brushes/SlateImageBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "ConvaiCloudProjects"

namespace
{
const TCHAR* TabName = TEXT("ConvaiCloudProjects");
const TCHAR* IconName = TEXT("CloudProjects");
}

/** Tools > Convai > Cloud Projects: package and upload this project without leaving the editor. */
class FConvaiCloudProjectsEditorModule : public IModuleInterface
{
	TSharedPtr<FConvaiCloudProjectsController> Controller;
	TSharedPtr<FSlateStyleSet> IconStyle;

	FSlateIcon GetToolIcon() const
	{
		return IconStyle
			? FSlateIcon(IconStyle->GetStyleSetName(), IconName)
			: FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("MainFrame.PackageProject"));
	}

public:
	void StartupModule() override
	{
		if (IsRunningCommandlet()) return;

		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI")))
		{
			IconStyle = MakeShared<FSlateStyleSet>(TEXT("ConvaiCloudProjectsStyle"));
			IconStyle->Set(IconName, new FSlateVectorImageBrush(
				Plugin->GetBaseDir() / TEXT("Resources/CloudProjects/CloudProjects.svg"), FVector2D(20, 20)));
			FSlateStyleRegistry::RegisterSlateStyle(*IconStyle);
		}

		Controller = MakeShared<FConvaiCloudProjectsController>();

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName,
			FOnSpawnTab::CreateRaw(this, &FConvaiCloudProjectsEditorModule::SpawnTab))
			.SetDisplayName(LOCTEXT("Title", "Cloud Projects"))
			.SetTooltipText(LOCTEXT("Tooltip", "Package this project and upload it to Convai for browser streaming."))
			.SetIcon(GetToolIcon())
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FConvaiCloudProjectsEditorModule::RegisterMenus));
	}

	void ShutdownModule() override
	{
		if (Controller) Controller->Shutdown();
		if (UToolMenus::IsToolMenuUIEnabled())
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		Controller.Reset();
		if (IconStyle)
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*IconStyle);
			IconStyle.Reset();
		}
	}

	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
	{
		// The tab can outlive module shutdown, so it holds the brush owner rather than the module.
		TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)[Controller->CreateWidget()];
		Tab->SetTabIcon(TAttribute<const FSlateBrush*>::CreateLambda([Style = IconStyle]() -> const FSlateBrush*
		{
			return Style ? Style->GetBrush(IconName) : FAppStyle::GetBrush(TEXT("MainFrame.PackageProject"));
		}));
		return Tab;
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		FToolMenuSection& Section = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"))
			->FindOrAddSection(TEXT("Convai"));
		Section.Label = LOCTEXT("Section", "Convai");

		Section.AddMenuEntry(TabName,
			LOCTEXT("Menu", "Cloud Projects"),
			LOCTEXT("Help", "Package this project and upload it to Convai for browser streaming."),
			GetToolIcon(),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				FGlobalTabmanager::Get()->TryInvokeTab(FName(TabName));
			})));
	}
};

IMPLEMENT_MODULE(FConvaiCloudProjectsEditorModule, ConvaiCloudProjectsEditor)

#undef LOCTEXT_NAMESPACE
