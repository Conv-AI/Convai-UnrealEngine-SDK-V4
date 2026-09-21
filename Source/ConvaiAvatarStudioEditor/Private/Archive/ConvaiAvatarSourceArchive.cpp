// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Archive/ConvaiAvatarSourceArchive.h"
#include "Services/ConvaiAvatarMissingPackages.h"
#include "Services/ConvaiAvatarSourceMap.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Packaging/ConvaiAvatarPackaging.h"

namespace
{
	FCriticalSection HelperPathMutex;
	FString HelperPath;

	bool RunArchiveHelper(const TSharedRef<FJsonObject>& Request, TSharedPtr<FJsonObject>& Result, FString& Error)
	{
#if PLATFORM_WINDOWS
		FString Script;
		{
			FScopeLock Lock(&HelperPathMutex);
			Script = HelperPath;
		}
		if (Script.IsEmpty()) { Error = TEXT("The avatar archive helper was not initialized. Reopen Cloud Avatars and try again."); return false; }
		if (!IFileManager::Get().FileExists(*Script)) { Error = TEXT("The Cloud Avatars archive helper is missing. Reinstall the Convai plugin."); return false; }
		const FString Job = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/ArchiveJobs") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!IFileManager::Get().MakeDirectory(*Job, true)) { Error = TEXT("The archive job directory could not be created."); return false; }
		const FString RequestPath = Job / TEXT("request.json"), ResultPath = Job / TEXT("result.json");
		FString Json;
		FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Json));
		if (!FFileHelper::SaveStringToFile(Json, *RequestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { Error = TEXT("The archive request could not be saved."); return false; }
		const FString Executable = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")) / TEXT("System32/WindowsPowerShell/v1.0/powershell.exe");
		// All variable data travels in JSON. These three file paths are generated locally and cannot
		// contain Windows filename quotes; no archive-provided string becomes PowerShell source.
		const FString Arguments = FString::Printf(TEXT("-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy RemoteSigned -File \"%s\" -RequestPath \"%s\" -ResultPath \"%s\""), *Script, *RequestPath, *ResultPath);
		int32 ReturnCode = -1;
		FString StdOut, StdErr;
		const bool bRan = FPlatformProcess::ExecProcess(*Executable, *Arguments, &ReturnCode, &StdOut, &StdErr);
		FString ResultText;
		if (!FFileHelper::LoadFileToString(ResultText, *ResultPath))
		{
			Error = TEXT("The source archive helper could not finish. Check disk space and that Windows PowerShell can run the bundled local script.");
			return false;
		}
		if (!ConvaiAvatarSourceMap::ValidateJsonText(ResultText, Error)) return false;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultText), Result) || !Result)
		{
			Error = TEXT("The source archive helper returned an unreadable result.");
			return false;
		}
		bool bOk = false;
		Result->TryGetBoolField(TEXT("ok"), bOk);
		if (!bRan || ReturnCode != 0 || !bOk)
		{
			Result->TryGetStringField(TEXT("error"), Error);
			if (Error.IsEmpty()) Error = TEXT("The source archive could not be prepared or imported.");
			return false;
		}
		return true;
#else
		Error = TEXT("Cloud Avatars source archive preparation currently requires Windows.");
		return false;
#endif
	}
}

bool FConvaiAvatarSourceArchive::Initialize(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
	if (!Plugin) { OutError = TEXT("The Convai plugin directory could not be found."); return false; }
	const FString Script = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/AvatarStudio/Archive.ps1"));
	if (!IFileManager::Get().FileExists(*Script)) { OutError = TEXT("The Cloud Avatars archive helper is missing. Reinstall the Convai plugin."); return false; }
	FScopeLock Lock(&HelperPathMutex);
	HelperPath = Script;
	return true;
}

