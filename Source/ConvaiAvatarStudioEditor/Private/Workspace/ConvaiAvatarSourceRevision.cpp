// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarSourceRevision.h"
#include "Packaging/ConvaiAvatarPackagingPrivate.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/SecureHash.h"

namespace ConvaiAvatarWorkspacePrivate
{
	FString Normalized(const FString& Path);
}

namespace ConvaiAvatarSourceRevision
{
namespace
{
	const TCHAR* Extensions[] = {TEXT(".uasset"), TEXT(".umap"), TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl")};
	/** A capture given no Cancelled, as a failed attempt's cleanup is, always finishes, whatever was pressed. */
	bool Cancel(const TFunction<bool()>& Cancelled, const FSlowTask& Progress, FString& Error)
	{
		if (!Cancelled || (!Cancelled() && !Progress.ShouldCancel())) return false;
		Error = TEXT("Avatar source refresh was cancelled."); return true;
	}
	struct FHashMemoEntry { FString Stamp; FString Hash; };
	/** Set while an FHashMemoScope is open. */
	TOptional<TMap<FString, FHashMemoEntry>> HashMemo;
	bool HashFile(const FString& File, FString& Hash, FString& Error)
	{
		if (ConvaiAvatarPackagingPrivate::IsReparsePoint(File))
		{ Error = TEXT("The avatar contains a redirected file: ") + File; return false; }
		TMap<FString, FHashMemoEntry>* Memo = IsInGameThread() ? HashMemo.GetPtrOrNull() : nullptr;
		const FString Key = Memo ? ConvaiAvatarWorkspacePrivate::Normalized(File) : FString();
		// Stamped before reading: a save while the bytes are read leaves a stamp the next capture no longer matches.
		const FString Stamp = Memo ? ConvaiAvatarPackagingPrivate::FileStamp(File) : FString();
		if (const FHashMemoEntry* Known = Memo && !Stamp.IsEmpty() ? Memo->Find(Key) : nullptr; Known && Known->Stamp == Stamp)
		{ Hash = Known->Hash; return true; }
		const FMD5Hash Value = FMD5Hash::HashFile(*File);
		if (!Value.IsValid()) { Error = TEXT("Could not read the saved avatar file: ") + File; return false; }
#if WITH_DEV_AUTOMATION_TESTS
		++HashedFiles; HashedBytes += IFileManager::Get().FileSize(*File);
#endif
		Hash = LexToString(Value);
		if (Memo && !Stamp.IsEmpty()) Memo->Add(Key, {Stamp, Hash});
		return true;
	}
	bool Enumerate(const FString& Directory, TArray<FString>& Files, FString& Error)
	{
		// Validate every ancestor, including dangling redirect entries, before walking this tree.
		for (FString Cursor = ConvaiAvatarWorkspacePrivate::Normalized(Directory); !Cursor.IsEmpty();)
		{
			if (ConvaiAvatarPackagingPrivate::IsReparsePoint(Cursor))
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
				if (ConvaiAvatarPackagingPrivate::IsReparsePoint(Path))
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

#if WITH_DEV_AUTOMATION_TESTS
int64 HashedFiles = 0;
int64 HashedBytes = 0;
#endif

FHashMemoScope::FHashMemoScope()
{
	check(IsInGameThread());
	if (HashMemo) return;
	HashMemo.Emplace();
	bOutermost = true;
}

FHashMemoScope::~FHashMemoScope()
{
	if (bOutermost) HashMemo.Reset();
}

bool CaptureSource(const TMap<FName, FName>& Packages, TMap<FString, FString>& Files,
	TMap<FName, FString>& LegacyHashes, const TFunction<bool()>& Cancelled, FString& Error)
{
	TMap<FString, FString> CapturedFiles;
	TMap<FName, FString> CapturedHashes;
	FScopedSlowTask Progress(static_cast<float>(Packages.Num()), NSLOCTEXT("ConvaiAvatarStudio", "CheckOriginalFiles", "Checking the original avatar files…"));
	Progress.MakeDialogDelayed(0.5f, static_cast<bool>(Cancelled));
	for (const auto& Pair : Packages)
	{
		if (Cancel(Cancelled, Progress, Error)) return false;
		Progress.EnterProgressFrame();
		FString MainFile, Hash;
		if (!FPackageName::DoesPackageExist(Pair.Key.ToString(), &MainFile) || !HashFile(MainFile, Hash, Error))
		{ if (Error.IsEmpty()) Error = TEXT("The original avatar asset is missing: ") + Pair.Key.ToString(); return false; }
		CapturedHashes.Add(Pair.Key, Hash);
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
			if (File == MainFile) Hash = CapturedHashes[Pair.Key];
			CapturedFiles.Add(Pair.Key.ToString() + File.Mid(Stem.Len()), Hash);
		}
	}
	Files = MoveTemp(CapturedFiles); LegacyHashes = MoveTemp(CapturedHashes);
	return true;
}

bool CapturePrepared(const FString& PluginDirectory, TMap<FString, FString>& Files,
	const TFunction<bool()>& Cancelled, FString& Error)
{
	const FString Root = ConvaiAvatarWorkspacePrivate::Normalized(PluginDirectory / TEXT("Content"));
	if (!IFileManager::Get().DirectoryExists(*Root) && IFileManager::Get().DirectoryExists(*PluginDirectory) &&
		!ConvaiAvatarPackagingPrivate::IsReparsePoint(Root)) { Files.Reset(); return true; } // An owned incomplete draft may not have Content yet.
	TArray<FString> Paths;
	if (!Enumerate(Root, Paths, Error)) return false;
	FScopedSlowTask Progress(static_cast<float>(Paths.Num()), NSLOCTEXT("ConvaiAvatarStudio", "CheckPreparedFiles", "Checking the prepared avatar files…"));
	Progress.MakeDialogDelayed(0.5f, static_cast<bool>(Cancelled));
	TMap<FString, FString> Captured;
	for (const FString& Path : Paths)
	{
		if (Cancel(Cancelled, Progress, Error)) return false;
		Progress.EnterProgressFrame();
		bool bPackageFile = false;
		for (const TCHAR* Extension : Extensions) if (Path.EndsWith(Extension, ESearchCase::IgnoreCase)) { bPackageFile = true; break; }
		if (!bPackageFile) continue;
		FString Hash;
		if (!HashFile(Path, Hash, Error)) return false;
		Captured.Add(Path.Mid(Root.Len() + 1), Hash);
	}
	Files = MoveTemp(Captured);
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

TMap<FString, FString> AdvancePreparedBaseline(const TMap<FString, FString>& Baseline,
	const TMap<FString, FString>& BeforeWrites, const TMap<FString, FString>& Now)
{
	TMap<FString, FString> Advanced = Baseline;
	if (Baseline.IsEmpty()) return Advanced; // Missing history is not evidence of edits; never fabricate a partial one.
	const auto Same = [](const FString* A, const FString* B) { return A ? B && *A == *B : !B; };
	TSet<FString> Files;
	for (const auto& Pair : BeforeWrites) Files.Add(Pair.Key);
	for (const auto& Pair : Now) Files.Add(Pair.Key);
	for (const FString& File : Files)
	{
		const FString* Before = BeforeWrites.Find(File);
		const FString* After = Now.Find(File);
		if (!Same(Before, Baseline.Find(File)) || Same(Before, After)) continue;
		if (After) Advanced.Add(File, *After); else Advanced.Remove(File);
	}
	return Advanced;
}

}
