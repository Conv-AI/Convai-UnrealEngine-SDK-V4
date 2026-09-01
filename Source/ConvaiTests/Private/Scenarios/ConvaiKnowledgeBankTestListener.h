// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"
#include "UObject/Object.h"
#include "ConvaiKnowledgeBankTestListener.generated.h"

/**
 * Where the Knowledge Bank REST proxies deliver their dynamic delegates for
 * knowledge_bank_lifecycle.
 *
 * A UObject only because AddDynamic needs one; the scenario is a plain class
 * and owns the phase machine. This records the last reply and its payload and
 * the scenario's Poll takes it. No lock: HTTP completion is delivered on the
 * game thread, the same thread that polls.
 */
UCLASS()
class UConvaiKnowledgeBankTestListener : public UObject
{
    GENERATED_BODY()

public:
    enum class EOutcome
    {
        None,
        Uploaded,
        UploadFailed,
        Listed,
        ListFailed,
        StatusSet,
        StatusFailed,
        Deleted,
        DeleteFailed
    };

    UFUNCTION()
    void HandleUploaded(const FConvaiKnowledgeBankDocument& Document);

    UFUNCTION()
    void HandleUploadFailed(const FConvaiKnowledgeBankDocument& Document);

    UFUNCTION()
    void HandleListed(const TArray<FConvaiKnowledgeBankDocument>& Documents);

    UFUNCTION()
    void HandleListFailed(const TArray<FConvaiKnowledgeBankDocument>& Documents);

    UFUNCTION()
    void HandleStatusSet(FString ResponseString);

    UFUNCTION()
    void HandleStatusFailed(FString ResponseString);

    UFUNCTION()
    void HandleDeleted(FString ResponseString);

    UFUNCTION()
    void HandleDeleteFailed(FString ResponseString);

    /** The reply since the last Take, cleared so one reply is consumed once. */
    EOutcome Take();

    FConvaiKnowledgeBankDocument UploadedDocument;
    TArray<FConvaiKnowledgeBankDocument> Listing;
    FString Response;

private:
    EOutcome Outcome = EOutcome::None;
};