bool FConvaiAvatarSourceArchive::Create(const FConvaiAvatarPreparedAsset& Asset, const FString& ProxyUprojectPath,
	const FString& OutputZip, FString& OutError, const FString& ExpectedBaseManifestHash)
{
	OutError.Reset();
	if (!FConvaiAvatarWorkspace::IsSafePluginName(Asset.PluginName)) { OutError = TEXT("The avatar plugin name is invalid."); return false; }
	if (!ConvaiAvatarMissingPackages::Validate(Asset.AcknowledgedMissingPackages, Asset.PluginName, OutError)) return false;
	TMap<FName, FName> PortableMap = Asset.RetainedSourceToDestinationPackages;
	PortableMap.Append(Asset.SourceToDestinationPackages);
	// A diorama level is copied under a new name with regenerated external packages, so it has no portable source identity.
	if (Asset.Diorama.IsSet())
		for (auto It = PortableMap.CreateIterator(); It; ++It)
			if (It.Value() != FConvaiAvatarWorkspace::MakeDestinationPackage(It.Key(), Asset.PluginName)) It.RemoveCurrent();
	if (!ConvaiAvatarSourceMap::Validate(PortableMap, Asset.PluginName, OutError)) return false;
	const FString Mount = TEXT("/") + Asset.PluginName + TEXT("/");
	for (auto It = PortableMap.CreateIterator(); It; ++It)
		if (!IFileManager::Get().FileExists(*(Asset.PluginDirectory / TEXT("Content") / It.Value().ToString().Mid(Mount.Len()) + TEXT(".uasset")))) It.RemoveCurrent();
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("operation"), TEXT("create"));
	Request->SetStringField(TEXT("plugin_directory"), FPaths::ConvertRelativePathToFull(Asset.PluginDirectory));
	Request->SetStringField(TEXT("plugin_name"), Asset.PluginName);
	Request->SetStringField(TEXT("entry_point"), Asset.EntryPoint.ToString());
	Request->SetBoolField(TEXT("is_metahuman"), Asset.bIsMetaHuman);
	Request->SetBoolField(TEXT("include_convai_content"), Asset.bIncludeConvaiContent);
	ConvaiAvatarSourceMap::Write(*Request, TOptional<TMap<FName, FName>>(PortableMap));
	ConvaiAvatarMissingPackages::Write(*Request, TOptional<TArray<FName>>(Asset.AcknowledgedMissingPackages));
	Request->SetStringField(TEXT("proxy_uproject"), FPaths::ConvertRelativePathToFull(ProxyUprojectPath));
	Request->SetStringField(TEXT("portable_project_name"), FConvaiAvatarPackaging::GetRuntimeProjectName());
	Request->SetStringField(TEXT("output_zip"), FPaths::ConvertRelativePathToFull(OutputZip));
	Request->SetStringField(TEXT("expected_base_manifest_md5"), ExpectedBaseManifestHash);
	TArray<TSharedPtr<FJsonValue>> RequiredPlugins;
	for (const FString& Name : Asset.RequiredPlugins) RequiredPlugins.Add(MakeShared<FJsonValueString>(Name));
	Request->SetArrayField(TEXT("required_plugins"), RequiredPlugins);
	TSharedPtr<FJsonObject> Result;
	return RunArchiveHelper(Request, Result, OutError);
}

bool FConvaiAvatarSourceArchive::Import(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
	const FSoftObjectPath& EntryPoint, FString& OutInstalledDirectory, TArray<FString>& OutRequiredPlugins, FString& OutError, TOptional<bool>* OutMetaHumanChoice,
	TOptional<TArray<FName>>* OutAcknowledgedMissingPackages, TOptional<bool>* OutIncludeConvaiContent,
	TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages)
{
	return Stage(Zip, AssetId, MetadataPluginName, EntryPoint, FConvaiAvatarWorkspace::GetAvatarsDirectory(),
		OutInstalledDirectory, OutRequiredPlugins, OutError, nullptr, OutMetaHumanChoice, OutAcknowledgedMissingPackages, OutIncludeConvaiContent, OutSourceToDestinationPackages);
}

