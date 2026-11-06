// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Archive/ConvaiAvatarSourceArchive.h"
#include "Services/ConvaiAvatarNativeRuntimeContent.h"
#include "Archive/ConvaiAvatarSourceArchivePrivate.h"
#include "ConvaiAvatarErrorPresentation.h"
#include "Services/ConvaiAvatarMissingPackages.h"
#include "Services/ConvaiAvatarSourceMap.h"
#include "Packaging/ConvaiAvatarProcess.h"

#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Packaging/ConvaiAvatarPackaging.h"

namespace
{
	FCriticalSection HelperPathMutex;
	FString HelperPath;

	bool RunArchiveHelper(const TSharedRef<FJsonObject>& Request, TSharedPtr<FJsonObject>& Result, FString& Error, const std::atomic<bool>& Cancelled)
	{
		FString Script;
		{
			FScopeLock Lock(&HelperPathMutex);
			Script = HelperPath;
		}
		if (Script.IsEmpty()) { Error = TEXT("The avatar archive helper was not initialized. Reopen Cloud Avatars and try again."); return false; }
		return ConvaiAvatarSourceArchivePrivate::RunHelper(Script, Request, Result, Error, Cancelled);
	}

	/** The redacted last 4 KB of a helper's output. The cut moves to the next whitespace first: a URL cut in half would lose
	 * the scheme that redaction looks for. */
	FString ArchiveHelperOutputTail(const FString& Output)
	{
		FString Tail = Output.Right(4096);
		if (Tail.Len() < Output.Len())
		{
			int32 Start = 0;
			while (Start < Tail.Len() && !FChar::IsWhitespace(Tail[Start])) ++Start;
			Tail = Tail.Mid(Start);
		}
		return ConvaiAvatarErrorPresentation::RedactDiagnostic(Tail.TrimStartAndEnd(), FString());
	}

	/** Deletes <OutputZip>.<32 hex digits>.part, the temporary Archive.ps1 renames to OutputZip on success. Its finally
	 * block would delete it, but a job kill skips that. */
	void DeleteArchiveHelperTemporaries(const FString& OutputZip)
	{
		const FString Directory = FPaths::GetPath(OutputZip), Prefix = FPaths::GetCleanFilename(OutputZip) + TEXT(".");
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Directory / (Prefix + TEXT("*.part"))), true, false);
		for (const FString& Name : Names)
		{
			const FString Middle = Name.Mid(Prefix.Len(), Name.Len() - Prefix.Len() - 5);
			if (Name.StartsWith(Prefix) && Name.EndsWith(TEXT(".part")) && Middle.Len() == 32 && Algo::AllOf(Middle, [](TCHAR C) { return FChar::IsHexDigit(C); }))
				IFileManager::Get().Delete(*(Directory / Name), false, false, true);
		}
	}
}

namespace ConvaiAvatarSourceArchivePrivate
{
	FString HelperArguments(const FString& Script, const FString& RequestPath, const FString& ResultPath)
	{
		// Bypass, as every other helper runs: Explorer marks each file of a browser-downloaded SDK ZIP as from the internet,
		// and RemoteSigned then refuses this unsigned script. It is the plugin's own file; requests carry only data.
		return FString::Printf(TEXT("-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"%s\" -RequestPath \"%s\" -ResultPath \"%s\""), *Script, *RequestPath, *ResultPath);
	}

