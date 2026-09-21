// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarSaveReview.h"
#include "UObject/Package.h"

namespace ConvaiAvatarSaveReview
{
    FString PackageList(const TArray<UPackage*>& Packages)
    {
        TArray<FString> Names;
        for (const UPackage* Package : Packages) if (Package) Names.AddUnique(Package->GetName());
        Names.Sort();
        return FString::Join(Names, TEXT("\n"));
    }

    EOutcome Evaluate(FEditorFileUtils::EPromptReturnCode Result, const TArray<UPackage*>& ReviewedPackages,
        const TArray<UPackage*>& FailedPackages, bool bOperationCurrent, bool bCancelled, FString& Error)
    {
        Error.Reset();
        if (!bOperationCurrent) return EOutcome::Superseded;
        if (bCancelled || Result == FEditorFileUtils::PR_Cancelled || Result == FEditorFileUtils::PR_Declined) return EOutcome::Cancelled;
        TArray<UPackage*> Outstanding = FailedPackages;
        for (UPackage* Package : ReviewedPackages) if (Package && Package->IsDirty()) Outstanding.AddUnique(Package);
        if (Result != FEditorFileUtils::PR_Success || !Outstanding.IsEmpty())
        {
            Error = TEXT("Some avatar files were not saved. Save the remaining files, resolve any reported checkout or file-permission problem, then retry the upload.");
            if (!Outstanding.IsEmpty()) Error += TEXT("\n") + PackageList(Outstanding);
            return EOutcome::Failed;
        }
        return EOutcome::Continue;
    }
}
