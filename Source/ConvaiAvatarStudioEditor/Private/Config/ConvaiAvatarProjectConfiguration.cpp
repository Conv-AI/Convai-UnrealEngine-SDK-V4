// Copyright Convai Inc. All Rights Reserved.
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "Workspace/ConvaiAvatarUploaderWorkspace.h"

#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/ProjectPackagingSettings.h"
#include "UObject/UnrealType.h"
#include "Windows/WindowsHWrapper.h"

namespace AvatarConfiguration
{
const TCHAR* SourceUrl = TEXT("https://github.com/Conv-AI/Convai-UnrealEngine-ModdingTool/blob/5133b6072915d888d5b5bc1c78683f44287361e3/core/unreal_engine_manager.py");
const TCHAR* ProfileId = TEXT("avatar-studio-rendering-packaging-5133b60-v1");
const TCHAR* JournalRelative = TEXT("Saved/ConvaiAvatarStudio/Configuration/active.json");

struct FSetting
{
	FString File, Section, Key;
	TArray<FString> Lines;
};
struct FDocument
{
	FString File, Text;
	TArray<uint8> Before, After;
	bool bExisted = false;
	bool bDelete = false;
};
struct FAnalysis
{
	FConvaiAvatarConfigurationPlan Plan;
	TArray<FSetting> Settings;
	TArray<FDocument> Documents;
	TSharedPtr<FJsonObject> Journal;
};

TArray<FSetting> Profile()
{
	TArray<FSetting> Result;
	auto Add = [&Result](const TCHAR* File, const TCHAR* Section, const TCHAR* Key, const TCHAR* Value)
	{
		FSetting Setting; Setting.File = File; Setting.Section = Section; Setting.Key = Key;
		Setting.Lines.Add(Setting.Key + TEXT("=") + Value); Result.Add(MoveTemp(Setting));
	};
	const TCHAR* Engine = TEXT("DefaultEngine.ini");
	const TCHAR* Renderer = TEXT("/Script/Engine.RendererSettings");
	// Reviewed values from the pinned ModdingTool renderer template. Unrelated legacy defaults are excluded.
	Add(Engine, Renderer, TEXT("r.GenerateMeshDistanceFields"), TEXT("True"));
	Add(Engine, Renderer, TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("0"));
	Add(Engine, Renderer, TEXT("r.ReflectionMethod"), TEXT("2"));
	Add(Engine, Renderer, TEXT("r.RayTracing"), TEXT("False"));
	Add(Engine, Renderer, TEXT("r.Shadow.Virtual.Enable"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.GPUSkin.Support16BitBoneIndex"), TEXT("True"));
	Add(Engine, Renderer, TEXT("r.GPUSkin.UnlimitedBoneInfluences"), TEXT("True"));
	Add(Engine, Renderer, TEXT("SkeletalMesh.UseExperimentalChunking"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.VirtualTextures"), TEXT("True"));
	Add(Engine, Renderer, TEXT("r.SkinCache.CompileShaders"), TEXT("True"));
	Add(Engine, Renderer, TEXT("r.SkinCache.BlendUsingVertexColorForRecomputeTangents"), TEXT("2"));
	Add(Engine, Renderer, TEXT("r.SkinCache.SceneMemoryLimitInMB"), TEXT("1500.000000"));
	Add(Engine, Renderer, TEXT("r.SkinCache.DefaultBehavior"), TEXT("0"));
	Add(Engine, Renderer, TEXT("r.HairStrands.SkyLighting"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.SkyAO"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Visibility.MaterialPass"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Visibility.FullCoverageThreshold"), TEXT("0.9"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Visibility.MSAA.MeanSamplePerPixel"), TEXT("1.0"));
	Add(Engine, Renderer, TEXT("r.HairStrands.RasterizationScale"), TEXT("1.0"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.DensityScale"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.DepthBiasScale_Shadow"), TEXT("2"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.DepthBiasScale_Light"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.DepthBiasScale_Environment"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.GPUDriven"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.Virtual.VoxelWorldSize"), TEXT("0.15"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.Virtual.VoxelPageCountPerDim"), TEXT("9"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.Raymarching.SteppingScale"), TEXT("1.0"));
	Add(Engine, Renderer, TEXT("r.HairStrands.Voxelization.Raymarching.SteppingScale.Shadow"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.DeepShadow.RandomType"), TEXT("2"));
	Add(Engine, Renderer, TEXT("r.HairStrands.ComposeAfterTranslucency"), TEXT("0"));
	Add(Engine, Renderer, TEXT("r.HairStrands.AsyncLoad"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.BindingAsyncLoad"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.SimulationRestUpdate"), TEXT("1"));
	Add(Engine, Renderer, TEXT("r.HairStrands.MaxSimulatedLOD"), TEXT("0"));
	Add(Engine, Renderer, TEXT("r.HairStrands.LODMode"), TEXT("False"));
	Add(Engine, Renderer, TEXT("r.ReflectionCaptureResolution"), TEXT("512"));
	Add(Engine, Renderer, TEXT("r.AllowStaticLighting"), TEXT("True"));
	Add(Engine, TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings"), TEXT("DefaultGraphicsRHI"), TEXT("DefaultGraphicsRHI_DX12"));
	FSetting Shaders; Shaders.File = Engine; Shaders.Section = TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings"); Shaders.Key = TEXT("D3D12TargetedShaderFormats");
	Shaders.Lines = { TEXT("-D3D12TargetedShaderFormats=PCD3D_SM5"), TEXT("+D3D12TargetedShaderFormats=PCD3D_SM6") }; Result.Add(MoveTemp(Shaders));
	const TCHAR* Packaging = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
	Add(TEXT("DefaultGame.ini"), Packaging, TEXT("bUseIoStore"), TEXT("False"));
	Add(TEXT("DefaultGame.ini"), Packaging, TEXT("bGenerateChunks"), TEXT("True"));
	Add(TEXT("DefaultGame.ini"), Packaging, TEXT("bShareMaterialShaderCode"), TEXT("False"));
	Add(TEXT("DefaultGame.ini"), Packaging, TEXT("UsePakFile"), TEXT("True"));
	return Result;
}

FString Full(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path); FPaths::NormalizeDirectoryName(Path); FPaths::CollapseRelativeDirectories(Path); return Path;
}
bool Redirected(const FString& Path)
{
	const DWORD Attributes = GetFileAttributesW(*Path); return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
bool SafePath(const FString& Root, const FString& Relative, FString& Error, bool bCreateParents = false)
{
	const FString Target = Full(Root / Relative);
	if (!Target.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase) || Redirected(Root)) { Error = TEXT("Configuration paths must stay in a regular project directory."); return false; }
	TArray<FString> Parts; Relative.ParseIntoArray(Parts, TEXT("/"), true);
	FString Current = Root;
	for (int32 I = 0; I < Parts.Num(); ++I)
	{
		Current /= Parts[I];
		if (Parts[I] == TEXT("..") || Redirected(Current)) { Error = TEXT("Configuration cannot write through a linked file or folder: ") + Current; return false; }
		if (I + 1 < Parts.Num() && bCreateParents && !IFileManager::Get().DirectoryExists(*Current) && !IFileManager::Get().MakeDirectory(*Current, false))
		{ Error = TEXT("Could not create configuration backup directory: ") + Current; return false; }
	}
	return true;
}
bool ProjectRoot(const FString& Input, FString& Root, FString& Error)
{
	Root = Full(Input);
	TArray<FString> Projects; IFileManager::Get().FindFiles(Projects, *(Root / TEXT("*.uproject")), true, false);
	if (Projects.Num() != 1 || Redirected(Root)) { Error = TEXT("Choose a project directory containing exactly one .uproject file."); return false; }
	return true;
}
FString Hash(const TArray<uint8>& Bytes)
{
	FMD5 Md5; if (!Bytes.IsEmpty()) Md5.Update(Bytes.GetData(), Bytes.Num()); uint8 Digest[16]; Md5.Final(Digest); return BytesToHex(Digest, 16);
}
bool Decode(const TArray<uint8>& Bytes, FString& Text, FString& Error)
{
	if (Bytes.Num() > 4 * 1024 * 1024) { Error = TEXT("Configuration files larger than 4 MiB must be reviewed manually."); return false; }
	if (Bytes.Num() >= 2 && Bytes[0] == 0xfe && Bytes[1] == 0xff) { Error = TEXT("UTF-16 big-endian configuration is not supported. Save the file as UTF-8 before applying this profile."); return false; }
	if (!Bytes.IsEmpty()) FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num()); else Text.Empty();
	return true;
}
TArray<uint8> Encode(const FString& Text, const TArray<uint8>& Original)
{
	TArray<uint8> Result;
	if (Original.Num() >= 2 && Original[0] == 0xff && Original[1] == 0xfe)
	{
		Result.Add(0xff); Result.Add(0xfe);
		for (TCHAR C : Text) { Result.Add(static_cast<uint8>(C & 255)); Result.Add(static_cast<uint8>((C >> 8) & 255)); }
	}
	else
	{
		if (Original.Num() >= 3 && Original[0] == 0xef && Original[1] == 0xbb && Original[2] == 0xbf) { Result.Append(Original.GetData(), 3); }
		FTCHARToUTF8 Utf8(*Text); Result.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
	return Result;
}
TArray<FString> Lines(const FString& Text)
{
	TArray<FString> Result; int32 Start = 0;
	for (int32 I = 0; I < Text.Len(); ++I) if (Text[I] == TEXT('\n')) { Result.Add(Text.Mid(Start, I - Start + 1)); Start = I + 1; }
	if (Start < Text.Len()) Result.Add(Text.Mid(Start)); return Result;
}
bool ParseSection(const FString& Line, FString& Section)
{
	const FString Trimmed = Line.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]"))) { Section = Trimmed.Mid(1, Trimmed.Len() - 2); return true; } return false;
}
bool MatchesKey(const FString& Line, const FString& Key)
{
	FString Trimmed = Line.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT(";")) || Trimmed.StartsWith(TEXT("#"))) return false;
	int32 Equal = INDEX_NONE; if (!Trimmed.FindChar(TEXT('='), Equal)) return false;
	FString Name = Trimmed.Left(Equal).TrimStartAndEnd();
	if (!Name.IsEmpty() && (Name[0] == TEXT('+') || Name[0] == TEXT('-') || Name[0] == TEXT('!') || Name[0] == TEXT('.'))) Name.RightChopInline(1);
	return Name.Equals(Key, ESearchCase::IgnoreCase);
}
TArray<FString> Gather(const FString& Text, const FSetting& Setting, bool bRaw)
{
	TArray<FString> Result; FString Section;
	for (const FString& Line : Lines(Text))
	{
		if (ParseSection(Line, Section)) continue;
		if (Section.Equals(Setting.Section, ESearchCase::IgnoreCase) && MatchesKey(Line, Setting.Key)) Result.Add(bRaw ? Line : Line.TrimStartAndEnd());
	}
	return Result;
}
FString Patch(const FString& Text, const FSetting& Setting, const TArray<FString>& Replacement, bool bRaw)
{
	const FString Newline = Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
	TArray<FString> Content = Lines(Text); TArray<FString> Output; FString Section;
	bool bInserted = false; int32 SectionInsert = INDEX_NONE;
	auto Insert = [&]
	{
		for (const FString& Value : Replacement) Output.Add(bRaw ? Value : Value + Newline);
		bInserted = true;
	};
	for (const FString& Line : Content)
	{
		if (ParseSection(Line, Section))
		{
			Output.Add(Line);
			if (Section.Equals(Setting.Section, ESearchCase::IgnoreCase) && SectionInsert == INDEX_NONE) SectionInsert = Output.Num();
			continue;
		}
		if (Section.Equals(Setting.Section, ESearchCase::IgnoreCase) && MatchesKey(Line, Setting.Key)) { if (!bInserted) Insert(); }
		else Output.Add(Line);
	}
	if (!bInserted && !Replacement.IsEmpty())
	{
		TArray<FString> Additions;
		for (const FString& Value : Replacement) Additions.Add(bRaw ? Value : Value + Newline);
		if (SectionInsert != INDEX_NONE)
		{
			if (!Output[SectionInsert - 1].EndsWith(TEXT("\n"))) Output[SectionInsert - 1] += Newline;
			Output.Insert(Additions, SectionInsert);
		}
		else
		{
			if (!Output.IsEmpty() && !Output.Last().EndsWith(TEXT("\n"))) Output.Last() += Newline;
			Output.Add(Newline + TEXT("[") + Setting.Section + TEXT("]") + Newline); Output.Append(Additions);
		}
	}
	// A restored original final line may have had no newline. Keep later unrelated
	// edits on separate lines when that key is no longer the end of the file.
	for (int32 I = 0; I + 1 < Output.Num(); ++I) if (!Output[I].EndsWith(TEXT("\n"))) Output[I] += Newline;
	return FString::Join(Output, TEXT(""));
}
bool ReadDocument(const FString& Root, const FString& File, FDocument& Document, FString& Error)
{
	if (File != TEXT("DefaultEngine.ini") && File != TEXT("DefaultGame.ini")) { Error = TEXT("Unsupported configuration file."); return false; }
	if (!SafePath(Root, TEXT("Config/") + File, Error)) return false;
	Document.File = File; const FString Path = Root / TEXT("Config") / File;
	Document.bExisted = IFileManager::Get().FileExists(*Path);
	if (Document.bExisted && !FFileHelper::LoadFileToArray(Document.Before, *Path)) { Error = TEXT("Could not read ") + Path; return false; }
	if (!Decode(Document.Before, Document.Text, Error)) return false;
	if (Encode(Document.Text, Document.Before) != Document.Before) { Error = TEXT("Save ") + File + TEXT(" as UTF-8 or UTF-16 little-endian before applying this profile. Its current encoding cannot be preserved safely."); return false; }
	return true;
}
TArray<TSharedPtr<FJsonValue>> JsonStrings(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Result; for (const FString& String : Strings) Result.Add(MakeShared<FJsonValueString>(String)); return Result;
}
bool ReadStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<FString>& Values)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Object->TryGetArrayField(Field, Array)) return false;
	for (const auto& Item : *Array) { FString Value; if (!Item || !Item->TryGetString(Value)) return false; Values.Add(Value); }
	return true;
}
bool CompressionArguments(const TSharedPtr<FJsonObject>& Json, const FString& Configuration, FString& Arguments, FString& Error)
{
	Arguments = TEXT(" -compressionformats=Zlib");
	const TSharedPtr<FJsonObject>* Compression = nullptr;
	if (!Json || !Json->HasField(TEXT("compression"))) return true;
	FString Format;
	if (!Json->TryGetObjectField(TEXT("compression"), Compression) || !(*Compression)->TryGetStringField(TEXT("format"), Format))
	{ Error = TEXT("The profile's compression format is missing."); return false; }
	if (Format == TEXT("Zlib"))
	{
		if ((*Compression)->Values.Num() != 1) { Error = TEXT("Zlib does not accept the profile's extra compression options."); return false; }
		return true;
	}
	FString Method; double Development = 0, Shipping = 0;
	if (Format != TEXT("Oodle") || (*Compression)->Values.Num() != 4 ||
		!(*Compression)->TryGetStringField(TEXT("method"), Method) ||
		(Method != TEXT("Kraken") && Method != TEXT("Mermaid") && Method != TEXT("Selkie") && Method != TEXT("Leviathan")) ||
		!(*Compression)->TryGetNumberField(TEXT("development_level"), Development) ||
		!(*Compression)->TryGetNumberField(TEXT("shipping_level"), Shipping) ||
		!FMath::IsFinite(Development) || !FMath::IsFinite(Shipping) || Development < 1 || Development > 9 || Shipping < 1 || Shipping > 9 ||
		Development != FMath::FloorToDouble(Development) || Shipping != FMath::FloorToDouble(Shipping))
	{ Error = TEXT("The profile contains an unsupported compression format, method, or level."); return false; }
	const int32 Level = static_cast<int32>((Configuration == TEXT("Shipping") || Configuration == TEXT("Test")) ? Shipping : Development);
	Arguments = FString::Printf(TEXT(" -compressionformats=Oodle -compressmethod=%s -compresslevel=%d"), *Method, Level);
	return true;
}
bool ReadProfile(const FString& Json, TArray<FSetting>& Settings, FString& Error, FConvaiAvatarConfigurationPlan* Plan = nullptr)
{
	if (Json.IsEmpty())
	{
		Settings = Profile();
		if (Plan) Plan->Notes.Add(TEXT("Using the bundled reviewed profile. A remote profile update has not been applied."));
		return true;
	}
	if (!FConvaiAvatarProjectConfiguration::ValidateProfile(Json, Error)) return false;
	TSharedPtr<FJsonObject> Root;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
	Settings.Reset();
	for (const auto& Value : Root->GetArrayField(TEXT("settings")))
	{
		const auto Object = Value->AsObject();
		FSetting Setting; Setting.File = Object->GetStringField(TEXT("file")); Setting.Section = Object->GetStringField(TEXT("section"));
		Setting.Key = Object->GetStringField(TEXT("key")); ReadStrings(Object, TEXT("values"), Setting.Lines); Settings.Add(MoveTemp(Setting));
	}
	if (Plan)
	{
		Plan->ProfileJson = Json;
		Root->TryGetStringField(TEXT("profile_id"), Plan->ProfileId);
		Root->TryGetStringField(TEXT("source_url"), Plan->SourceUrl);
		Plan->Notes.Add(TEXT("This preview uses the captured declarative profile. Apply and restore keep this exact snapshot."));
	}
	return true;
}
bool ReadJournal(const FString& Root, TSharedPtr<FJsonObject>& Journal, FString& Error)
{
	if (!SafePath(Root, JournalRelative, Error)) return false;
	const FString Path = Root / JournalRelative;
	if (!IFileManager::Get().FileExists(*Path)) return true;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path) || Text.Len() > 16 * 1024 * 1024 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Journal) || !Journal)
	{ Error = TEXT("The configuration backup journal could not be read. Keep the backup and restore it manually."); return false; }
	FString ProfileName, StoredRoot;
	if (!Journal->TryGetStringField(TEXT("profile"), ProfileName) || ProfileName != ProfileId || !Journal->TryGetStringField(TEXT("project"), StoredRoot) || !StoredRoot.Equals(Root, ESearchCase::IgnoreCase))
	{ Error = TEXT("The configuration backup belongs to another profile or project."); return false; }
	return true;
}
FString Revision(const FAnalysis& Analysis)
{
	FString Data = Analysis.Plan.ProjectDirectory + Analysis.Plan.ProfileJson + Analysis.Plan.SourceUrl + ProfileId + (Analysis.Plan.bRestore ? TEXT("restore") : TEXT("apply"));
	for (const FDocument& Doc : Analysis.Documents) Data += Doc.File + (Doc.bExisted ? TEXT("exists") : TEXT("missing")) + Hash(Doc.Before);
	if (Analysis.Journal) { FString Json; FJsonSerializer::Serialize(Analysis.Journal.ToSharedRef(), TJsonWriterFactory<>::Create(&Json)); Data += Json; }
	return FMD5::HashAnsiString(*Data);
}
void InitPlan(FConvaiAvatarConfigurationPlan& Plan, const FString& Root, bool bRestore)
{
	Plan.ProjectDirectory = Root; Plan.SourceUrl = SourceUrl; Plan.ProfileId = ProfileId; Plan.bRestore = bRestore;
	Plan.Notes.Add(TEXT("This is a selected rendering and packaging profile, not the full Avatar Studio project configuration."));
	Plan.Notes.Add(TEXT("Restart Unreal after applying or restoring. Shader compilation may be required."));
	Plan.Notes.Add(TEXT("GameMode, input, collision, network verification, credentials, plugins, engine files, and build-tool settings are excluded."));
}
bool AnalyzeApply(const FString& InputRoot, FAnalysis& Analysis, FString& Error, const FString& ProfileJson = FString(), const FString& ProfileSourceUrl = FString(), const FString& ProfileNotice = FString())
{
	FString Root; if (!ProjectRoot(InputRoot, Root, Error)) return false;
	InitPlan(Analysis.Plan, Root, false);
	if (!ReadProfile(ProfileJson, Analysis.Settings, Error, &Analysis.Plan)) return false;
	if (!ProfileSourceUrl.IsEmpty()) Analysis.Plan.SourceUrl = ProfileSourceUrl;
	if (!ProfileNotice.IsEmpty()) Analysis.Plan.Notes.Add(ProfileNotice);
	if (!ReadJournal(Root, Analysis.Journal, Error)) return false;
	Analysis.Plan.bHasBackup = Analysis.Journal.IsValid();
	for (const FString& File : { FString(TEXT("DefaultEngine.ini")), FString(TEXT("DefaultGame.ini")) })
	{
		FDocument Doc; if (!ReadDocument(Root, File, Doc, Error)) return false;
		const FString OriginalText = Doc.Text;
		for (const FSetting& Setting : Analysis.Settings)
		{
			if (Setting.File != File) continue;
			const auto Before = Gather(OriginalText, Setting, false);
			if (Before == Setting.Lines) continue;
			FConvaiAvatarConfigurationChange Change; Change.FileName = File; Change.Section = Setting.Section; Change.Key = Setting.Key; Change.Before = Before; Change.After = Setting.Lines;
			Analysis.Plan.Changes.Add(MoveTemp(Change)); Doc.Text = Patch(Doc.Text, Setting, Setting.Lines, false);
		}
		Doc.After = Encode(Doc.Text, Doc.Before); Analysis.Documents.Add(MoveTemp(Doc));
	}
	Analysis.Plan.bAlreadyApplied = Analysis.Plan.Changes.IsEmpty(); Analysis.Plan.Revision = Revision(Analysis);
	return true;
}
bool AtomicWrite(const FString& Path, const TArray<uint8>& Bytes, FString& Error)
{
	if (Redirected(Path) || IFileManager::Get().IsReadOnly(*Path)) { Error = TEXT("The configuration file is linked or read-only: ") + Path; return false; }
	const FString Temp = Path + TEXT(".convai-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	if (!FFileHelper::SaveArrayToFile(Bytes, *Temp)) { Error = TEXT("Could not write the temporary configuration file."); return false; }
	const bool bExists = IFileManager::Get().FileExists(*Path);
	const bool bSuccess = bExists ? !!ReplaceFileW(*Path, *Temp, nullptr, 0, nullptr, nullptr) : !!MoveFileExW(*Temp, *Path, MOVEFILE_WRITE_THROUGH);
	if (!bSuccess) { IFileManager::Get().Delete(*Temp, false, false, true); Error = TEXT("Could not replace configuration file; check write access: ") + Path; }
	return bSuccess;
}
bool WriteJournal(const FString& Root, const TSharedRef<FJsonObject>& Journal, FString& Error)
{
	if (!SafePath(Root, JournalRelative, Error, true)) return false;
	FString Text; FJsonSerializer::Serialize(Journal, TJsonWriterFactory<>::Create(&Text)); return AtomicWrite(Root / JournalRelative, Encode(Text, {}), Error);
}

FString SnapshotRelative(const FString& Id, const FString& File, const TCHAR* Suffix)
{
	return TEXT("Saved/ConvaiAvatarStudio/Configuration/Snapshots/") + Id + TEXT("/") + File + Suffix;
}
bool ReadSnapshot(const FString& Root, const FString& Relative, const FString& ExpectedHash, TArray<uint8>& Bytes, FString& Error)
{
	if (!SafePath(Root, Relative, Error) || !FFileHelper::LoadFileToArray(Bytes, *(Root / Relative)) || Bytes.Num() > 4 * 1024 * 1024 || Hash(Bytes) != ExpectedHash)
	{ if (Error.IsEmpty()) Error = TEXT("A configuration backup is missing or has changed. Restore it manually after reviewing the backup."); return false; }
	return true;
}
bool AnalyzeUndo(const FString& InputRoot, FAnalysis& Analysis, FString& Error)
{
	FString Root; if (!ProjectRoot(InputRoot, Root, Error)) return false;
	InitPlan(Analysis.Plan, Root, true);
	if (!ReadJournal(Root, Analysis.Journal, Error)) return false;
	if (!Analysis.Journal) { Error = TEXT("There is no previous configuration to restore for this project."); return false; }
	FString AppliedProfile;
	Analysis.Journal->TryGetStringField(TEXT("applied_profile_json"), AppliedProfile);
	if (!ReadProfile(AppliedProfile, Analysis.Settings, Error, &Analysis.Plan)) return false;
	Analysis.Journal->TryGetStringField(TEXT("source"), Analysis.Plan.SourceUrl);
	Analysis.Plan.bHasBackup = true;
	FString Snapshot; FGuid SnapshotId;
	const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
	if (!Analysis.Journal->TryGetStringField(TEXT("snapshot"), Snapshot) || !FGuid::ParseExact(Snapshot, EGuidFormats::Digits, SnapshotId) ||
		!Analysis.Journal->TryGetArrayField(TEXT("files"), Files) || Files->IsEmpty() || Files->Num() > 2)
	{ Error = TEXT("The configuration backup journal is invalid."); return false; }
	TSet<FString> Seen;
	for (const auto& Value : *Files)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString File, BeforeHash, AfterHash; bool bExisted = false;
		if (!Value || !Value->TryGetObject(Object) || !Object || !(*Object)->TryGetStringField(TEXT("file"), File) ||
			!(*Object)->TryGetStringField(TEXT("before_hash"), BeforeHash) || !(*Object)->TryGetStringField(TEXT("after_hash"), AfterHash) ||
			!(*Object)->TryGetBoolField(TEXT("existed"), bExisted) || Seen.Contains(File))
		{ Error = TEXT("The configuration backup file entry is invalid."); return false; }
		Seen.Add(File);
		FDocument Current;
		if (!ReadDocument(Root, File, Current, Error)) return false;
		TArray<uint8> Original, Applied; FString OriginalText, AppliedText;
		if (!ReadSnapshot(Root, SnapshotRelative(Snapshot, File, TEXT(".original")), BeforeHash, Original, Error) ||
			!ReadSnapshot(Root, SnapshotRelative(Snapshot, File, TEXT(".applied")), AfterHash, Applied, Error) ||
			!Decode(Original, OriginalText, Error) || !Decode(Applied, AppliedText, Error)) return false;
		FString RestoredText = Current.Text;
		for (const FSetting& Setting : Analysis.Settings)
		{
			if (Setting.File != File) continue;
			const auto Before = Gather(OriginalText, Setting, false);
			const auto After = Gather(AppliedText, Setting, false);
			if (Before == After) continue; // The profile did not own this key.
			if (After != Setting.Lines) { Error = TEXT("The applied backup contains an unrecognized setting value."); return false; }
			const auto Now = Gather(Current.Text, Setting, false);
			if (Now == Before) continue; // A prior rollback/restoration already restored this key.
			if (Now != After)
			{
				Error = TEXT("Restore stopped because a profile setting changed after apply: ") + File + TEXT(" [") + Setting.Section + TEXT("] ") + Setting.Key +
					TEXT(". Keep your edit, or put this key back to its applied value and review restore again. No configuration was changed."); return false;
			}
			FConvaiAvatarConfigurationChange Change; Change.FileName = File; Change.Section = Setting.Section; Change.Key = Setting.Key; Change.Before = Now; Change.After = Before;
			Analysis.Plan.Changes.Add(MoveTemp(Change)); RestoredText = Patch(RestoredText, Setting, Gather(OriginalText, Setting, true), true);
		}
		if (Current.Before == Applied)
		{
			Current.After = Original; Current.bDelete = !bExisted;
		}
		else Current.After = Encode(RestoredText, Current.Before);
		Analysis.Documents.Add(MoveTemp(Current));
	}
	Analysis.Plan.Notes.Add(TEXT("Only settings changed by this profile are restored. Unrelated edits made since apply are preserved."));
	Analysis.Plan.Revision = Revision(Analysis); return true;
}
bool WritableDocuments(const FAnalysis& Analysis, FString& Error)
{
	for (const FDocument& Doc : Analysis.Documents)
	{
		const FString Relative = TEXT("Config/") + Doc.File;
		if (!SafePath(Analysis.Plan.ProjectDirectory, Relative, Error, true)) return false;
		if (IFileManager::Get().IsReadOnly(*(Analysis.Plan.ProjectDirectory / Relative))) { Error = TEXT("Configuration is read-only. Check it out or make it writable, then review again: ") + Doc.File; return false; }
	}
	return true;
}
bool WriteDocuments(const FAnalysis& Analysis, FString& Error)
{
	TArray<const FDocument*> Written;
	for (const FDocument& Doc : Analysis.Documents)
	{
		const FString Path = Analysis.Plan.ProjectDirectory / TEXT("Config") / Doc.File;
		if (Doc.Before == Doc.After && !Doc.bDelete) continue;
		const bool bSuccess = Doc.bDelete ? (!Doc.bExisted || IFileManager::Get().Delete(*Path, true, false, true)) : AtomicWrite(Path, Doc.After, Error);
		if (!bSuccess)
		{
			if (Error.IsEmpty()) Error = TEXT("Could not restore ") + Doc.File;
			for (const FDocument* Done : Written)
			{
				const FString DonePath = Analysis.Plan.ProjectDirectory / TEXT("Config") / Done->File; FString RollbackError;
				if (Done->bExisted) { if (!AtomicWrite(DonePath, Done->Before, RollbackError)) Error += TEXT(" Rollback needs attention: ") + RollbackError; }
				else if (!IFileManager::Get().Delete(*DonePath, false, false, true)) Error += TEXT(" Could not remove the newly created ") + Done->File;
			}
			return false;
		}
		Written.Add(&Doc);
	}
	return true;
}