bool FConvaiAvatarSourceArchive::Stage(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
	const FSoftObjectPath& EntryPoint, const FString& StagingRoot, FString& OutInstalledDirectory, TArray<FString>& OutRequiredPlugins, FString& OutError,
	TMap<FString, FString>* OutSupportFiles, TOptional<bool>* OutMetaHumanChoice, TOptional<TArray<FName>>* OutAcknowledgedMissingPackages,
	TOptional<bool>* OutIncludeConvaiContent, TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages)
{
	OutInstalledDirectory.Reset(); OutRequiredPlugins.Reset(); OutError.Reset();
	if (OutSupportFiles) OutSupportFiles->Reset();
	if (OutMetaHumanChoice) OutMetaHumanChoice->Reset();
	if (OutAcknowledgedMissingPackages) OutAcknowledgedMissingPackages->Reset();
	if (OutIncludeConvaiContent) OutIncludeConvaiContent->Reset();
	if (OutSourceToDestinationPackages) OutSourceToDestinationPackages->Reset();
	if (AssetId.IsEmpty() || !FConvaiAvatarWorkspace::IsSafePluginName(MetadataPluginName)) { OutError = TEXT("Refresh this avatar before downloading it. Its plugin name or asset identity is missing."); return false; }
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("operation"), TEXT("import"));
	Request->SetStringField(TEXT("zip"), FPaths::ConvertRelativePathToFull(Zip));
	Request->SetStringField(TEXT("plugin_name"), MetadataPluginName);
	Request->SetStringField(TEXT("entry_point"), EntryPoint.ToString());
	Request->SetStringField(TEXT("destination_root"), FPaths::ConvertRelativePathToFull(StagingRoot));
	Request->SetBoolField(TEXT("stage_support"), OutSupportFiles != nullptr);
	Request->SetNumberField(TEXT("engine_major"), FEngineVersion::Current().GetMajor());
	Request->SetNumberField(TEXT("engine_minor"), FEngineVersion::Current().GetMinor());
	TSharedPtr<FJsonObject> Result;
	if (!RunArchiveHelper(Request, Result, OutError)) return false;
	TOptional<TArray<FName>> AcknowledgedMissingPackages;
	if (!ConvaiAvatarMissingPackages::Read(*Result, MetadataPluginName, AcknowledgedMissingPackages, OutError)) return false;
	TOptional<TMap<FName, FName>> SourceMap;
	if (!ConvaiAvatarSourceMap::Read(*Result, MetadataPluginName, SourceMap, OutError)) return false;
	const FString Expected = FPaths::ConvertRelativePathToFull(StagingRoot / MetadataPluginName);
	Result->TryGetStringField(TEXT("installed_directory"), OutInstalledDirectory);
	if (!FPaths::IsSamePath(Expected, OutInstalledDirectory)) { OutError = TEXT("The archive helper returned an unexpected installation directory."); OutInstalledDirectory.Reset(); return false; }
	Result->TryGetStringArrayField(TEXT("required_plugins"), OutRequiredPlugins);
	if (const auto Choice = Result->TryGetField(TEXT("is_metahuman")))
	{
		if (Choice->Type != EJson::Boolean) { OutError = TEXT("The source archive returned an invalid MetaHuman choice."); return false; }
		if (OutMetaHumanChoice) *OutMetaHumanChoice = Choice->AsBool();
	}
	if (OutSupportFiles)
	{
		const TSharedPtr<FJsonObject>* Files = nullptr;
		if (Result->TryGetObjectField(TEXT("support_files"), Files))
			for (const auto& Pair : (*Files)->Values)
			{
				FString Hash;
				if (!Pair.Value->TryGetString(Hash)) { OutError = TEXT("The source archive returned an invalid support file record."); return false; }
				OutSupportFiles->Add(FString(*Pair.Key), MoveTemp(Hash));
			}
	}
	if (const auto Choice = Result->TryGetField(TEXT("include_convai_content")))
	{
		if (Choice->Type != EJson::Boolean) { OutError = TEXT("The source archive returned an invalid Convai content choice."); return false; }
		if (OutIncludeConvaiContent) *OutIncludeConvaiContent = Choice->AsBool();
	}
	if (OutAcknowledgedMissingPackages) *OutAcknowledgedMissingPackages = MoveTemp(AcknowledgedMissingPackages);
	if (OutSourceToDestinationPackages) *OutSourceToDestinationPackages = MoveTemp(SourceMap);
	return true;
}
