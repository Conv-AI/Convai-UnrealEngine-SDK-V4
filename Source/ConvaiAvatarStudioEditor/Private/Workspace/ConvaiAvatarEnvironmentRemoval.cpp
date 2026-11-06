// Copyright 2026 Convai Inc. All Rights Reserved.
#include "ConvaiAvatarEnvironmentRemoval.h"
#include "Packaging/ConvaiAvatarPackagingPrivate.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Engine/Level.h"
#include "ExternalPackageHelper.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace ConvaiAvatarEnvironmentRemoval
{
	FString ReceiptPath(FName Map)
	{
		return FPaths::ProjectSavedDir() / TEXT("ConvaiAvatarStudio/EnvironmentRemovals") / (FMD5::HashAnsiString(*Map.ToString()) + TEXT(".txt"));
	}
	static bool Resolve(FName Map, const FString& Relative, FString& File)
	{
		const FString Name = Map.ToString();
		const FString Plugin = FPackageName::GetPackageMountPoint(Name).ToString();
		if (!FConvaiAvatarWorkspace::IsSafePluginName(Plugin) || !FPaths::IsRelative(Relative) || Relative.Contains(TEXT("..")) || Relative.Contains(TEXT(":"))) return false;
		const FString Mount = TEXT("/") + Plugin + TEXT("/");
		FString Root;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Mount, Root)) return false;
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		File = FPaths::ConvertRelativePathToFull(Root / Relative);
		FPaths::NormalizeFilename(File);
		const FString Virtual = Mount + Relative;
		const bool bMap = Virtual.StartsWith(Name + TEXT("."));
		const bool bExternal = Virtual.StartsWith(ULevel::GetExternalActorsPath(Name) + TEXT("/")) ||
			Virtual.StartsWith(FExternalPackageHelper::GetExternalObjectsPath(Name) + TEXT("/"));
		const FString Extension = FPaths::GetExtension(File).ToLower();
		if ((!bMap && !bExternal) || !(Extension == TEXT("umap") || (bExternal && Extension == TEXT("uasset")) ||
			Extension == TEXT("ubulk") || Extension == TEXT("uexp") || Extension == TEXT("uptnl"))) return false;
		if (!File.StartsWith(Root + TEXT("/"))) return false;
		for (FString Path = File; Path.StartsWith(Root + TEXT("/")); Path = FPaths::GetPath(Path))
			if (ConvaiAvatarPackagingPrivate::IsReparsePoint(Path)) return false;
		return !ConvaiAvatarPackagingPrivate::IsReparsePoint(Root);
	}
	static bool Read(FName Map, TArray<FString>& Relative, FString& Error)
	{
		const FString Receipt = ReceiptPath(Map);
		if (!IFileManager::Get().FileExists(*Receipt)) return true;
		if (FFileHelper::LoadFileToStringArray(Relative, *Receipt))
		{
			for (const FString& Item : Relative)
			{
				FString File;
				if (!Resolve(Map, Item, File)) { Error = TEXT("The pending environment source-control record contains an invalid path."); return false; }
			}
			return true;
		}
		Error = TEXT("Could not read pending environment source-control changes."); return false;
	}
	bool Record(FName Map, const TArray<FString>& Files, FString& Error)
	{
		TArray<FString> Relative;
		if (!Read(Map, Relative, Error)) return false;
		const FString Mount = TEXT("/") + FPackageName::GetPackageMountPoint(Map.ToString()).ToString() + TEXT("/");
		FString Root;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Mount, Root))
		{ Error = TEXT("The environment plugin is not mounted."); return false; }
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		Root += TEXT("/");
		for (const FString& File : Files)
		{
			FString Item = FPaths::ConvertRelativePathToFull(File), Resolved;
			if (!FPaths::MakePathRelativeTo(Item, *Root) || !Resolve(Map, Item, Resolved))
			{ Error = TEXT("Could not record an environment file outside its owned map."); return false; }
			Relative.AddUnique(Item);
		}
		if (Relative.IsEmpty()) return true;
		const FString Receipt = ReceiptPath(Map), Temp = Receipt + TEXT(".tmp");
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Receipt), true);
		if (FFileHelper::SaveStringArrayToFile(Relative, *Temp) && IFileManager::Get().Move(*Receipt, *Temp, true, false)) return true;
		Error = TEXT("Could not save pending environment source-control changes. No further files were removed."); return false;
	}
	bool Finish(FName Map, TFunctionRef<bool(const TArray<FString>&, const TArray<FString>&)> Finalize, FString& Error)
	{
		TArray<FString> Relative, Missing, Existing;
		if (!Read(Map, Relative, Error)) return false;
		for (const FString& Item : Relative)
		{
			FString File;
			if (!Resolve(Map, Item, File)) return false;
			if (!IFileManager::Get().FileExists(*File)) Missing.Add(File);
			else Existing.Add(File);
		}
		if (!Relative.IsEmpty() && !Finalize(Missing, Existing))
		{
			if (Error.IsEmpty()) Error = TEXT("Source control could not finish the environment changes. Their paths have been saved; reconnect to source control and retry Upload changes.");
			return false;
		}
		if (IFileManager::Get().Delete(*ReceiptPath(Map), false)) return true;
		Error = TEXT("Could not clear the completed environment source-control record."); return false;
	}
}