// Fixtures and other projects must never replace this editor's live configuration.
void ReloadHostConfiguration(const FString& Root, bool bCookExclusionOnly = false)
{
	if (!GConfig || !Full(Root).Equals(Full(FPaths::ProjectDir()), ESearchCase::IgnoreCase)) return;
	FConfigCacheIni::LoadGlobalIniFile(GGameIni, TEXT("Game"), nullptr, true);
	UProjectPackagingSettings* Packaging = GetMutableDefault<UProjectPackagingSettings>();
	FProperty* Property = bCookExclusionOnly ? FindFProperty<FProperty>(UProjectPackagingSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UProjectPackagingSettings, DirectoriesToNeverCook)) : nullptr;
	// Loading an absent array key leaves its current value in UObject::LoadConfig.
	// Clear this constructor-empty array first so removing the last exclusion takes effect.
	Packaging->DirectoriesToNeverCook.Reset();
	Packaging->LoadConfig(nullptr, *GGameIni, UE::LCPF_ReloadingConfigData | UE::LCPF_ReadParentSections, Property);
	if (!bCookExclusionOnly)
	{
		FConfigCacheIni::LoadGlobalIniFile(GEngineIni, TEXT("Engine"), nullptr, true);
		for (const TCHAR* ClassPath : { TEXT("/Script/Engine.RendererSettings"), TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings") })
		{
			if (UClass* Class = FindObject<UClass>(nullptr, ClassPath)) Class->GetDefaultObject()->LoadConfig(nullptr, *GEngineIni, UE::LCPF_ReloadingConfigData | UE::LCPF_ReadParentSections);
		}
	}
}
}

