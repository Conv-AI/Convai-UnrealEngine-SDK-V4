// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarSupportFiles.h"
#include "Services/ConvaiAvatarDownloadService.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace ConvaiAvatarSupportFiles
{
namespace
{
	FString Full(const FString& Path) { return FPaths::ConvertRelativePathToFull(Path); }
	FString Source(const FConvaiAvatarPendingDownload& Job, const FString& Relative) { return Full(Job.JobDirectory / TEXT("Source/BaseContent") / Relative); }
	FString Temporary(const FConvaiAvatarPendingDownload& Job, const FString& Relative) { return Full(Job.JobDirectory / TEXT("SupportInstall") / Relative); }
	FString Target(const FString& Relative) { return Full(FPaths::ProjectContentDir() / Relative); }
	FString PackageName(const FString& Relative) { return TEXT("/Game/") + FPaths::ChangeExtension(Relative, TEXT("")); }
	bool Hex(const FString& Value, int32 Length)
	{
		if (Value.Len() != Length) return false;
		for (TCHAR C : Value) if (!FChar::IsHexDigit(C)) return false;
		return true;
	}
	bool RelativePath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.Len() > 2048 || Path.Contains(TEXT("\\")) || Path.StartsWith(TEXT("/"))) return false;
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		if (Extension != TEXT("uasset") && Extension != TEXT("uexp") && Extension != TEXT("ubulk") && Extension != TEXT("uptnl")) return false;
		TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
		for (const FString& Part : Parts)
		{
			const FString Base = FPaths::GetBaseFilename(Part).ToUpper();
			if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" ")) || Part.Len() > 255
				|| Base == TEXT("CON") || Base == TEXT("PRN") || Base == TEXT("AUX") || Base == TEXT("NUL")
				|| (Base.Len() == 4 && (Base.StartsWith(TEXT("COM")) || Base.StartsWith(TEXT("LPT"))) && Base[3] >= '1' && Base[3] <= '9')) return false;
		}
		return FPackageName::IsValidTextForLongPackageName(PackageName(Path));
	}
	bool NoLinks(const FString& Path, FString& Error)
	{
#if PLATFORM_WINDOWS
		FString Current = Full(Path);
		while (!Current.IsEmpty())
		{
			const DWORD Attributes = GetFileAttributesW(*Current);
			if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT))
			{ Error = TEXT("Shared avatar support uses a symbolic link or junction. Its files were kept; use ordinary project folders before retrying."); return false; }
			const FString Parent = FPaths::GetPath(Current); if (Parent == Current) break; Current = Parent;
		}
		return true;
#else
		Error = TEXT("Installing shared avatar support currently requires Windows."); return false;
#endif
	}
	bool Loaded(const FString& Relative, bool& Dirty)
	{
		Dirty = false;
		UPackage* Package = FindPackage(nullptr, *PackageName(Relative));
		if (!Package) return false;
		Dirty = Package->IsDirty();
		bool Objects = false;
		ForEachObjectWithPackage(Package, [&Objects](UObject*) { Objects = true; return false; });
		return Dirty || Objects || Package->GetLinker();
	}
	bool Matches(const FString& File, const FString& Expected)
	{
		const FMD5Hash Hash = FMD5Hash::HashFile(*File);
		return Hash.IsValid() && LexToString(Hash).Equals(Expected, ESearchCase::IgnoreCase);
	}
