// Copyright Convai. All Rights Reserved.

#include "ConvaiSceneTaggingEditor.h"
#include "ConvaiSceneAutoTaggerStyle.h"
#include "ConvaiSceneAutoTaggerCommands.h"
#include "SceneAutoTaggerNativeCoreAdapter.h"
#include "SceneAutoTaggerController.h"
#include "SConvaiSceneAutoTagger.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "ToolMenus.h"

static const FName ConvaiSceneAutoTaggerTabName("ConvaiSceneAutoTagger");
static const FName ConvaiSceneObjectsTabName("ConvaiSceneObjects");

DEFINE_LOG_CATEGORY_STATIC(LogConvaiSceneAutoTaggerNativeCore, Log, All);

#define LOCTEXT_NAMESPACE "FConvaiSceneTaggingEditorModule"

void FConvaiSceneTaggingEditorModule::StartupModule()
{
	FSceneAutoTaggerNativeCoreAdapter& NativeCore = FSceneAutoTaggerNativeCoreAdapter::Get();
	if (!NativeCore.Initialize())
	{
		UE_LOG(
			LogConvaiSceneAutoTaggerNativeCore,
			Warning,
			TEXT("%s The rest of the Convai editor remains available."),
			*NativeCore.GetDiagnostic());
	}

	FConvaiSceneAutoTaggerStyle::Initialize();
	FConvaiSceneAutoTaggerStyle::ReloadTextures();

	FConvaiSceneAutoTaggerCommands::Register();

	PluginCommands = MakeShareable(new FUICommandList);
	Controller = MakeShared<FSceneAutoTaggerController>();

	PluginCommands->MapAction(
		FConvaiSceneAutoTaggerCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FConvaiSceneTaggingEditorModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FConvaiSceneTaggingEditorModule::RegisterMenus));

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ConvaiSceneAutoTaggerTabName, FOnSpawnTab::CreateRaw(this, &FConvaiSceneTaggingEditorModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("FConvaiSceneAutoTaggerTabTitle", "Convai Scene Auto Tagger"))
		.SetTooltipText(LOCTEXT("FConvaiSceneAutoTaggerTabTooltip", "Explore a level, review scene-object descriptions, and apply accepted metadata to Convai."))
		.SetIcon(FSlateIcon(FConvaiSceneAutoTaggerStyle::GetStyleSetName(), "ConvaiSceneAutoTagger.OpenPluginWindow"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

}

void FConvaiSceneTaggingEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FConvaiSceneAutoTaggerStyle::Shutdown();

	FConvaiSceneAutoTaggerCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ConvaiSceneAutoTaggerTabName);
	Controller.Reset();
	FSceneAutoTaggerNativeCoreAdapter::Get().Shutdown();
}

TSharedRef<SDockTab> FConvaiSceneTaggingEditorModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SConvaiSceneAutoTagger)
			.Controller(Controller)
		];
}

void FConvaiSceneTaggingEditorModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(ConvaiSceneAutoTaggerTabName);
}

void FConvaiSceneTaggingEditorModule::OpenSceneObjectsManager()
{
	FGlobalTabmanager::Get()->TryInvokeTab(ConvaiSceneObjectsTabName);
}

void FConvaiSceneTaggingEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("Convai", LOCTEXT("ConvaiToolsSection", "Convai"));
			Section.AddMenuEntryWithCommandList(FConvaiSceneAutoTaggerCommands::Get().OpenPluginWindow, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FConvaiSceneAutoTaggerCommands::Get().OpenPluginWindow));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FConvaiSceneTaggingEditorModule, ConvaiSceneTaggingEditor)