FString FConvaiAvatarConfigurationPlan::Describe() const
{
	FString Text = bRestore ? TEXT("Restore the settings changed by this profile:\n\n") : TEXT("Apply the following selected rendering and packaging settings:\n\n");
	for (const FString& Note : Notes) Text += Note + TEXT("\n");
	Text += TEXT("\nSource: ") + SourceUrl + TEXT("\n");
	for (const auto& Change : Changes)
	{
		Text += TEXT("\n") + Change.FileName + TEXT(" [") + Change.Section + TEXT("]\n");
		Text += TEXT("Before: ") + (Change.Before.IsEmpty() ? TEXT("(not set in this project file)") : FString::Join(Change.Before, TEXT("; "))) + TEXT("\n");
		Text += TEXT("After: ") + (Change.After.IsEmpty() ? TEXT("(remove project override)") : FString::Join(Change.After, TEXT("; "))) + TEXT("\n");
	}
	return Text;
}

bool FConvaiAvatarProjectConfiguration::Analyze(const FString& Root, FConvaiAvatarConfigurationPlan& Plan, FString& Error,
	const FString& ProfileJson, const FString& ProfileSourceUrl, const FString& ProfileNotice)
{
	Error.Empty(); AvatarConfiguration::FAnalysis Analysis; if (!AvatarConfiguration::AnalyzeApply(Root, Analysis, Error, ProfileJson, ProfileSourceUrl, ProfileNotice)) return false; Plan = MoveTemp(Analysis.Plan); return true;
}
bool FConvaiAvatarProjectConfiguration::AnalyzeRestore(const FString& Root, FConvaiAvatarConfigurationPlan& Plan, FString& Error)
{
	Error.Empty(); AvatarConfiguration::FAnalysis Analysis; if (!AvatarConfiguration::AnalyzeUndo(Root, Analysis, Error)) return false; Plan = MoveTemp(Analysis.Plan); return true;
}
bool FConvaiAvatarProjectConfiguration::ApplyProxyProfile(const FString& Directory, FString& Error, const FString& SelectedPlugin,
	const FString& ProfileJson, const FString& Configuration)
{
	using namespace AvatarConfiguration;
	Error.Empty(); FString Root;
	if (!ProjectRoot(Directory, Root, Error)) return false;
	if (Root.Equals(Full(FPaths::ProjectDir()), ESearchCase::IgnoreCase) ||
		!IFileManager::Get().FileExists(*ConvaiAvatarUploaderWorkspace::ProjectPath(Root)))
	{ Error = TEXT("The proxy profile can only configure the managed Cloud Avatars uploader project, never the open project."); return false; }
	// Reject an alias to the open project, including redirected ancestors above the supplied directory.
	for (FString Parent = Root; !Parent.IsEmpty();)
	{
		if (Redirected(Parent)) { Error = TEXT("The uploader configuration path contains a linked directory."); return false; }
		const FString Next = FPaths::GetPath(Parent); if (Next == Parent) break; Parent = Next;
	}
	const FString MarkerPath = Root / TEXT(".convai-workspace.json");
	FString MarkerText, Owner; TSharedPtr<FJsonObject> Marker; double MarkerVersion = 0;
	if (Redirected(MarkerPath) || IFileManager::Get().FileSize(*MarkerPath) > 64 * 1024 ||
		!FFileHelper::LoadFileToString(MarkerText, *MarkerPath) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MarkerText), Marker) || !Marker ||
		!Marker->TryGetStringField(TEXT("owner"), Owner) || Owner != TEXT("ConvaiAvatarUploader") ||
		!Marker->TryGetNumberField(TEXT("version"), MarkerVersion) || MarkerVersion != 2)
	{ Error = TEXT("This project is not an owned Cloud Avatars uploader workspace. Its configuration was not changed."); return false; }
	if (HasBackup(Root)) { Error = TEXT("A reviewed configuration backup is active in this project. The generated proxy profile will not overwrite it."); return false; }
	if (!SelectedPlugin.IsEmpty() && !Algo::AllOf(SelectedPlugin, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_'); }))
	{ Error = TEXT("The selected avatar plugin name is invalid."); return false; }
	TArray<FSetting> Settings;
	if (!ReadProfile(ProfileJson, Settings, Error)) return false;
	if (!Configuration.IsEmpty())
	{
		if (!IsSupportedConfiguration(Configuration)) { Error = TEXT("The published build configuration is not supported."); return false; }
		FSetting Build; Build.File = TEXT("DefaultGame.ini"); Build.Section = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		Build.Key = TEXT("BuildConfiguration"); Build.Lines = { TEXT("BuildConfiguration=PPBC_") + Configuration }; Settings.Add(MoveTemp(Build));
	}
	if (!SelectedPlugin.IsEmpty())
	{
		auto Add = [&Settings](const TCHAR* File, const TCHAR* Section, const TCHAR* Key, TArray<FString> Values)
		{ FSetting Setting; Setting.File = File; Setting.Section = Section; Setting.Key = Key; Setting.Lines = MoveTemp(Values); Settings.Add(MoveTemp(Setting)); };
		const TCHAR* Maps = TEXT("/Script/EngineSettings.GameMapsSettings");
		for (const TCHAR* Key : { TEXT("GameDefaultMap"), TEXT("EditorStartupMap"), TEXT("ServerDefaultMap") })
			Add(TEXT("DefaultEngine.ini"), Maps, Key, { FString(Key) + TEXT("=") });
		const TCHAR* Packaging = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		Add(TEXT("DefaultGame.ini"), Packaging, TEXT("bCookAll"), { TEXT("bCookAll=False") });
		Add(TEXT("DefaultGame.ini"), Packaging, TEXT("bCookMapsOnly"), { TEXT("bCookMapsOnly=False") });
		Add(TEXT("DefaultGame.ini"), Packaging, TEXT("DirectoriesToNeverCook"), { TEXT("!DirectoriesToNeverCook=ClearArray") });
		Add(TEXT("DefaultGame.ini"), Packaging, TEXT("DirectoriesToAlwaysCook"), { TEXT("!DirectoriesToAlwaysCook=ClearArray"), TEXT("+DirectoriesToAlwaysCook=(Path=\"/") + SelectedPlugin + TEXT("\")") });
	}
	FAnalysis Analysis; Analysis.Plan.ProjectDirectory = Root;
	for (const FString& File : { FString(TEXT("DefaultEngine.ini")), FString(TEXT("DefaultGame.ini")) })
	{
		FDocument Doc; if (!ReadDocument(Root, File, Doc, Error)) return false;
		for (const FSetting& Setting : Settings)
			if (Setting.File == File && Gather(Doc.Text, Setting, false) != Setting.Lines) Doc.Text = Patch(Doc.Text, Setting, Setting.Lines, false);
		Doc.After = Encode(Doc.Text, Doc.Before); Analysis.Documents.Add(MoveTemp(Doc));
	}
	// This is a disposable caller-owned project. No host journal or GConfig/CDO is modified.
	return WritableDocuments(Analysis, Error) && WriteDocuments(Analysis, Error);
}
bool FConvaiAvatarProjectConfiguration::Apply(const FConvaiAvatarConfigurationPlan& Reviewed, FString& Error)
{
	using namespace AvatarConfiguration;
	Error.Empty(); FAnalysis Analysis;
	if (Reviewed.bRestore || !AnalyzeApply(Reviewed.ProjectDirectory, Analysis, Error, Reviewed.ProfileJson, Reviewed.SourceUrl)) return false;
	if (Reviewed.Revision != Analysis.Plan.Revision) { Error = TEXT("Project configuration changed since this preview. Review the updated settings before applying."); return false; }
	if (Analysis.Plan.bAlreadyApplied) return true;
	if (Analysis.Plan.bHasBackup) { Error = TEXT("A previous profile backup is still active. Restore the previous configuration before applying a new profile."); return false; }
	if (!WritableDocuments(Analysis, Error)) return false;
	const FString Root = Analysis.Plan.ProjectDirectory;
	const FString Snapshot = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const auto Journal = MakeShared<FJsonObject>();
	Journal->SetStringField(TEXT("project"), Root); Journal->SetStringField(TEXT("profile"), ProfileId); Journal->SetStringField(TEXT("source"), Analysis.Plan.SourceUrl);
	Journal->SetStringField(TEXT("applied_profile_json"), Analysis.Plan.ProfileJson);
	Journal->SetStringField(TEXT("snapshot"), Snapshot); Journal->SetStringField(TEXT("state"), TEXT("applying"));
	TArray<TSharedPtr<FJsonValue>> Files;
	for (const FDocument& Doc : Analysis.Documents)
	{
		if (Doc.Before == Doc.After) continue;
		const FString Original = SnapshotRelative(Snapshot, Doc.File, TEXT(".original"));
		const FString Applied = SnapshotRelative(Snapshot, Doc.File, TEXT(".applied"));
		if (!SafePath(Root, Original, Error, true) || !SafePath(Root, Applied, Error, true) ||
			!AtomicWrite(Root / Original, Doc.Before, Error) || !AtomicWrite(Root / Applied, Doc.After, Error)) return false;
		const auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("file"), Doc.File); Entry->SetBoolField(TEXT("existed"), Doc.bExisted);
		Entry->SetStringField(TEXT("before_hash"), Hash(Doc.Before)); Entry->SetStringField(TEXT("after_hash"), Hash(Doc.After));
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const auto& Change : Analysis.Plan.Changes)
		{
			if (Change.FileName != Doc.File) continue;
			const auto Key = MakeShared<FJsonObject>(); Key->SetStringField(TEXT("section"), Change.Section); Key->SetStringField(TEXT("key"), Change.Key);
			Key->SetArrayField(TEXT("before"), JsonStrings(Change.Before)); Key->SetArrayField(TEXT("after"), JsonStrings(Change.After)); Keys.Add(MakeShared<FJsonValueObject>(Key));
		}
		Entry->SetArrayField(TEXT("changed_keys"), Keys); Files.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Journal->SetArrayField(TEXT("files"), Files);
	if (!WriteJournal(Root, Journal, Error)) return false;
	if (!WriteDocuments(Analysis, Error)) { Error += TEXT(" Original bytes remain in the configuration backup."); return false; }
	ReloadHostConfiguration(Root);
	Journal->SetStringField(TEXT("state"), TEXT("applied"));
	if (!WriteJournal(Root, Journal, Error)) { Error = TEXT("Settings were applied, but the journal status could not be finalized. The original backup remains available for restore."); return false; }
	return true;
}
bool FConvaiAvatarProjectConfiguration::Restore(const FConvaiAvatarConfigurationPlan& Reviewed, FString& Error)
{
	using namespace AvatarConfiguration;
	Error.Empty(); FAnalysis Analysis;
	if (!Reviewed.bRestore || !AnalyzeUndo(Reviewed.ProjectDirectory, Analysis, Error)) return false;
	if (Reviewed.Revision != Analysis.Plan.Revision) { Error = TEXT("Project configuration changed since this restore preview. Review it again before restoring."); return false; }
	if (!WritableDocuments(Analysis, Error) || !WriteDocuments(Analysis, Error)) return false;
	const FString Root = Analysis.Plan.ProjectDirectory;
	ReloadHostConfiguration(Root);
	if (!SafePath(Root, JournalRelative, Error) || !IFileManager::Get().Delete(*(Root / JournalRelative), true, false, true))
	{ Error = TEXT("Settings were restored, but the active backup journal could not be cleared. Original snapshot files remain available."); return false; }
	return true;
}
namespace AvatarConfiguration
{
const TCHAR* CookSection = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
FString CookBegin(const FString& Plugin) { return TEXT("; BEGIN Convai Cloud Avatars staging ") + Plugin; }
FString CookEnd(const FString& Plugin) { return TEXT("; END Convai Cloud Avatars staging ") + Plugin; }
FString CookLine(const FString& Path) { return TEXT("+DirectoriesToNeverCook=(Path=\"") + Path + TEXT("\")"); }

bool StagingOwner(const FString& Root, const FString& Plugin, TSharedPtr<FJsonObject>& Owner, FString& Error)
{
	if (!Plugin.StartsWith(TEXT("ConvaiAvatar_"), ESearchCase::CaseSensitive) || Plugin.Len() > 100 ||
		!Algo::AllOf(Plugin, [](TCHAR C) { return C < 128 && (FChar::IsAlnum(C) || C == TEXT('_')); }))
	{ Error = TEXT("Only a managed avatar plugin can change its staging cook exclusion."); return false; }
	const FString Relative = TEXT("Plugins/ConvaiAvatars/") + Plugin + TEXT("/ConvaiAvatarStudio.json");
	FString Text, Name, Id;
	const int64 Size = IFileManager::Get().FileSize(*(Root / Relative));
	if (!SafePath(Root, Relative, Error) || Size <= 0 || Size > 64 * 1024 * 1024 || !FFileHelper::LoadFileToString(Text, *(Root / Relative)) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Owner) || !Owner ||
		!Owner->TryGetStringField(TEXT("plugin_name"), Name) || Name != Plugin || !Owner->TryGetStringField(TEXT("asset_id"), Id) || Id.TrimStartAndEnd().IsEmpty())
	{ if (Error.IsEmpty()) Error = TEXT("The avatar ownership record is missing or invalid. Restore its local record before changing cook exclusions."); return false; }
	return true;
}
bool StagingRoots(const FString& Root, const FString& Plugin, const FJsonObject& Owner, TArray<FString>& Out, FString& Error)
{
	const FString Mount = TEXT("/") + Plugin + TEXT("/");
	TSet<FString> Roots;
	auto AddPackage = [&](const FString& Package)
	{
		if (!Package.StartsWith(Mount, ESearchCase::CaseSensitive) || Package.Len() >= NAME_SIZE || !FPackageName::IsValidTextForLongPackageName(Package))
		{ Error = TEXT("The avatar's saved package map contains an invalid destination. Prepare the avatar again before changing cook exclusions."); return false; }
		const FString Relative = Package.Mid(Mount.Len()); int32 Slash = INDEX_NONE;
		if (!Relative.FindChar(TEXT('/'), Slash) || Slash <= 0)
		{ Error = TEXT("An avatar asset is saved directly at its plugin's content root. Move it into a content subfolder in Unreal and retry. Existing cook rules were kept."); return false; }
		Roots.Add(Mount + Relative.Left(Slash)); return true;
	};
	const TSharedPtr<FJsonObject>* Packages = nullptr;
	if (!Owner.TryGetObjectField(TEXT("packages"), Packages) || (*Packages)->Values.Num() > 15000)
	{ Error = TEXT("The avatar's package map is missing or invalid. Prepare the avatar again before changing cook exclusions."); return false; }
	for (const auto& Pair : (*Packages)->Values)
	{
		FString Destination;
		if (!Pair.Value || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Destination))
		{ Error = TEXT("The avatar's package map contains an unreadable destination. Prepare the avatar again before changing cook exclusions."); return false; }
		if (!AddPackage(Destination)) return false;
	}
	// During initial preparation the map exists before bytes are copied. On later updates,
	// also cover every actual first-level content directory, including new user-created ones.
	const FString Content = TEXT("Plugins/ConvaiAvatars/") + Plugin + TEXT("/Content");
	if (!SafePath(Root, Content, Error)) return false;
	TArray<FString> Children;
	IFileManager::Get().FindFiles(Children, *(Root / Content / TEXT("*")), true, true);
	if (Children.Num() > 15000) { Error = TEXT("The avatar contains too many top-level content entries to review safely."); return false; }
	for (const FString& Child : Children)
	{
		const FString Relative = Content / Child;
		if (!SafePath(Root, Relative, Error)) return false;
		if (IFileManager::Get().DirectoryExists(*(Root / Relative)))
		{
			if (!AddPackage(Mount + Child + TEXT("/Placeholder"))) return false;
		}
		else
		{
			const FString Extension = FPaths::GetExtension(Child).ToLower();
			if (Extension == TEXT("uasset") || Extension == TEXT("umap") || Extension == TEXT("uexp") || Extension == TEXT("ubulk") || Extension == TEXT("uptnl"))
			{ Error = TEXT("An avatar asset is saved directly at its plugin's content root. Move it into a content subfolder in Unreal and retry. Existing cook rules were kept."); return false; }
		}
	}
	Out = Roots.Array(); Out.Sort();
	if (Out.IsEmpty()) { Error = TEXT("The avatar has no content subfolders to exclude. Prepare its source before continuing."); return false; }
	return true;
}