#if PLATFORM_WINDOWS
	FString Identity(HANDLE Handle)
	{
		BY_HANDLE_FILE_INFORMATION Info{};
		if (!GetFileInformationByHandle(Handle, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return FString();
		return FString::Printf(TEXT("%08x%08x%08x"), Info.dwVolumeSerialNumber, Info.nFileIndexHigh, Info.nFileIndexLow);
	}
	FString HandleHash(HANDLE Handle)
	{
		LARGE_INTEGER Start{}; if (!SetFilePointerEx(Handle, Start, nullptr, FILE_BEGIN)) return FString();
		FMD5 Digest; TArray<uint8> Bytes; Bytes.SetNumUninitialized(1024 * 1024);
		DWORD Read = 0;
		while (true)
		{
			if (!ReadFile(Handle, Bytes.GetData(), Bytes.Num(), &Read, nullptr)) return FString();
			if (!Read) break; Digest.Update(Bytes.GetData(), Read);
		}
		uint8 Hash[16]; Digest.Final(Hash); return BytesToHex(Hash, 16).ToLower();
	}
	// A retained Windows file identity proves ownership across the intent/move journal gap.
	// Only private temporary files may be deleted without matching their completed content hash.
	bool RemoveOwned(const FString& File, const FString& ExpectedIdentity, const FString& ExpectedHash, bool bTemporary, FString& Error)
	{
		if (!IFileManager::Get().FileExists(*File)) return true;
		if (!NoLinks(File, Error)) return false;
		HANDLE Handle = CreateFileW(*File, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Handle == INVALID_HANDLE_VALUE) { Error = TEXT("A newly added shared support file is still in use. Restart Unreal to finish the pending download rollback."); return false; }
		bool Ok = true;
		if (Identity(Handle) == ExpectedIdentity && (bTemporary || HandleHash(Handle).Equals(ExpectedHash, ESearchCase::IgnoreCase)))
		{
			FILE_DISPOSITION_INFO Disposition{true};
			Ok = SetFileInformationByHandle(Handle, FileDispositionInfo, &Disposition, sizeof(Disposition)) != 0;
			if (!Ok) Error = TEXT("A newly added shared support file could not be removed. Restart Unreal to finish the pending rollback.");
		}
		CloseHandle(Handle); return Ok; // Different identity or bytes belong to someone else; preserve them.
	}
	bool CreateOwnedTemporary(FConvaiAvatarPendingDownload& Job, const FString& Relative, const FString& ExpectedHash, FString& Error)
	{
		const FString Temp = Temporary(Job, Relative);
		if (!NoLinks(Temp, Error) || !NoLinks(Source(Job, Relative), Error)) return false;
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Temp), true)) { Error = TEXT("Could not create shared support staging folders."); return false; }
		HANDLE Writer = CreateFileW(*Temp, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Writer == INVALID_HANDLE_VALUE) { Error = TEXT("A shared support temporary file is already present or locked. Retry the pending download after restarting Unreal."); return false; }
		const FString OwnedIdentity = Identity(Writer);
		const FString PreviousIdentity = Job.SupportOwnedIdentities.FindRef(Relative);
		const bool HadPreviousIdentity = Job.SupportOwnedIdentities.Contains(Relative);
		bool Ok = !OwnedIdentity.IsEmpty();
		if (Ok)
		{
			Job.SupportOwnedIdentities.Add(Relative, OwnedIdentity);
			Ok = FConvaiAvatarDownloadService::SaveJob(Job, Error);
			if (!Ok)
			{
				if (HadPreviousIdentity) Job.SupportOwnedIdentities.Add(Relative, PreviousIdentity);
				else Job.SupportOwnedIdentities.Remove(Relative);
			}
		}
		else Error = TEXT("The shared support file's ownership could not be verified. No project content was installed.");
		const bool Recorded = Ok;
		HANDLE Reader = INVALID_HANDLE_VALUE;
		if (Ok) Reader = CreateFileW(*Source(Job, Relative), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Ok && Reader == INVALID_HANDLE_VALUE) { Error = TEXT("The staged shared support file could not be read. Download this avatar again."); Ok = false; }
		TArray<uint8> Bytes; Bytes.SetNumUninitialized(1024 * 1024);
		while (Ok)
		{
			DWORD Read = 0, Written = 0;
			if (!ReadFile(Reader, Bytes.GetData(), Bytes.Num(), &Read, nullptr)) { Ok = false; break; }
			if (!Read) break;
			if (!WriteFile(Writer, Bytes.GetData(), Read, &Written, nullptr) || Written != Read) { Ok = false; break; }
		}
		if (Ok) Ok = FlushFileBuffers(Writer) && HandleHash(Writer).Equals(ExpectedHash, ESearchCase::IgnoreCase);
		if (Reader != INVALID_HANDLE_VALUE) CloseHandle(Reader);
		if (!Recorded) { FILE_DISPOSITION_INFO Disposition{true}; SetFileInformationByHandle(Writer, FileDispositionInfo, &Disposition, sizeof(Disposition)); }
		CloseHandle(Writer);
		if (!Ok && Error.IsEmpty()) Error = TEXT("The shared support file could not be copied consistently. Check disk space and download this avatar again.");
		return Ok;
	}
#endif
}

bool Validate(const FConvaiAvatarPendingDownload& Job, FString& Error)
{
	if (Job.SupportFiles.Num() > 100000 || (Job.SupportPhase != TEXT("pending") && Job.SupportPhase != TEXT("installing") && Job.SupportPhase != TEXT("installed") && Job.SupportPhase != TEXT("rolling_back")))
	{ Error = TEXT("The pending shared support record is invalid."); return false; }
	TSet<FString> Names;
	for (const auto& Pair : Job.SupportFiles)
	{
		if (!RelativePath(Pair.Key) || !Hex(Pair.Value, 32) || Names.Contains(Pair.Key.ToLower()))
		{ Error = TEXT("The pending shared support record contains an unsafe or duplicate path or hash."); return false; }
		Names.Add(Pair.Key.ToLower());
		if (FPaths::GetExtension(Pair.Key).ToLower() != TEXT("uasset") && !Job.SupportFiles.Contains(FPaths::ChangeExtension(Pair.Key, TEXT("uasset"))))
		{ Error = TEXT("A shared support sidecar does not identify its owning asset package."); return false; }
		if (!NoLinks(Source(Job, Pair.Key), Error) || !NoLinks(Temporary(Job, Pair.Key), Error) || !NoLinks(Target(Pair.Key), Error)) return false;
	}
	for (const auto& Pair : Job.SupportOwnedIdentities)
		if (!Job.SupportFiles.Contains(Pair.Key) || !Hex(Pair.Value, 24)) { Error = TEXT("The pending shared support ownership record is invalid."); return false; }
	return true;
}

