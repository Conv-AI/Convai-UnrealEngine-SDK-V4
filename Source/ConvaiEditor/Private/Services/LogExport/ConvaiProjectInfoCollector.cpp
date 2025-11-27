/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiProjectInfoCollector.cpp
 *
 * Implementation of project information collector.
 */

#include "Services/LogExport/ConvaiProjectInfoCollector.h"
#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "ProjectDescriptor.h"
#include "Interfaces/IProjectManager.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

TSharedPtr<FJsonObject> FConvaiProjectInfoCollector::CollectInfo() const
{
    TSharedPtr<FJsonObject> ProjectInfo = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> EngineInfo = CollectEngineInfo();
    if (EngineInfo.IsValid())
    {
        ProjectInfo->SetObjectField(TEXT("Engine"), EngineInfo);
    }

    TSharedPtr<FJsonObject> ProjectData = CollectProjectInfo();
    if (ProjectData.IsValid())
    {
        ProjectInfo->SetObjectField(TEXT("Project"), ProjectData);
    }

    TSharedPtr<FJsonObject> ConvaiPlugin = CollectConvaiPluginInfo();
    if (ConvaiPlugin.IsValid())
    {
        ProjectInfo->SetObjectField(TEXT("ConvaiPlugin"), ConvaiPlugin);
    }

    TArray<TSharedPtr<FJsonValue>> PluginsList = CollectInstalledPlugins();
    if (PluginsList.Num() > 0)
    {
        ProjectInfo->SetArrayField(TEXT("InstalledPlugins"), PluginsList);
    }

    TSharedPtr<FJsonObject> Settings = CollectProjectSettings();
    if (Settings.IsValid())
    {
        ProjectInfo->SetObjectField(TEXT("ProjectSettings"), Settings);
    }

    ProjectInfo->SetStringField(TEXT("CollectionTimestamp"), FDateTime::UtcNow().ToIso8601());

    return ProjectInfo;
}

TSharedPtr<FJsonObject> FConvaiProjectInfoCollector::CollectEngineInfo() const
{
    TSharedPtr<FJsonObject> EngineInfo = MakeShared<FJsonObject>();

    FEngineVersion EngineVersion = FEngineVersion::Current();
    EngineInfo->SetNumberField(TEXT("Major"), EngineVersion.GetMajor());
    EngineInfo->SetNumberField(TEXT("Minor"), EngineVersion.GetMinor());
    EngineInfo->SetNumberField(TEXT("Patch"), EngineVersion.GetPatch());
    EngineInfo->SetNumberField(TEXT("Changelist"), EngineVersion.GetChangelist());
    EngineInfo->SetStringField(TEXT("Branch"), EngineVersion.GetBranch());
    EngineInfo->SetStringField(TEXT("VersionString"), EngineVersion.ToString());

    EngineInfo->SetStringField(TEXT("BuildConfiguration"), GetBuildConfiguration());

    EngineInfo->SetStringField(TEXT("EngineDirectory"), FPaths::EngineDir());

    EngineInfo->SetBoolField(TEXT("IsEditor"), GIsEditor);

    EngineInfo->SetStringField(TEXT("CommandLine"), FCommandLine::Get());

    return EngineInfo;
}

TSharedPtr<FJsonObject> FConvaiProjectInfoCollector::CollectProjectInfo() const
{
    TSharedPtr<FJsonObject> ProjectInfo = MakeShared<FJsonObject>();

    ProjectInfo->SetStringField(TEXT("Name"), FApp::GetProjectName());

    ProjectInfo->SetStringField(TEXT("Directory"), FPaths::ProjectDir());

    const FString ProjectFilePath = FPaths::GetProjectFilePath();
    ProjectInfo->SetStringField(TEXT("ProjectFilePath"), ProjectFilePath);

    const FProjectDescriptor *Descriptor = IProjectManager::Get().GetCurrentProject();
    if (Descriptor)
    {
        ProjectInfo->SetStringField(TEXT("Description"), Descriptor->Description);
        ProjectInfo->SetStringField(TEXT("Category"), Descriptor->Category);
        ProjectInfo->SetStringField(TEXT("EngineAssociation"), Descriptor->EngineAssociation);

        TArray<TSharedPtr<FJsonValue>> TargetPlatforms;
        for (const FName &Platform : Descriptor->TargetPlatforms)
        {
            TargetPlatforms.Add(MakeShared<FJsonValueString>(Platform.ToString()));
        }
        if (TargetPlatforms.Num() > 0)
        {
            ProjectInfo->SetArrayField(TEXT("TargetPlatforms"), TargetPlatforms);
        }
    }

    return ProjectInfo;
}