struct FStagingCookBlock
{
	TArray<FString> Remaining;
	TSet<FString> Paths;
	int32 InsertAt = INDEX_NONE;
	bool bLegacy = false;
};
bool ReadStagingCookBlock(const FString& Text, const FString& Plugin, FStagingCookBlock& Block, FString& Error)
{
	const FString Mount = TEXT("/") + Plugin;
	FString Section; bool bInside = false, bFound = false;
	for (const FString& Line : Lines(Text))
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed == CookBegin(Plugin))
		{
			if (bInside || bFound || !Section.Equals(CookSection, ESearchCase::IgnoreCase)) break;
			bInside = true; bFound = true; continue;
		}
		if (Trimmed == CookEnd(Plugin))
		{
			if (!bInside || Block.Paths.IsEmpty()) break;
			bInside = false; continue;
		}
		if (bInside)
		{
			FString Path;
			if (!FParse::Value(*Trimmed, TEXT("Path="), Path) || Trimmed != CookLine(Path) ||
				!Path.StartsWith(Mount + TEXT("/"), ESearchCase::CaseSensitive) || !FPackageName::IsValidTextForLongPackageName(Path) ||
				Path.Mid(Mount.Len() + 1).Contains(TEXT("/")) || Block.Paths.Contains(Path)) break;
			Block.Paths.Add(Path); continue;
		}
		FString NextSection = Section;
		if (ParseSection(Line, NextSection))
		{
			if (Section.Equals(CookSection, ESearchCase::IgnoreCase)) Block.InsertAt = Block.Remaining.Num();
			Section = MoveTemp(NextSection); Block.Remaining.Add(Line); continue;
		}
		FString Path;
		const bool bLegacy = Section.Equals(CookSection, ESearchCase::IgnoreCase) && FParse::Value(*Trimmed, TEXT("Path="), Path) &&
			(Path.Equals(Mount, ESearchCase::IgnoreCase) || Path.Equals(Mount + TEXT("/"), ESearchCase::IgnoreCase)) &&
			(Trimmed == CookLine(Path) || Trimmed == CookLine(Path).Mid(1));
		if (bLegacy) Block.bLegacy = true;
		else Block.Remaining.Add(Line);
	}
	// The parser must consume every line; malformed owned blocks stay untouched.
	int32 Starts = 0, Ends = 0;
	for (const FString& Line : Lines(Text)) { Starts += Line.TrimStartAndEnd() == CookBegin(Plugin); Ends += Line.TrimStartAndEnd() == CookEnd(Plugin); }
	if (bInside || Starts != Ends || Starts > 1 || (Starts == 1 && (!bFound || Block.Paths.IsEmpty())))
	{ Error = TEXT("The avatar's generated cook-exclusion block was edited or is incomplete. Restore that block from a known copy before retrying. Other cook rules were kept."); return false; }
	if (Section.Equals(CookSection, ESearchCase::IgnoreCase)) Block.InsertAt = Block.Remaining.Num();
	return true;
}
}