	bool RunHelper(const FString& Script, const TSharedRef<FJsonObject>& Request, TSharedPtr<FJsonObject>& Result, FString& Error,
		const std::atomic<bool>& Cancelled)
	{
#if PLATFORM_WINDOWS
		if (!IFileManager::Get().FileExists(*Script)) { Error = TEXT("The Cloud Avatars archive helper is missing. Reinstall the Convai plugin."); return false; }
		if (Cancelled.load()) { Error = TEXT("Source archive processing cancelled."); return false; }
		const FString Job = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/ArchiveJobs") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!IFileManager::Get().MakeDirectory(*Job, true)) { Error = TEXT("The archive job directory could not be created."); return false; }
		const FString RequestPath = Job / TEXT("request.json"), ResultPath = Job / TEXT("result.json");
		// The helper's whole job has ended by the time RunToExit returns, so nothing holds these files on any exit path.
		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*RequestPath, false, false, true);
			IFileManager::Get().Delete(*ResultPath, false, false, true);
			IFileManager::Get().DeleteDirectory(*Job, false, false);
		};
		FString Json;
		FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Json));
		if (!FFileHelper::SaveStringToFile(Json, *RequestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { Error = TEXT("The archive request could not be saved."); return false; }
		const FString Executable = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")) / TEXT("System32/WindowsPowerShell/v1.0/powershell.exe");
		const TCHAR* CouldNotFinish = TEXT("The source archive helper could not finish. Check disk space and that Windows PowerShell can run the bundled local script.");
		int32 ReturnCode = -1;
		TArray<uint8> Output;
		// The transfers' ceiling: no archive up to the 10,000 MiB upload cap needs longer, and a helper stalled on a locked
		// file cannot keep the Studio busy for the rest of the session.
		const EConvaiAvatarProcessEnd End = FConvaiAvatarProcess::RunToExit(Executable, HelperArguments(Script, RequestPath, ResultPath), Cancelled,
			6 * 60 * 60 + 120, ReturnCode, Error, [&Output](const TArray<uint8>& Bytes)
			{
				Output.Append(Bytes);
				if (Output.Num() > 64 * 1024) Output.RemoveAt(0, Output.Num() - 64 * 1024);
			});
		if (End == EConvaiAvatarProcessEnd::Cancelled || End == EConvaiAvatarProcessEnd::TimedOut)
		{
			FString OutputZip;
			if (Request->TryGetStringField(TEXT("output_zip"), OutputZip)) DeleteArchiveHelperTemporaries(OutputZip);
			Error = End == EConvaiAvatarProcessEnd::Cancelled ? TEXT("Source archive processing cancelled.")
				: TEXT("The source archive helper did not finish within 6 hours and was stopped. Check disk space and the project folder, then retry.");
			return false;
		}
		if (End == EConvaiAvatarProcessEnd::NotStarted)
		{
			UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars archive helper did not start: %s"), Error.IsEmpty() ? TEXT("no output pipe") : *Error);
			Error = CouldNotFinish;
			return false;
		}
		FString ResultText;
		if (!FFileHelper::LoadFileToString(ResultText, *ResultPath))
		{
			// Child output never reaches the UI; this log line is the only record of why PowerShell wrote no result.
			UE_LOG(LogTemp, Warning, TEXT("Cloud Avatars archive helper wrote no result: exit_code=%d output=%s"), ReturnCode,
				*ArchiveHelperOutputTail(FString(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Output.GetData()), Output.Num()))));
			Error = CouldNotFinish;
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
		if (ReturnCode != 0 || !bOk)
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
	static const std::atomic<bool> NeverCancelled{false};
	return ConvaiAvatarSourceArchivePrivate::Create(Asset, ProxyUprojectPath, OutputZip, OutError, ExpectedBaseManifestHash, NeverCancelled);
}

bool ConvaiAvatarSourceArchivePrivate::Create(const FConvaiAvatarPreparedAsset& Asset, const FString& ProxyUprojectPath,
	const FString& OutputZip, FString& OutError, const FString& ExpectedBaseManifestHash, const std::atomic<bool>& Cancelled)
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
	if(!Asset.NativeRuntimeContract.IsEmpty()) Request->SetStringField(TEXT("native_runtime_contract"),Asset.NativeRuntimeContract);
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
	return RunArchiveHelper(Request, Result, OutError, Cancelled);
}

