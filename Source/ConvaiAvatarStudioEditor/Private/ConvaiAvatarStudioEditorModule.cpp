// Copyright Convai Inc. All Rights Reserved.
#include "Modules/ModuleManager.h"
#include "ConvaiAvatarStudioController.h"
#include "Services/ConvaiAvatarDownloadService.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Brushes/SlateImageBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

class FConvaiAvatarStudioEditorModule : public IModuleInterface
{
    TSharedPtr<FConvaiAvatarStudioController,ESPMode::ThreadSafe> Controller;
    TSharedPtr<FSlateStyleSet> IconStyle;
    FSlateIcon GetToolIcon() const
    { return IconStyle ? FSlateIcon(IconStyle->GetStyleSetName(),"CloudAvatars") : FSlateIcon(FAppStyle::GetAppStyleSetName(),"LevelEditor.Tabs.ContentBrowser"); }
public:
    void StartupModule() override
    {
        if(IsRunningCommandlet())return;
		// Default-phase plugin modules load before UEditorEngine initializes editor
		// subsystems. Repair legacy root rules before the reference-domain DB sees them.
		// Listing local records reads metadata only; it does not mount or load Blueprints.
		TArray<FString> StartupNotices;
		TArray<FConvaiAvatarPreparedAsset> LocalRecords; TSet<FString> IncompleteRecords; FString MigrationError;
		if (!FConvaiAvatarWorkspace::ListLocalAssetRecords(LocalRecords, IncompleteRecords, MigrationError))
			StartupNotices.Add(TEXT("Some avatar cook exclusions could not be reviewed. ") + MigrationError);
		else for (const auto& Record : LocalRecords) if (Record.bIsStaging)
		{
			if (!FConvaiAvatarProjectConfiguration::MigrateLegacyStagingCookExclusion(FPaths::ProjectDir(), Record.PluginName, MigrationError))
				StartupNotices.AddUnique(TEXT("An avatar cook exclusion needs attention. ") + MigrationError);
		}
		for (const FString& Notice : StartupNotices) UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars: %s"), *Notice);
        if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI")))
        {
            IconStyle = MakeShared<FSlateStyleSet>(TEXT("ConvaiCloudAvatarsStyle"));
            IconStyle->Set("CloudAvatars", new FSlateVectorImageBrush(Plugin->GetBaseDir() / TEXT("Resources/AvatarStudio/CloudAvatars.svg"), FVector2D(20,20)));
            FSlateStyleRegistry::RegisterSlateStyle(*IconStyle);
        }
        // Apply previously confirmed replacements before the startup map loads its avatar packages.
        TArray<FConvaiAvatarPendingDownload> Pending;FString RecoveryError;
        FConvaiAvatarDownloadService::GetPending(Pending,RecoveryError);
        for(auto& Job:Pending)
        {
            FString Notice,Error;
            FConvaiAvatarDownloadService::TryApply(Job,false,Notice,Error,false);
        }
        Controller=MakeShared<FConvaiAvatarStudioController,ESPMode::ThreadSafe>();
		if (!StartupNotices.IsEmpty()) Controller->SetStartupNotice(FString::Join(StartupNotices, TEXT("\n")));
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner("ConvaiAvatarStudio",FOnSpawnTab::CreateRaw(this,&FConvaiAvatarStudioEditorModule::SpawnTab))
            .SetDisplayName(NSLOCTEXT("ConvaiAvatarStudio","Title","Cloud Avatars"))
            .SetTooltipText(NSLOCTEXT("ConvaiAvatarStudio","Tooltip","Create, download, and manage your custom avatars."))
            .SetIcon(GetToolIcon())
            .SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,&FConvaiAvatarStudioEditorModule::RegisterMenus));
    }
    void ShutdownModule() override
    {
        if(Controller)Controller->Shutdown();
        if(UToolMenus::IsToolMenuUIEnabled()){UToolMenus::UnRegisterStartupCallback(this);UToolMenus::UnregisterOwner(this);}
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner("ConvaiAvatarStudio");
        Controller.Reset();
        if (IconStyle) { FSlateStyleRegistry::UnRegisterSlateStyle(*IconStyle); IconStyle.Reset(); }
    }
    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
    {
        // The tab retains the brush owner even if it outlives module shutdown.
        TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)[Controller->CreateWidget()];
        Tab->SetTabIcon(TAttribute<const FSlateBrush*>::CreateLambda([Style = IconStyle]() -> const FSlateBrush*
            { return Style ? Style->GetBrush("CloudAvatars") : FAppStyle::GetBrush("LevelEditor.Tabs.ContentBrowser"); }));
        return Tab;
    }
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        auto& Section=UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools")->FindOrAddSection("Convai");
        Section.Label=NSLOCTEXT("ConvaiAvatarStudio","Section","Convai");
        Section.AddMenuEntry("ConvaiAvatarStudio",NSLOCTEXT("ConvaiAvatarStudio","Menu","Cloud Avatars"),NSLOCTEXT("ConvaiAvatarStudio","Help","Upload and manage custom avatars for Avatar Studio and ConvaiSim streaming."),GetToolIcon(),FUIAction(FExecuteAction::CreateLambda([]{FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("ConvaiAvatarStudio")));})));
    }
};
IMPLEMENT_MODULE(FConvaiAvatarStudioEditorModule,ConvaiAvatarStudioEditor)
