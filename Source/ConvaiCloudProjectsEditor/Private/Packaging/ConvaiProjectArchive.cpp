// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiProjectArchive.h"
#include "Misc/EngineVersionComparison.h"

#include "FileUtilities/ZipArchiveWriter.h"
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
#include "FileUtilities/ZipArchiveReader.h"
#endif
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ConvaiProjectArchivePrivate
{
/** Room kept beyond the archive itself, so compressing never fills the drive to its last byte. */
constexpr int64 FreeSpaceMarginBytes = 256ll * 1024 * 1024;

/** Empty when the drive that will hold OutputZip has room for PayloadBytes, or cannot say. */
FString FreeSpaceError(const FString& OutputZip, int64 PayloadBytes)
{
	const FString Directory = FPaths::GetPath(OutputZip);
	const uint64 Needed = static_cast<uint64>(FMath::Max<int64>(PayloadBytes, 0) + FreeSpaceMarginBytes);
	uint64 TotalSpace = 0;
	uint64 FreeSpace = 0;
	if (!FPlatformMisc::GetDiskTotalAndFreeSpace(Directory, TotalSpace, FreeSpace) || FreeSpace >= Needed) return FString();

	return FString::Printf(
		TEXT("Compressing the build needs about %.1f GB free on the drive that holds %s, but only %.1f GB is free. ")
		TEXT("Free up space and upload again."), Needed / 1073741824.0, *Directory, FreeSpace / 1073741824.0);
}
}

bool FConvaiProjectArchive::IsCompleteArchive(const FString& ZipPath, int32 ExpectedEntries)
{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
	// FZipArchiveReader is confirmed from UE 5.4 on; older engines trust the writer as before.
	return true;
#else
	const FZipArchiveReader Reader(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*ZipPath));
	return Reader.IsValid() && Reader.GetFileNames().Num() == ExpectedEntries;
#endif
}

bool FConvaiProjectArchive::ZipDirectory(const FString& SourceDirectory, const FString& OutputZip,
	const TSharedPtr<TAtomic<bool>>& CancelFlag, FProgress Progress, FString& OutError)
{
	OutError.Reset();

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*SourceDirectory))
	{
		OutError = FString::Printf(TEXT("The packaged build was not found at %s."), *SourceDirectory);
		return false;
	}

	TArray<FString> RelativeFiles;
	FileManager.FindFilesRecursive(RelativeFiles, *SourceDirectory, TEXT("*"), true, false);
	if (RelativeFiles.IsEmpty())
	{
		OutError = TEXT("The packaged build is empty. Check the packaging log and try again.");
		return false;
	}

	int64 TotalBytes = 0;
	for (const FString& File : RelativeFiles)
	{
		const int64 Size = FileManager.FileSize(*File);
		if (Size > MaxEntryBytes)
		{
			OutError = FString::Printf(
				TEXT("'%s' is %.1f GB, larger than the %.1f GB this archiver supports for a single file. ")
				TEXT("Package with a smaller pak (or split chunks) and upload again."),
				*FPaths::GetCleanFilename(File), Size / 1073741824.0, MaxEntryBytes / 1073741824.0);
			return false;
		}
		TotalBytes += FMath::Max<int64>(Size, 0);
	}

	FileManager.Delete(*OutputZip, false, true, true);
	OutError = ConvaiProjectArchivePrivate::FreeSpaceError(OutputZip, TotalBytes);
	if (!OutError.IsEmpty()) return false;

	IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*OutputZip);
	if (!Handle)
	{
		OutError = FString::Printf(TEXT("The archive could not be created at %s."), *OutputZip);
		return false;
	}

	// The writer takes ownership of the handle and finalises the central directory on destruction,
	// so it has to go out of scope before the archive is read or deleted.
	bool bFailed = false;
	{
		// Stored, not deflated: the build is mostly paks cooked with -compressed, which deflate barely shrinks
		// while it holds another copy of each entry in memory.
		FZipArchiveWriter Writer(Handle);
		int64 WrittenBytes = 0;

		for (const FString& File : RelativeFiles)
		{
			if (CancelFlag.IsValid() && CancelFlag->Load())
			{
				OutError = TEXT("The upload was cancelled.");
				bFailed = true;
				break;
			}

			TArray<uint8> Contents;
			if (!FFileHelper::LoadFileToArray(Contents, *File))
			{
				OutError = FString::Printf(TEXT("'%s' could not be read while building the archive."), *File);
				bFailed = true;
				break;
			}

			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *(SourceDirectory / TEXT("")));
			Writer.AddFile(Relative.Replace(TEXT("\\"), TEXT("/")), Contents, FileManager.GetTimeStamp(*File));

			WrittenBytes += Contents.Num();
			if (Progress && TotalBytes > 0) Progress(FMath::Clamp(static_cast<float>(WrittenBytes) / TotalBytes, 0.f, 1.f));
		}
	}

	if (bFailed)
	{
		FileManager.Delete(*OutputZip, false, true, true);
		return false;
	}

	// The writer keeps no failure state: after a failed write it drops the rest, central directory included.
	if (!IsCompleteArchive(OutputZip, RelativeFiles.Num()))
	{
		FileManager.Delete(*OutputZip, false, true, true);
		OutError = FString::Printf(
			TEXT("The archive at %s could not be written completely, most likely because the drive ran out of free space. ")
			TEXT("Free up space and upload again."), *OutputZip);
		return false;
	}
	return true;
}