TSharedPtr<FJsonObject> FConvaiProjectInfoCollector::CollectConvaiPluginInfo() const
{
    TSharedPtr<FJsonObject> ConvaiInfo = MakeShared<FJsonObject>();

    TSharedPtr<IPlugin> ConvaiPlugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
    if (ConvaiPlugin.IsValid())
    {
        const FPluginDescriptor &Descriptor = ConvaiPlugin->GetDescriptor();

        ConvaiInfo->SetStringField(TEXT("Name"), ConvaiPlugin->GetName());
        ConvaiInfo->SetNumberField(TEXT("Version"), (double)Descriptor.Version);
        ConvaiInfo->SetStringField(TEXT("VersionName"), Descriptor.VersionName);
        ConvaiInfo->SetStringField(TEXT("FriendlyName"), ConvaiPlugin->GetFriendlyName());
        ConvaiInfo->SetStringField(TEXT("Description"), Descriptor.Description);
        ConvaiInfo->SetStringField(TEXT("Category"), Descriptor.Category);
        ConvaiInfo->SetStringField(TEXT("CreatedBy"), Descriptor.CreatedBy);
        ConvaiInfo->SetStringField(TEXT("CreatedByURL"), Descriptor.CreatedByURL);
        ConvaiInfo->SetStringField(TEXT("BaseDir"), ConvaiPlugin->GetBaseDir());
        ConvaiInfo->SetStringField(TEXT("ContentDir"), ConvaiPlugin->GetContentDir());
        ConvaiInfo->SetBoolField(TEXT("IsEnabled"), ConvaiPlugin->IsEnabled());
        ConvaiInfo->SetBoolField(TEXT("IsEnabledByDefault"), ConvaiPlugin->IsEnabledByDefault(false));

        TArray<TSharedPtr<FJsonValue>> Modules;
        for (const FModuleDescriptor &Module : Descriptor.Modules)
        {
            TSharedPtr<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
            ModuleObj->SetStringField(TEXT("Name"), Module.Name.ToString());

            FString ModuleType = TEXT("Unknown");
            switch (Module.Type)
            {
            case EHostType::Runtime:
                ModuleType = TEXT("Runtime");
                break;
            case EHostType::RuntimeNoCommandlet:
                ModuleType = TEXT("RuntimeNoCommandlet");
                break;
            case EHostType::RuntimeAndProgram:
                ModuleType = TEXT("RuntimeAndProgram");
                break;
            case EHostType::CookedOnly:
                ModuleType = TEXT("CookedOnly");
                break;
            case EHostType::Developer:
                ModuleType = TEXT("Developer");
                break;
            case EHostType::Editor:
                ModuleType = TEXT("Editor");
                break;
            case EHostType::EditorNoCommandlet:
                ModuleType = TEXT("EditorNoCommandlet");
                break;
            case EHostType::Program:
                ModuleType = TEXT("Program");
                break;
            case EHostType::ServerOnly:
                ModuleType = TEXT("ServerOnly");
                break;
            case EHostType::ClientOnly:
                ModuleType = TEXT("ClientOnly");
                break;
            }
            ModuleObj->SetStringField(TEXT("Type"), ModuleType);

            Modules.Add(MakeShared<FJsonValueObject>(ModuleObj));
        }
        if (Modules.Num() > 0)
        {
            ConvaiInfo->SetArrayField(TEXT("Modules"), Modules);
        }
    }
    else
    {
        ConvaiInfo->SetBoolField(TEXT("Found"), false);
    }

    return ConvaiInfo;
}

TArray<TSharedPtr<FJsonValue>> FConvaiProjectInfoCollector::CollectInstalledPlugins() const
{
    TArray<TSharedPtr<FJsonValue>> PluginsList;

    TArray<TSharedRef<IPlugin>> EnabledPlugins = IPluginManager::Get().GetEnabledPlugins();

    for (const TSharedRef<IPlugin> &Plugin : EnabledPlugins)
    {
        if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine)
        {
            continue;
        }

        TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();

        const FPluginDescriptor &Descriptor = Plugin->GetDescriptor();

        PluginObj->SetStringField(TEXT("Name"), Plugin->GetName());
        PluginObj->SetNumberField(TEXT("Version"), (double)Descriptor.Version);
        PluginObj->SetStringField(TEXT("VersionName"), Descriptor.VersionName);
        PluginObj->SetStringField(TEXT("FriendlyName"), Plugin->GetFriendlyName());
        PluginObj->SetBoolField(TEXT("IsEnabled"), Plugin->IsEnabled());

        FString LoadedFrom;
        switch (Plugin->GetLoadedFrom())
        {
        case EPluginLoadedFrom::Project:
            LoadedFrom = TEXT("Project");
            break;
        case EPluginLoadedFrom::Engine:
            LoadedFrom = TEXT("Engine");
            break;
        default:
            LoadedFrom = TEXT("Unknown");
            break;
        }
        PluginObj->SetStringField(TEXT("LoadedFrom"), LoadedFrom);

        PluginsList.Add(MakeShared<FJsonValueObject>(PluginObj));
    }

    return PluginsList;
}

TSharedPtr<FJsonObject> FConvaiProjectInfoCollector::CollectProjectSettings() const
{
    TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();

    Settings->SetStringField(TEXT("BuildDate"), FApp::GetBuildDate());

    Settings->SetStringField(TEXT("GameName"), FApp::GetProjectName());

    Settings->SetBoolField(TEXT("IsRunningOnBattery"), FPlatformMisc::IsRunningOnBattery());

    Settings->SetStringField(TEXT("TargetPlatform"), FPlatformProperties::IniPlatformName());

    return Settings;
}

FString FConvaiProjectInfoCollector::GetBuildConfiguration() const
{
#if UE_BUILD_DEBUG
    return TEXT("Debug");
#elif UE_BUILD_DEVELOPMENT
    return TEXT("Development");
#elif UE_BUILD_TEST
    return TEXT("Test");
#elif UE_BUILD_SHIPPING
    return TEXT("Shipping");
#else
    return TEXT("Unknown");
#endif
}
