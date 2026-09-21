// Copyright Convai Inc. All Rights Reserved.
#include "Packaging/ConvaiProjectArchive.h"
#include "Misc/EngineVersionComparison.h"

#include "FileUtilities/ZipArchiveWriter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        FZipArchiveWriter Writer(Handle); // Legacy UE writes valid, uncompressed ZIP entries.
#else
        FZipArchiveWriter Writer(Handle, EZipArchiveOptions::Deflate);
#endif
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
	return true;
}
