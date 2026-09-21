// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarWorkspaceLinks.h"
#include "Workspace/ConvaiAvatarWorkspace.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winioctl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace ConvaiAvatarWorkspaceLinks
{
namespace
{
	FString Full(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Result);
		FPaths::CollapseRelativeDirectories(Result);
		return Result;
	}
	FString SourceRelative(const FString& Name) { return TEXT("Plugins/ConvaiAvatars") / Name; }
	FString LinkRelative(const FString& Name) { return TEXT("Saved/ConvaiAvatarStudio/Uploader/Plugins/ConvaiAvatars") / Name; }
	FString ReceiptsRelative() { return TEXT("Saved/ConvaiAvatarStudio/Uploader/AvatarLinks"); }

#if PLATFORM_WINDOWS
	bool Resolve(const FString& Path, FString& Result)
	{
		HANDLE Handle = CreateFileW(*Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (Handle == INVALID_HANDLE_VALUE) return false;
		WCHAR Buffer[32768];
		const DWORD Size = GetFinalPathNameByHandleW(Handle, Buffer, UE_ARRAY_COUNT(Buffer), FILE_NAME_NORMALIZED);
		CloseHandle(Handle);
		if (!Size || Size >= UE_ARRAY_COUNT(Buffer)) return false;
		Result = Buffer;
		if (Result.StartsWith(TEXT("\\\\?\\UNC\\"))) Result = TEXT("\\\\") + Result.Mid(8);
		else if (Result.StartsWith(TEXT("\\\\?\\"))) Result.RightChopInline(4);
		Result = Full(Result);
		return true;
	}

	/** Never walk through a redirected child, including a dangling link. The project root may have a normal alias. */
	bool LocalDirectory(const FString& Root, const FString& Path, bool bCreate, FString& Error)
	{
		const FString Absolute = Full(Path), Project = Full(Root);
		FString ResolvedRoot;
		if (!Absolute.StartsWith(Project + TEXT("/"), ESearchCase::IgnoreCase) || !Resolve(Project, ResolvedRoot))
		{ Error = TEXT("The uploader folders must stay inside this project. Check the project location and retry."); return false; }
		TArray<FString> Parts;
		Absolute.Mid(Project.Len() + 1).ParseIntoArray(Parts, TEXT("/"), true);
		FString Current = Project, Expected = ResolvedRoot;
		for (const FString& Part : Parts)
		{
			Current /= Part; Expected /= Part;
			DWORD Attributes = GetFileAttributesW(*Current);
			if (Attributes == INVALID_FILE_ATTRIBUTES && bCreate)
			{
				if (!CreateDirectoryW(*Current, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
				{ Error = TEXT("Could not create the uploader folder. Check folder permissions: ") + Current; return false; }
				Attributes = GetFileAttributesW(*Current);
			}
			FString Resolved;
			if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & FILE_ATTRIBUTE_DIRECTORY) ||
				(Attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !Resolve(Current, Resolved) || !Resolved.Equals(Expected, ESearchCase::IgnoreCase))
			{ Error = TEXT("An uploader or avatar folder is missing or points elsewhere. Restore a normal folder inside this project and retry: ") + Current; return false; }
		}
		return true;
	}

	bool ReadReparse(HANDLE Handle, const FString& Link, FString& Fingerprint, FString& Destination)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
		DWORD Count = 0;
		BY_HANDLE_FILE_INFORMATION Info{};
		if (!GetFileInformationByHandle(Handle, &Info) || !(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			!(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
			!DeviceIoControl(Handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, Bytes.GetData(), Bytes.Num(), &Count, nullptr) || Count < 8) return false;
		DWORD Tag = 0;
		FMemory::Memcpy(&Tag, Bytes.GetData(), sizeof(Tag));
		if (Tag != IO_REPARSE_TAG_MOUNT_POINT && Tag != IO_REPARSE_TAG_SYMLINK) return false;
		const uint32 PathOffset = Tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
		if (Count < PathOffset) return false;
		WORD Offset = 0, Length = 0;
		FMemory::Memcpy(&Offset, Bytes.GetData() + 8, sizeof(Offset));
		FMemory::Memcpy(&Length, Bytes.GetData() + 10, sizeof(Length));
		if (!Length || Offset % sizeof(WCHAR) || Length % sizeof(WCHAR) || PathOffset + Offset + Length > Count) return false;
		FString Target(Length / sizeof(WCHAR), reinterpret_cast<const WCHAR*>(Bytes.GetData() + PathOffset + Offset));
		DWORD Flags = 0;
		if (Tag == IO_REPARSE_TAG_SYMLINK) FMemory::Memcpy(&Flags, Bytes.GetData() + 16, sizeof(Flags));
		if (Flags & ~1u) return false;
		if (Flags & 1u)
		{
			if (!FPaths::IsRelative(Target)) return false;
			Destination = Full(FPaths::GetPath(Link) / Target);
		}
		else
		{
			if (Target.StartsWith(TEXT("\\??\\UNC\\"))) Target = TEXT("\\\\") + Target.Mid(8);
			else if (Target.StartsWith(TEXT("\\??\\"))) Target.RightChopInline(4);
			if (FPaths::IsRelative(Target)) return false;
			Destination = Full(Target);
		}
		Fingerprint = FMD5::HashBytes(Bytes.GetData(), Count);
		return true;
	}

	bool ReadReceipt(const FString& Root, const FString& Name, FString& Fingerprint, bool& bExists, FString& Error)
	{
		const FString Path = ReceiptPath(Root, Name);
		const DWORD Attributes = GetFileAttributesW(*Path);
		bExists = Attributes != INVALID_FILE_ATTRIBUTES;
		if (!bExists) return true;
		FString Text, Plugin, Source, Link;
		TSharedPtr<FJsonObject> Object;
		double Version = 0;
		if ((Attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || IFileManager::Get().FileSize(*Path) > 4096 ||
			!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object ||
			!Object->TryGetNumberField(TEXT("version"), Version) || Version != 1 ||
			!Object->TryGetStringField(TEXT("plugin_name"), Plugin) || Plugin != Name ||
			!Object->TryGetStringField(TEXT("source_relative"), Source) || Source != SourceRelative(Name) ||
			!Object->TryGetStringField(TEXT("link_relative"), Link) || Link != LinkRelative(Name) ||
			!Object->TryGetStringField(TEXT("reparse_fingerprint"), Fingerprint) || Fingerprint.Len() != 32 ||
			Fingerprint.GetCharArray().ContainsByPredicate([](TCHAR C) { return C != 0 && !FChar::IsHexDigit(C); }))
		{ Error = TEXT("The uploader shortcut's ownership record is damaged or belongs to another avatar. Keep the avatar plugin and restore the uploader cache from a known copy: ") + Path; return false; }
		return true;
	}

	bool SaveReceipt(const FString& Root, const FString& Name, const FString& Fingerprint, FString& Error)
	{
		const FString Path = ReceiptPath(Root, Name), Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("version"), 1);
		Object->SetStringField(TEXT("plugin_name"), Name);
		Object->SetStringField(TEXT("source_relative"), SourceRelative(Name));
		Object->SetStringField(TEXT("link_relative"), LinkRelative(Name));
		Object->SetStringField(TEXT("reparse_fingerprint"), Fingerprint);
		FString Text;
		if (!FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text)) ||
			!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
			!MoveFileExW(*Temporary, *Path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			Error = TEXT("The uploader shortcut was checked, but its recovery record could not be saved. Check folder permissions and retry.");
			return false;
		}
		return true;
	}
#endif
}

FString ReceiptPath(const FString& Root, const FString& Name) { return Full(Root / ReceiptsRelative() / (Name + TEXT(".json"))); }

#if PLATFORM_WINDOWS
bool CreateLink(const FString& Source, const FString& Link, bool bForceJunction, FString& Error)
{
	if (!bForceJunction)
	{
		FString Relative = Full(Source);
		if (FPaths::MakePathRelativeTo(Relative, *Full(Link)))
		{
			Relative.ReplaceInline(TEXT("/"), TEXT("\\"));
			// ALLOW_UNPRIVILEGED_CREATE uses an existing Windows capability; it never enables Developer Mode.
			if (CreateSymbolicLinkW(*Link, *Relative, SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) ||
				(GetLastError() == ERROR_INVALID_PARAMETER && CreateSymbolicLinkW(*Link, *Relative, SYMBOLIC_LINK_FLAG_DIRECTORY))) return true;
		}
	}
	if (!CreateDirectoryW(*Link, nullptr)) { Error = TEXT("Could not create the uploader shortcut. A folder or link may already use its name: ") + Link; return false; }
	HANDLE Handle = CreateFileW(*Link, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Handle == INVALID_HANDLE_VALUE) { RemoveDirectoryW(*Link); Error = TEXT("Could not open the new uploader shortcut. Check folder permissions and retry."); return false; }
	struct FMountPointBuffer { DWORD Tag; WORD DataLength, Reserved, SubstituteOffset, SubstituteLength, PrintOffset, PrintLength; WCHAR Paths[1]; };
	const FString WindowsSource = Full(Source).Replace(TEXT("/"), TEXT("\\"));
	const FString Substitute = WindowsSource.StartsWith(TEXT("\\\\")) ? TEXT("\\??\\UNC\\") + WindowsSource.Mid(2) : TEXT("\\??\\") + WindowsSource;
	const int32 DataBytes = 8 + (Substitute.Len() + WindowsSource.Len() + 2) * sizeof(WCHAR);
	if (8 + DataBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) { CloseHandle(Handle); RemoveDirectoryW(*Link); Error = TEXT("This project path is too long for an uploader shortcut. Move the project to a shorter path and retry."); return false; }
	TArray<uint8> Storage; Storage.SetNumZeroed(8 + DataBytes);
	auto* Buffer = reinterpret_cast<FMountPointBuffer*>(Storage.GetData());
	Buffer->Tag = IO_REPARSE_TAG_MOUNT_POINT; Buffer->DataLength = static_cast<WORD>(DataBytes);
	Buffer->SubstituteLength = static_cast<WORD>(Substitute.Len() * sizeof(WCHAR));
	Buffer->PrintOffset = Buffer->SubstituteLength + sizeof(WCHAR); Buffer->PrintLength = static_cast<WORD>(WindowsSource.Len() * sizeof(WCHAR));
	FMemory::Memcpy(Buffer->Paths, *Substitute, Buffer->SubstituteLength);
	FMemory::Memcpy(reinterpret_cast<uint8*>(Buffer->Paths) + Buffer->PrintOffset, *WindowsSource, Buffer->PrintLength);
	DWORD Returned = 0;
	const bool bSuccess = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT, Buffer, Storage.Num(), nullptr, 0, &Returned, nullptr) != 0;
	const DWORD Failure = bSuccess ? ERROR_SUCCESS : GetLastError();
	CloseHandle(Handle);
	if (!bSuccess) { RemoveDirectoryW(*Link); Error = FString::Printf(TEXT("Windows could not create the uploader shortcut (error %lu). Use a local NTFS project folder and retry."), Failure); }
	return bSuccess;
}
#endif

bool Ensure(const FString& ProjectRoot, const FString& PluginName, FString& Error, bool bForceJunction)
{
	Error.Reset();
	if (!FConvaiAvatarWorkspace::IsSafePluginName(PluginName)) { Error = TEXT("This avatar has an invalid plugin folder name. Download or prepare it again."); return false; }
#if PLATFORM_WINDOWS
	const FString Root = Full(ProjectRoot), Source = Root / SourceRelative(PluginName), Link = Root / LinkRelative(PluginName);
	if (!LocalDirectory(Root, FPaths::GetPath(Link), true, Error) ||
		!LocalDirectory(Root, Root / ReceiptsRelative(), true, Error)) return false;
	if (!LocalDirectory(Root, Source, false, Error))
	{
		const DWORD LinkAttributes = GetFileAttributesW(*Link);
		Error += TEXT(" Restore this project's avatar folder before uploading this avatar.");
		if (LinkAttributes != INVALID_FILE_ATTRIBUTES && (LinkAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (LinkAttributes & FILE_ATTRIBUTE_DIRECTORY))
			Error += TEXT(" If you intentionally removed that avatar, close Unreal and remove only its leftover uploader shortcut before uploading other avatars. Keep any avatar files. Shortcut: ") + Link;
		return false;
	}
	FString SourceResolved;
	if (!Resolve(Source, SourceResolved)) { Error = TEXT("The avatar folder could not be opened. Check that this project's avatar files are available and retry."); return false; }
	FString Recorded;
	bool bReceiptExists = false;
	if (!ReadReceipt(Root, PluginName, Recorded, bReceiptExists, Error)) return false;
	const DWORD Attributes = GetFileAttributesW(*Link);
	if (Attributes != INVALID_FILE_ATTRIBUTES)
	{
		// No FILE_SHARE_DELETE: inspection and link-only deletion operate on the same directory entry.
		HANDLE Handle = CreateFileW(*Link, GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (Handle == INVALID_HANDLE_VALUE) { Error = TEXT("The uploader shortcut is in use or cannot be opened. Close other uploader processes and retry: ") + Link; return false; }
		ON_SCOPE_EXIT { CloseHandle(Handle); };
		FString Fingerprint, Target;
		if (!(Attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ReadReparse(Handle, Link, Fingerprint, Target))
		{ Error = TEXT("A different folder occupies the uploader shortcut. Move that folder out of the uploader cache and retry. Your prepared avatar was kept: ") + Link; return false; }
		// Read the reparse target itself first. Never follow a stale/unknown link into another project.
		const bool bNamesCurrentSource = Target.Equals(Source, ESearchCase::IgnoreCase) || Target.Equals(SourceResolved, ESearchCase::IgnoreCase);
		const bool bCurrentTarget = bNamesCurrentSource && Resolve(Link, Target) && Target.Equals(SourceResolved, ESearchCase::IgnoreCase);
		if (bCurrentTarget) return SaveReceipt(Root, PluginName, Fingerprint, Error);
		if (!bReceiptExists || Recorded != Fingerprint)
		{
			Error = TEXT("This uploader shortcut points to another project or a missing folder, and its ownership cannot be verified. Upload stopped; your avatar files were kept. Close Unreal, remove only this shortcut (not the avatar plugin), reopen this project and retry: ") + Link;
			return false;
		}
		FILE_DISPOSITION_INFO Disposition{true};
		if (!SetFileInformationByHandle(Handle, FileDispositionInfo, &Disposition, sizeof(Disposition)))
		{ Error = TEXT("The old uploader shortcut could not be replaced. Close programs using the uploader and retry. Both projects' avatar files were kept."); return false; }
	}
	// The existing entry's handle is closed above before creating a new one. No recursive deletion occurs.
	if (!CreateLink(Source, Link, bForceJunction, Error)) return false;
	HANDLE NewLink = CreateFileW(*Link, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (NewLink == INVALID_HANDLE_VALUE) { Error = TEXT("The new uploader shortcut could not be checked. Upload stopped; retry after checking folder permissions."); return false; }
	ON_SCOPE_EXIT { CloseHandle(NewLink); };
	FString Fingerprint, Target;
	if (!ReadReparse(NewLink, Link, Fingerprint, Target) ||
		(!Target.Equals(Source, ESearchCase::IgnoreCase) && !Target.Equals(SourceResolved, ESearchCase::IgnoreCase)) ||
		!Resolve(Link, Target) || !Target.Equals(SourceResolved, ESearchCase::IgnoreCase))
	{ Error = TEXT("The uploader shortcut does not point to this project's avatar. Upload stopped; restore the uploader cache and retry."); return false; }
	return SaveReceipt(Root, PluginName, Fingerprint, Error);
#else
	Error = TEXT("Linked Cloud Avatars uploader workspaces currently require Windows.");
	return false;
#endif
}

bool EnsureAll(const FString& ProjectRoot, const FString& SelectedPlugin, FString& Error)
{
	if (!Ensure(ProjectRoot, SelectedPlugin, Error)) return false;
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *(Full(ProjectRoot) / TEXT("Saved/ConvaiAvatarStudio/Uploader/Plugins/ConvaiAvatars/*")), false, true);
	for (const FString& Name : Names)
	{
		if (Name != SelectedPlugin && !Ensure(ProjectRoot, Name, Error)) return false;
	}
	return true;
}
}
