// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "FileHelpers.h"

class UPackage;
namespace ConvaiAvatarSaveReview
{
    enum class EOutcome { Continue, Cancelled, Failed, Superseded };
    /** Sorted package names only; never gathers global editor dirties. */
    FString PackageList(const TArray<UPackage*>& Packages);
    /** Saving a subset or receiving a stale modal result cannot authorize preparation. */
    EOutcome Evaluate(FEditorFileUtils::EPromptReturnCode Result, const TArray<UPackage*>& ReviewedPackages,
        const TArray<UPackage*>& FailedPackages, bool bOperationCurrent, bool bCancelled, FString& Error);
    /** Rescans the saved packages' files. An editor save refreshes only their asset tags; the registry rereads their
     * dependencies on a later editor tick, and the upload reads them before one comes. */
    void ScanSaved(const TArray<UPackage*>& Packages);
}