bool FConvaiAvatarProjectConfiguration::HasStagingCookExclusion(const FString& InputRoot, const FString& PluginName, bool& bOutExcluded, FString& Error)
{
	using namespace AvatarConfiguration;
	bOutExcluded = false; Error.Reset(); FString Root; TSharedPtr<FJsonObject> Owner; FDocument Doc; FStagingCookBlock Block;
	if (!ProjectRoot(InputRoot, Root, Error) || !StagingOwner(Root, PluginName, Owner, Error) ||
		!ReadDocument(Root, TEXT("DefaultGame.ini"), Doc, Error) || !ReadStagingCookBlock(Doc.Text, PluginName, Block, Error)) return false;
	bOutExcluded = Block.bLegacy || !Block.Paths.IsEmpty(); return true;
}

bool FConvaiAvatarProjectConfiguration::MigrateLegacyStagingCookExclusion(const FString& InputRoot, const FString& PluginName, FString& Error)
{
	using namespace AvatarConfiguration;
	Error.Reset(); FString Root; TSharedPtr<FJsonObject> Owner; FDocument Doc; FStagingCookBlock Block;
	if (!ProjectRoot(InputRoot, Root, Error) || !StagingOwner(Root, PluginName, Owner, Error)) return false;
	const auto Staging = Owner->TryGetField(TEXT("staging"));
	if (!Staging || Staging->Type != EJson::Boolean) { Error = TEXT("The avatar's staging state is unreadable. Restore its local record before updating cook exclusions."); return false; }
	if (!Staging->AsBool()) return true;
	if (!ReadDocument(Root, TEXT("DefaultGame.ini"), Doc, Error) || !ReadStagingCookBlock(Doc.Text, PluginName, Block, Error)) return false;
	return !Block.bLegacy || SetStagingCookExclusion(Root, PluginName, true, Error);
}