bool Preflight(const FConvaiAvatarPendingDownload& Job, FString& Error)
{
	check(IsInGameThread());
	Error.Reset();
	if (!Validate(Job, Error)) return false;
	for (const auto& Pair : Job.SupportFiles)
	{
		const FString File = Target(Pair.Key);
		bool Dirty = false; const bool InMemory = Loaded(Pair.Key, Dirty);
		if (Dirty) { Error = TEXT("Save or discard changes to shared avatar asset ") + PackageName(Pair.Key) + TEXT(" before retrying the download. Its current files were kept."); return false; }
		if (IFileManager::Get().FileExists(*File))
		{
			if (!Matches(File, Pair.Value)) { Error = TEXT("Download stopped because this project has a different shared avatar asset at ") + PackageName(Pair.Key) + TEXT(". Your existing files were kept. Resolve this conflict or download into a project with compatible common assets, then retry."); return false; }
		}
		else if (IFileManager::Get().DirectoryExists(*File)) { Error = TEXT("A folder occupies the required shared asset path ") + PackageName(Pair.Key) + TEXT(". Move it before retrying the download."); return false; }
		else if (InMemory) { Error = TEXT("Shared avatar asset ") + PackageName(Pair.Key) + TEXT(" is still loaded without its saved file. Restart Unreal before retrying the download."); return false; }
		else if (!Matches(Source(Job, Pair.Key), Pair.Value)) { Error = TEXT("The staged shared support is missing or changed. Download this avatar again."); return false; }
	}
	return true;
}

bool Install(FConvaiAvatarPendingDownload& Job, FString& Error)
{
	check(IsInGameThread());
	Error.Reset();
	if (Job.SupportFiles.IsEmpty()) return true;
	if (!Preflight(Job, Error)) return false;
#if PLATFORM_WINDOWS
	Job.SupportPhase = TEXT("installing");
	if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
	TArray<FString> RelativePaths; Job.SupportFiles.GetKeys(RelativePaths);
	RelativePaths.Sort([](const FString& A, const FString& B)
	{
		const bool AMain = FPaths::GetExtension(A).ToLower() == TEXT("uasset"), BMain = FPaths::GetExtension(B).ToLower() == TEXT("uasset");
		return AMain != BMain ? !AMain : A < B; // Make sidecars available before exposing package headers.
	});
	for (const FString& Relative : RelativePaths)
	{
		const FString ExpectedHash = Job.SupportFiles[Relative];
		const FString File = Target(Relative), Temp = Temporary(Job, Relative);
		if (IFileManager::Get().FileExists(*File))
		{
			if (!Matches(File, ExpectedHash)) { Error = TEXT("A shared support file changed during installation. Its existing files were kept; resolve the conflict before retrying."); return false; }
			continue;
		}
		if (const FString* ExistingIdentity = Job.SupportOwnedIdentities.Find(Relative))
		{
			if (!RemoveOwned(Temp, *ExistingIdentity, ExpectedHash, true, Error)) return false;
			Job.SupportOwnedIdentities.Remove(Relative);
		}
		else if (IFileManager::Get().FileExists(*Temp))
		{
			// A crash between CREATE_NEW and the identity journal can leave only private job
			// scratch behind. This exact manifest-derived scratch path is never host content.
			if (!NoLinks(Temp, Error) || !IFileManager::Get().Delete(*Temp)) { Error = TEXT("The pending support scratch file is locked. Close programs using the uploader cache and retry."); return false; }
		}
		if (!CreateOwnedTemporary(Job, Relative, ExpectedHash, Error)) return false;
		if (!NoLinks(File, Error) || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true)) return false;
		// Move on the same volume only, without replacement or copy fallback. Ownership identity stays stable.
		if (!MoveFileExW(*Temp, *File, MOVEFILE_WRITE_THROUGH)) { Error = TEXT("A shared support destination changed or is locked. Its existing files were kept; retry after resolving the conflict."); return false; }
		if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
	}
	Job.SupportPhase = TEXT("installed");
	return FConvaiAvatarDownloadService::SaveJob(Job, Error);
#else
	Error = TEXT("Installing shared avatar support currently requires Windows."); return false;
#endif
}

