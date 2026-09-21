// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarSourceRevision.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace ConvaiAvatarWorkspacePrivate
{
	FString Normalized(const FString& Path);
	bool IsReparsePoint(const FString& Path);
}

namespace ConvaiAvatarSourceRevision
{
namespace
{
	const TCHAR* Extensions[] = {TEXT(".uasset"), TEXT(".umap"), TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl")};
	bool Cancel(const TFunction<bool()>& Cancelled, FString& Error)
	{
		if (!Cancelled || !Cancelled()) return false;
		Error = TEXT("Avatar source refresh was cancelled."); return true;
	}
	bool HashFile(const FString& File, FString& Hash, FString& Error)
	{
		if (ConvaiAvatarWorkspacePrivate::IsReparsePoint(File))
		{ Error = TEXT("The avatar contains a redirected file: ") + File; return false; }
		const FMD5Hash Value = FMD5Hash::HashFile(*File);
		if (!Value.IsValid()) { Error = TEXT("Could not read the saved avatar file: ") + File; return false; }
		Hash = LexToString(Value); return true;
	}
	bool Enumerate(const FString& Directory, TArray<FString>& Files, FString& Error)
	{
		// Validate every ancestor, including dangling redirect entries, before walking this tree.
		for (FString Cursor = ConvaiAvatarWorkspacePrivate::Normalized(Directory); !Cursor.IsEmpty();)
		{
			if (ConvaiAvatarWorkspacePrivate::IsReparsePoint(Cursor))
			{ Error = TEXT("The avatar folder is redirected and cannot be refreshed safely: ") + Cursor; return false; }
			const FString Parent = FPaths::GetPath(Cursor);
			if (Parent == Cursor) break;
			Cursor = Parent;
		}
		if (!IFileManager::Get().DirectoryExists(*Directory)) { Error = TEXT("The saved avatar folder is missing: ") + Directory; return false; }
		TArray<FString> Queue {Directory};
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			TArray<FString> Children;
			IFileManager::Get().FindFiles(Children, *(Queue[Index] / TEXT("*")), true, true);
			for (const FString& Child : Children)
			{
				const FString Path = Queue[Index] / Child;
				if (ConvaiAvatarWorkspacePrivate::IsReparsePoint(Path))
				{ Error = TEXT("The avatar contains a redirected path: ") + Path; return false; }
				if (IFileManager::Get().DirectoryExists(*Path)) Queue.Add(Path); else Files.Add(Path);
			}
		}
		Files.Sort(); return true;
	}
	void Changes(const TMap<FString, FString>& Before, const TMap<FString, FString>& After, TArray<FString>& Out)
	{
		for (const auto& Pair : Before) if (After.FindRef(Pair.Key) != Pair.Value) Out.AddUnique(Pair.Key);
		for (const auto& Pair : After) if (Before.FindRef(Pair.Key) != Pair.Value) Out.AddUnique(Pair.Key);
	}
	void AppendMap(const TMap<FString, FString>& Values, FString& Text)
	{
		TArray<FString> Keys; Values.GetKeys(Keys); Keys.Sort();
		for (const FString& Key : Keys) Text += FString::Printf(TEXT("%d:%s=%s\n"), Key.Len(), *Key, *Values[Key]);
	}
}

bool CaptureSource(const TMap<FName, FName>& Packages, TMap<FString, FString>& Files,
	TMap<FName, FString>& LegacyHashes, const TFunction<bool()>& Cancelled, FString& Error)
{
	Files.Reset(); LegacyHashes.Reset();
	for (const auto& Pair : Packages)
	{
		if (Cancel(Cancelled, Error)) return false;
		FString MainFile, Hash;
		if (!FPackageName::DoesPackageExist(Pair.Key.ToString(), &MainFile) || !HashFile(MainFile, Hash, Error))
		{ if (Error.IsEmpty()) Error = TEXT("The original avatar asset is missing: ") + Pair.Key.ToString(); return false; }
		LegacyHashes.Add(Pair.Key, Hash);
		const FString Stem = FPaths::GetBaseFilename(MainFile, false);
		TArray<FString> Family;
		IFileManager::Get().FindFiles(Family, *(Stem + TEXT(".*")), true, false);
		for (const FString& Member : Family)
		{
			const FString File = FPaths::GetPath(MainFile) / Member;
			bool bPackageFile = false;
			for (const TCHAR* Extension : Extensions) if (File.EndsWith(Extension, ESearchCase::IgnoreCase)) { bPackageFile = true; break; }
			if (!bPackageFile) continue;
			if (File != MainFile && !HashFile(File, Hash, Error)) return false;
			if (File == MainFile) Hash = LegacyHashes[Pair.Key];
			Files.Add(Pair.Key.ToString() + File.Mid(Stem.Len()), Hash);
		}
	}
	return true;
}

bool CapturePrepared(const FString& PluginDirectory, TMap<FString, FString>& Files,
	const TFunction<bool()>& Cancelled, FString& Error)
{
	Files.Reset();
	const FString Root = ConvaiAvatarWorkspacePrivate::Normalized(PluginDirectory / TEXT("Content"));
	if (!IFileManager::Get().DirectoryExists(*Root) && IFileManager::Get().DirectoryExists(*PluginDirectory) &&
		!ConvaiAvatarWorkspacePrivate::IsReparsePoint(Root)) return true; // An owned incomplete draft may not have Content yet.
	TArray<FString> Paths;
	if (!Enumerate(Root, Paths, Error)) return false;
	for (const FString& Path : Paths)
	{
		if (Cancel(Cancelled, Error)) return false;
		bool bPackageFile = false;
		for (const TCHAR* Extension : Extensions) if (Path.EndsWith(Extension, ESearchCase::IgnoreCase)) { bPackageFile = true; break; }
		if (!bPackageFile) continue;
		FString Hash;
		if (!HashFile(Path, Hash, Error)) return false;
		Files.Add(Path.Mid(Root.Len() + 1), Hash);
	}
	return true;
}

void Compare(const FConvaiAvatarPreparedAsset& Previous, const FConvaiAvatarPreparedAsset& Current,
	const TMap<FString, FString>& PreparedFiles, FConvaiAvatarSourceReview& Review)
{
	Review = {};
	Review.bLegacyBaseline = Previous.PreparedFileHashes.IsEmpty() || Previous.SourceFileHashes.IsEmpty();
	const bool bHasSourceBaseline = !Previous.SourceFileHashes.IsEmpty() || !Previous.SourcePackageHashes.IsEmpty();
	if (!Previous.SourceFileHashes.IsEmpty()) Changes(Previous.SourceFileHashes, Current.SourceFileHashes, Review.ChangedOriginalFiles);
	else if (bHasSourceBaseline)
	{
		for (const auto& Pair : Previous.SourcePackageHashes)
			if (Current.SourcePackageHashes.FindRef(Pair.Key) != Pair.Value) Review.ChangedOriginalFiles.AddUnique(Pair.Key.ToString());
		for (const auto& Pair : Current.SourcePackageHashes)
			if (Previous.SourcePackageHashes.FindRef(Pair.Key) != Pair.Value) Review.ChangedOriginalFiles.AddUnique(Pair.Key.ToString());
	}
	// Full source-family keys already bind the source graph. The live canonical mapping may
	// have pruned an unused file during a plugin-only edit; that must not look like a source edit.
	if (Previous.SourceFileHashes.IsEmpty() && bHasSourceBaseline)
	{
		// The live ownership map can also contain SDK copies introduced by prepared-only
		// edits. Legacy source hashes identify which mappings actually belong to the
		// original graph; ownership alone must not manufacture an original-side change.
		for (const auto& Pair : Previous.SourceToDestinationPackages)
			if (Previous.SourcePackageHashes.Contains(Pair.Key) && Current.SourceToDestinationPackages.FindRef(Pair.Key) != Pair.Value) Review.ChangedOriginalFiles.AddUnique(Pair.Key.ToString());
		for (const auto& Pair : Current.SourceToDestinationPackages)
			if (Current.SourcePackageHashes.Contains(Pair.Key) && Previous.SourceToDestinationPackages.FindRef(Pair.Key) != Pair.Value) Review.ChangedOriginalFiles.AddUnique(Pair.Key.ToString());
	}
	if (Previous.bIsMetaHuman != Current.bIsMetaHuman) Review.ChangedOriginalFiles.Add(TEXT("MetaHuman option"));
	if (Previous.bIncludeConvaiContent != Current.bIncludeConvaiContent) Review.ChangedOriginalFiles.Add(TEXT("Include Convai content option"));
	// Missing history does not prove that any prepared file was edited.
	if (!Previous.PreparedFileHashes.IsEmpty()) Changes(Previous.PreparedFileHashes, PreparedFiles, Review.ChangedPreparedFiles);
	Review.ChangedOriginalFiles.Sort(); Review.ChangedPreparedFiles.Sort();
	Review.bOriginalChanged = !Review.ChangedOriginalFiles.IsEmpty();
	Review.bPreparedChanged = !Review.ChangedPreparedFiles.IsEmpty();
	Review.bNeedsRefresh = Review.bOriginalChanged || !bHasSourceBaseline;
	Review.bRequiresConfirmation = Review.bNeedsRefresh && Review.bPreparedChanged;
	FString Identity = Previous.AssetId + TEXT("\n") + Previous.PluginName + TEXT("\n") + Previous.OriginalEntryPoint.ToString() + TEXT("\n");
	AppendMap(Current.SourceFileHashes, Identity); Identity += TEXT("--prepared--\n"); AppendMap(PreparedFiles, Identity);
	Identity += TEXT("--baseline--\n"); AppendMap(Previous.SourceFileHashes, Identity); AppendMap(Previous.PreparedFileHashes, Identity);
	// Include the prior/current graph, including its remap identities, in exact consent.
	TMap<FString, FString> Mapping;
	for (const auto& Pair : Current.SourceToDestinationPackages) Mapping.Add(Pair.Key.ToString(), Pair.Value.ToString());
	AppendMap(Mapping, Identity);
	Identity += Current.bIsMetaHuman ? TEXT("\nmetahuman=true") : TEXT("\nmetahuman=false");
	Identity += Current.bIncludeConvaiContent ? TEXT("\ninclude_convai_content=true") : TEXT("\ninclude_convai_content=false");
	FTCHARToUTF8 Utf8(*Identity);
	Review.ConflictFingerprint = FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

}