bool FConvaiAvatarProjectConfiguration::SetStagingCookExclusion(const FString& InputRoot, const FString& PluginName, bool bExclude, FString& Error)
{
	using namespace AvatarConfiguration;
	Error.Empty(); FString Root; TSharedPtr<FJsonObject> Owner;
	if (!ProjectRoot(InputRoot, Root, Error) || !StagingOwner(Root, PluginName, Owner, Error)) return false;
	TArray<FString> Roots;
	if (bExclude && !StagingRoots(Root, PluginName, *Owner, Roots, Error)) return false;
	FDocument Doc; if (!ReadDocument(Root, TEXT("DefaultGame.ini"), Doc, Error)) return false;
	FStagingCookBlock Block;
	if (!ReadStagingCookBlock(Doc.Text, PluginName, Block, Error)) return false;
	const FString Newline = Doc.Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
	TArray<FString>& Output = Block.Remaining;
	if (bExclude)
	{
		// UE's reference-domain database already owns /Plugin/. Child roots avoid
		// registering the same path as both the plugin and Never Cooked Content.
		FString Exclusion = CookBegin(PluginName) + Newline;
		for (const FString& Path : Roots) Exclusion += CookLine(Path) + Newline;
		Exclusion += CookEnd(PluginName) + Newline;
		if (Block.InsertAt == INDEX_NONE)
		{
			if (!Output.IsEmpty() && !Output.Last().EndsWith(TEXT("\n"))) Output.Last() += Newline;
			Output.Add(Newline + TEXT("[") + CookSection + TEXT("]") + Newline); Output.Add(Exclusion);
		}
		else
		{
			if (Block.InsertAt > 0 && !Output[Block.InsertAt - 1].EndsWith(TEXT("\n"))) Output[Block.InsertAt - 1] += Newline;
			Output.Insert(Exclusion, Block.InsertAt);
		}
	}
	Doc.Text = FString::Join(Output, TEXT("")); Doc.After = Encode(Doc.Text, Doc.Before);
	if (Doc.Before == Doc.After) return true; // Existing correct rules also work in a read-only checkout.
	FAnalysis Analysis; Analysis.Plan.ProjectDirectory = Root; Analysis.Documents.Add(MoveTemp(Doc));
	if (!WritableDocuments(Analysis, Error) || !WriteDocuments(Analysis, Error)) return false;
	ReloadHostConfiguration(Root, true);
	return true;
}
bool FConvaiAvatarProjectConfiguration::HasBackup(const FString& Root)
{
	return IFileManager::Get().FileExists(*(AvatarConfiguration::Full(Root) / AvatarConfiguration::JournalRelative));
}
FString FConvaiAvatarProjectConfiguration::GetBackupDirectory(const FString& Root)
{
	return AvatarConfiguration::Full(Root) / TEXT("Saved/ConvaiAvatarStudio/Configuration");
}
FString FConvaiAvatarProjectConfiguration::GetSourceUrl() { return AvatarConfiguration::SourceUrl; }
FString FConvaiAvatarProjectConfiguration::GetProfileId() { return AvatarConfiguration::ProfileId; }
bool FConvaiAvatarProjectConfiguration::ValidateProfile(const FString& Json, FString& Error)
{
	using namespace AvatarConfiguration;
	Error.Empty(); TSharedPtr<FJsonObject> Root; const TArray<TSharedPtr<FJsonValue>>* Settings = nullptr;
	if (Json.Len() > 64 * 1024 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root || !Root->TryGetArrayField(TEXT("settings"), Settings) || Settings->IsEmpty())
	{ Error = TEXT("The configuration profile is invalid."); return false; }
	double Schema = 1;
	if (Root->HasField(TEXT("schema_version")) && (!Root->TryGetNumberField(TEXT("schema_version"), Schema) || Schema != 1))
	{ Error = TEXT("This project profile version is not supported by the installed plugin."); return false; }
	FString Compression;
	if (!CompressionArguments(Root, TEXT("Shipping"), Compression, Error)) return false;
	const auto Allowed = Profile(); TSet<FString> Seen;
	if (Settings->Num() > Allowed.Num()) { Error = TEXT("The configuration profile contains too many settings."); return false; }
	for (const auto& Value : *Settings)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr; FString File, Section, Key; TArray<FString> Lines;
		if (!Value || !Value->TryGetObject(Object) || !Object || !(*Object)->TryGetStringField(TEXT("file"), File) ||
			!(*Object)->TryGetStringField(TEXT("section"), Section) || !(*Object)->TryGetStringField(TEXT("key"), Key) || !ReadStrings(*Object, TEXT("values"), Lines))
		{ Error = TEXT("A configuration profile entry is invalid."); return false; }
		const FSetting* Match = Allowed.FindByPredicate([&](const auto& Item) { return Item.File == File && Item.Section == Section && Item.Key == Key; });
		const FString Identity = File + TEXT("|") + Section + TEXT("|") + Key;
		if (!Match || Seen.Contains(Identity)) { Error = TEXT("The profile contains an unreviewed setting or duplicate key."); return false; }
		if (Match->Lines != Lines)
		{
			// Remote renderer values are data, not arbitrary INI sections or expressions. RHI and pak-format invariants stay fixed.
			if (Section != TEXT("/Script/Engine.RendererSettings") || Lines.Num() != 1 || !Lines[0].StartsWith(Key + TEXT("=")))
			{ Error = TEXT("The profile contains an unsupported setting value."); return false; }
			const FString ValueText = Lines[0].Mid(Key.Len() + 1), DefaultValue = Match->Lines[0].Mid(Key.Len() + 1);
			if (DefaultValue == TEXT("True") || DefaultValue == TEXT("False"))
			{
				if (ValueText != TEXT("True") && ValueText != TEXT("False")) { Error = TEXT("A renderer switch must be True or False."); return false; }
			}
			else
			{
				double Number = 0;
				if (ValueText.IsEmpty() || ValueText.Len() > 24 || !Algo::AllOf(ValueText, [](TCHAR C) { return FChar::IsDigit(C) || C == TEXT('.') || C == TEXT('-'); }) ||
					!FDefaultValueHelper::IsStringValidFloat(ValueText) || !LexTryParseString(Number, *ValueText) || !FMath::IsFinite(Number) || Number < 0 || Number > 65536)
				{ Error = TEXT("A renderer setting contains an invalid numeric value."); return false; }
			}
		}
		Seen.Add(Identity);
	}
	return true;
}

bool FConvaiAvatarProjectConfiguration::IsSupportedConfiguration(const FString& Configuration)
{
	return Configuration == TEXT("Shipping") || Configuration == TEXT("Development") || Configuration == TEXT("Test") ||
		Configuration == TEXT("Debug") || Configuration == TEXT("DebugGame");
}

bool FConvaiAvatarProjectConfiguration::GetPakCompressionArguments(const FString& ProfileJson, const FString& Configuration, FString& Arguments, FString& Error)
{
	Error.Reset(); Arguments.Reset();
	if (!Configuration.IsEmpty() && !IsSupportedConfiguration(Configuration)) { Error = TEXT("The published build configuration is not supported."); return false; }
	if (!ProfileJson.IsEmpty() && !ValidateProfile(ProfileJson, Error)) return false;
	TSharedPtr<FJsonObject> Json;
	if (!ProfileJson.IsEmpty()) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ProfileJson), Json);
	return AvatarConfiguration::CompressionArguments(Json, Configuration, Arguments, Error);
}