bool Rollback(FConvaiAvatarPendingDownload& Job, FString& Error)
{
	check(IsInGameThread());
	Error.Reset();
	if (Job.SupportOwnedIdentities.IsEmpty()) { Job.SupportPhase = TEXT("pending"); return FConvaiAvatarDownloadService::SaveJob(Job, Error); }
	if (!Validate(Job, Error)) return false;
	Job.SupportPhase = TEXT("rolling_back");
	if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
#if PLATFORM_WINDOWS
	TMap<FString, TArray<FString>> Families;
	for (const auto& Pair : Job.SupportFiles) Families.FindOrAdd(PackageName(Pair.Key)).Add(Pair.Key);
	for (const auto& Family : Families)
	{
		TArray<FString> OwnedPaths;
		for (const FString& Relative : Family.Value) if (Job.SupportOwnedIdentities.Contains(Relative)) OwnedPaths.Add(Relative);
		if (OwnedPaths.IsEmpty()) continue;
		struct FLockedFile { HANDLE Handle = INVALID_HANDLE_VALUE; bool Owned = false; FString Path; };
		TArray<FLockedFile> Locked;
		TArray<FString> RemovedPackages;
		ON_SCOPE_EXIT
		{
			for (const auto& File : Locked) if (File.Handle != INVALID_HANDLE_VALUE) CloseHandle(File.Handle);
			// File dispositions take effect when their handles close. UE5.8's modified-file scan
			// removes previous AssetData entries when the package now yields no assets on disk.
			if (!RemovedPackages.IsEmpty())
			{
				IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				Registry.SetTemporaryCachingModeInvalidated();
				Registry.ScanModifiedAssetFiles(RemovedPackages);
			}
		};
		bool Dirty = false, PreserveFamily = false, HasOwnedTarget = false;
		const bool InMemory = Loaded(Family.Value[0], Dirty);
		PreserveFamily = Dirty;
		// Lock and inspect the whole package family before deleting any part. A changed main
		// asset, sidecar, or foreign replacement preserves every owned member of that family.
		for (const FString& Relative : Family.Value)
		{
			const FString File = Target(Relative);
			if (!IFileManager::Get().FileExists(*File)) continue;
			const FString* ExpectedIdentity = Job.SupportOwnedIdentities.Find(Relative);
			HANDLE Handle = CreateFileW(*File, GENERIC_READ | (ExpectedIdentity ? DELETE : 0), FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (Handle == INVALID_HANDLE_VALUE) { Error = TEXT("Shared support is still in use at ") + Family.Key + TEXT(". Restart Unreal to finish the pending rollback; no part of this package was removed."); return false; }
			const bool Owned = ExpectedIdentity && Identity(Handle) == *ExpectedIdentity;
			Locked.Add({Handle, Owned, File});
			if ((ExpectedIdentity && !Owned) || !HandleHash(Handle).Equals(Job.SupportFiles[Relative], ESearchCase::IgnoreCase)) PreserveFamily = true;
			HasOwnedTarget |= Owned;
		}
		if (InMemory && !PreserveFamily && HasOwnedTarget)
		{ Error = TEXT("New shared avatar support is still in use at ") + Family.Key + TEXT(". Restart Unreal to finish the pending rollback; no loaded files were removed."); return false; }
		for (const FString& Relative : OwnedPaths)
			if (!RemoveOwned(Temporary(Job, Relative), Job.SupportOwnedIdentities[Relative], Job.SupportFiles[Relative], true, Error)) return false;
		if (!PreserveFamily) for (const auto& File : Locked) if (File.Owned)
		{
			FILE_DISPOSITION_INFO Disposition{true};
			if (!SetFileInformationByHandle(File.Handle, FileDispositionInfo, &Disposition, sizeof(Disposition)))
			{ Error = TEXT("New shared avatar support could not be removed. Restart Unreal to finish the pending rollback."); return false; }
			if (FPaths::GetExtension(File.Path).ToLower() == TEXT("uasset")) RemovedPackages.Add(File.Path);
		}
		for (const FString& Relative : OwnedPaths) Job.SupportOwnedIdentities.Remove(Relative);
		if (!FConvaiAvatarDownloadService::SaveJob(Job, Error)) return false;
	}
	Job.SupportPhase = TEXT("pending");
	return FConvaiAvatarDownloadService::SaveJob(Job, Error);
#else
	return false;
#endif
}

void RefreshRegistry(const FConvaiAvatarPendingDownload& Job)
{
	if (Job.SupportFiles.IsEmpty()) return;
	TArray<FString> Files;
	for (const auto& Pair : Job.SupportFiles)
		if (FPaths::GetExtension(Pair.Key).ToLower() == TEXT("uasset")) Files.Add(Target(Pair.Key));
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SetTemporaryCachingModeInvalidated();
	Registry.ScanFilesSynchronous(Files, true);
}
}
