// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarDurableFile.h"
#include "Packaging/ConvaiAvatarPackagingPrivate.h"
#include "Algo/AllOf.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Windows/WindowsHWrapper.h"

namespace ConvaiAvatarDurableFile
{
namespace
{
	const TCHAR* const SiblingExtension = TEXT(".tmp");

	FString SiblingOf(const FString& Path)
	{
		return Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + SiblingExtension;
	}

	bool IsSibling(const FString& Name)
	{
		// FGuid's Digits format writes uppercase hex.
		constexpr int32 Digits = 32, Suffix = 1 + Digits + 4;
		if (Name.Len() <= Suffix || !Name.EndsWith(SiblingExtension, ESearchCase::CaseSensitive) || Name[Name.Len() - Suffix] != TEXT('.')) return false;
		return Algo::AllOf(Name.Mid(Name.Len() - Suffix + 1, Digits), [](TCHAR C) { return FChar::IsDigit(C) || (C >= TEXT('A') && C <= TEXT('F')); });
	}

#if WITH_DEV_AUTOMATION_TESTS
	FCriticalSection FaultLock;
	FString FaultPath;

	bool IsFaulted(const FString& Path)
	{
		FScopeLock Lock(&FaultLock);
		return !FaultPath.IsEmpty() && FPaths::IsSamePath(FaultPath, Path);
	}
#endif
}

bool Replace(const FString& Path, TConstArrayView<uint8> Bytes)
{
	if (ConvaiAvatarPackagingPrivate::IsReparsePoint(Path)) return false;
	const FString Destination = ConvaiAvatarPackagingPrivate::NativePath(Path);
	const FString Temporary = ConvaiAvatarPackagingPrivate::NativePath(SiblingOf(Path));
	// CREATE_NEW without following a reparse point: the sibling is always a new ordinary file this call made.
	const HANDLE File = CreateFileW(*Temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (File == INVALID_HANDLE_VALUE) return false;
	DWORD Written = 0;
	const bool bWritten = (Bytes.IsEmpty() || (WriteFile(File, Bytes.GetData(), static_cast<DWORD>(Bytes.Num()), &Written, nullptr) &&
		Written == static_cast<DWORD>(Bytes.Num()))) && FlushFileBuffers(File);
	CloseHandle(File);
#if WITH_DEV_AUTOMATION_TESTS
	if (IsFaulted(Path)) { DeleteFileW(*Temporary); return false; }
#endif
	bool bReplaced = false;
	// A destination another process holds open fails with ERROR_ACCESS_DENIED, whatever its share mode, and a held sibling
	// with ERROR_SHARING_VIOLATION. Scanners and indexers let go within moments; most callers wait on the game thread.
	for (int32 Attempt = 0; bWritten && !bReplaced && Attempt < 8; ++Attempt)
	{
		if (Attempt) FPlatformProcess::Sleep(0.05f);
		bReplaced = MoveFileExW(*Temporary, *Destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		const DWORD Failure = bReplaced ? ERROR_SUCCESS : GetLastError();
		if (Failure != ERROR_ACCESS_DENIED && Failure != ERROR_SHARING_VIOLATION) break;
	}
	if (!bReplaced) DeleteFileW(*Temporary);
	return bReplaced;
}

bool Replace(const FString& Path, FStringView Text, FFileHelper::EEncodingOptions Encoding)
{
	using EEncoding = FFileHelper::EEncodingOptions;
	TArray<uint8> Bytes;
	if (Text.IsEmpty()) return Replace(Path, Bytes);
	if (Encoding == EEncoding::ForceUTF8 || Encoding == EEncoding::ForceUTF8WithoutBOM)
	{
		if (Encoding == EEncoding::ForceUTF8) Bytes.Append({0xEF, 0xBB, 0xBF});
		FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
	else if (Encoding == EEncoding::ForceUnicode || (Encoding == EEncoding::AutoDetect && !FCString::IsPureAnsi(Text.GetData(), Text.Len())))
	{
		Bytes.Append({0xFF, 0xFE});
		FTCHARToUTF16 Utf16(Text.GetData(), Text.Len());
		Bytes.Append(reinterpret_cast<const uint8*>(Utf16.Get()), Utf16.Length() * sizeof(UTF16CHAR));
	}
	else
	{
		auto Ansi = StringCast<ANSICHAR>(Text.GetData(), Text.Len());
		Bytes.Append(reinterpret_cast<const uint8*>(Ansi.Get()), Ansi.Length());
	}
	return Replace(Path, Bytes);
}

TArray<FString> DeleteStraySiblings(const FString& Directory)
{
	TArray<FString> Names, Left;
	IFileManager::Get().FindFiles(Names, *(Directory / TEXT("*") + SiblingExtension), true, false);
	for (const FString& Name : Names)
	{
		const FString File = Directory / Name;
		if (IsSibling(Name) && (ConvaiAvatarPackagingPrivate::IsReparsePoint(File) || !IFileManager::Get().Delete(*File, false, false, true))) Left.Add(File);
	}
	return Left;
}

#if WITH_DEV_AUTOMATION_TESTS
FScopedReplaceFault::FScopedReplaceFault(const FString& Path)
{
	FScopeLock Lock(&FaultLock);
	FaultPath = Path;
}

FScopedReplaceFault::~FScopedReplaceFault()
{
	FScopeLock Lock(&FaultLock);
	FaultPath.Reset();
}
#endif
}