bool FConvaiAvatarSourceArchive::Import(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
	const FSoftObjectPath& EntryPoint, FString& OutInstalledDirectory, TArray<FString>& OutRequiredPlugins, FString& OutError, TOptional<bool>* OutMetaHumanChoice,
	TOptional<TArray<FName>>* OutAcknowledgedMissingPackages, TOptional<bool>* OutIncludeConvaiContent,
	TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages, FString* OutNativeRuntimeContract)
{
	return Stage(Zip, AssetId, MetadataPluginName, EntryPoint, FConvaiAvatarWorkspace::GetAvatarsDirectory(),
		OutInstalledDirectory, OutRequiredPlugins, OutError, nullptr, OutMetaHumanChoice, OutAcknowledgedMissingPackages, OutIncludeConvaiContent, OutSourceToDestinationPackages, OutNativeRuntimeContract);
}

bool FConvaiAvatarSourceArchive::Stage(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
	const FSoftObjectPath& EntryPoint, const FString& StagingRoot, FString& OutInstalledDirectory, TArray<FString>& OutRequiredPlugins, FString& OutError,
	TMap<FString, FString>* OutSupportFiles, TOptional<bool>* OutMetaHumanChoice, TOptional<TArray<FName>>* OutAcknowledgedMissingPackages,
	TOptional<bool>* OutIncludeConvaiContent, TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages, FString* OutNativeRuntimeContract)
{
	static const std::atomic<bool> NeverCancelled{false};
	return ConvaiAvatarSourceArchivePrivate::Stage(Zip, AssetId, MetadataPluginName, EntryPoint, StagingRoot, OutInstalledDirectory, OutRequiredPlugins, OutError,
		OutSupportFiles, OutMetaHumanChoice, OutAcknowledgedMissingPackages, OutIncludeConvaiContent, OutSourceToDestinationPackages, NeverCancelled, OutNativeRuntimeContract);
}

bool ConvaiAvatarSourceArchivePrivate::Stage(const FString& Zip, const FString& AssetId, const FString& MetadataPluginName,
	const FSoftObjectPath& EntryPoint, const FString& StagingRoot, FString& OutInstalledDirectory, TArray<FString>& OutRequiredPlugins, FString& OutError,
	TMap<FString, FString>* OutSupportFiles, TOptional<bool>* OutMetaHumanChoice, TOptional<TArray<FName>>* OutAcknowledgedMissingPackages,
	TOptional<bool>* OutIncludeConvaiContent, TOptional<TMap<FName, FName>>* OutSourceToDestinationPackages, const std::atomic<bool>& Cancelled, FString* OutNativeRuntimeContract)
{
	OutInstalledDirectory.Reset(); OutRequiredPlugins.Reset(); OutError.Reset();
	if (OutSupportFiles) OutSupportFiles->Reset();
	if (OutMetaHumanChoice) OutMetaHumanChoice->Reset();
	if (OutAcknowledgedMissingPackages) OutAcknowledgedMissingPackages->Reset();
	if (OutIncludeConvaiContent) OutIncludeConvaiContent->Reset();
	if (OutSourceToDestinationPackages) OutSourceToDestinationPackages->Reset();
	if (OutNativeRuntimeContract) OutNativeRuntimeContract->Reset();
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
	if (!RunArchiveHelper(Request, Result, OutError, Cancelled)) return false;
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
	FString NativeContract;
    if (const auto Field = Result->TryGetField(TEXT("native_runtime_contract")))
    {
        if (Field->Type != EJson::String || !Field->TryGetString(NativeContract) || !ConvaiAvatarNativeRuntimeContent::IsValidRequirement(NativeContract))
        { OutError=TEXT("The source requires unsupported groom runtime content."); return false; }
    }
    if (OutNativeRuntimeContract) *OutNativeRuntimeContract = MoveTemp(NativeContract);
	if (OutAcknowledgedMissingPackages) *OutAcknowledgedMissingPackages = MoveTemp(AcknowledgedMissingPackages);
	if (OutSourceToDestinationPackages) *OutSourceToDestinationPackages = MoveTemp(SourceMap);
	return true;
}
