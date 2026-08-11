// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
// Engine/TimerHandle.h was extracted from EngineTypes.h in UE 5.1. On 5.0
// the type still lives inside EngineTypes.h, so include that instead to
// keep this header self-contained (don't rely on PCH transitivity).
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Engine/TimerHandle.h"
#else
#include "Engine/EngineTypes.h"
#endif
#include "ConvaiDefinitions.h"
#include "Tests/ConvaiTestBase.h"
#include "ConvaiTest_KnowledgeBank.generated.h"

/**
 * Drives the whole Knowledge Bank lifecycle against the live API.
 *
 * Flow:
 *   1. Write a small text file under Saved/ConvaiTests and upload it.
 *   2. Poll the listing until the new document reports available.
 *   3. Connect it to the character, list again, assert it reads back connected.
 *   4. Disconnect it, list again, assert it reads back disconnected.
 *   5. Delete it, list again, assert it is gone -> PASS.
 *
 * The run creates its own document and never touches any other, and Teardown
 * deletes that document even when the test fails or times out. A crash between
 * upload and Teardown still leaves one orphan document on the account.
 */
UCLASS()
class CONVAI_API UConvaiTest_KnowledgeBank : public UConvaiTestBase
{
	GENERATED_BODY()

public:
	virtual FString GetTestName() const override { return TEXT("KnowledgeBank"); }

protected:
	virtual bool Setup() override;
	virtual void Execute() override;
	virtual void Teardown() override;

private:
	enum class EPhase : uint8
	{
		Idle,
		Uploading,
		WaitingForAvailability,
		Connecting,
		VerifyingConnect,
		Disconnecting,
		VerifyingDisconnect,
		Deleting,
		VerifyingDelete,
	};

	void StartList();
	void RetryListAfterDelay(const TCHAR* Why);
	void StopRetryTimer();
	void SendStatus(bool bConnect);
	void SendDelete();

	/** Finds this run's document in a listing. Returns nullptr when it is absent. */
	const FConvaiKnowledgeBankDocument* FindOwnDocument(const TArray<FConvaiKnowledgeBankDocument>& Documents) const;

	UFUNCTION()
	void OnUploaded(const FConvaiKnowledgeBankDocument& Document);

	UFUNCTION()
	void OnUploadFailed(const FConvaiKnowledgeBankDocument& Document);

	UFUNCTION()
	void OnListed(const TArray<FConvaiKnowledgeBankDocument>& Documents);

	UFUNCTION()
	void OnListFailed(const TArray<FConvaiKnowledgeBankDocument>& Documents);

	UFUNCTION()
	void OnStatusSet(FString Response);

	UFUNCTION()
	void OnStatusFailed(FString Response);

	UFUNCTION()
	void OnDeleted(FString Response);

	UFUNCTION()
	void OnDeleteFailed(FString Response);

	FTimerHandle RetryHandle;
	EPhase Phase = EPhase::Idle;

	FString TempFilePath;
	FString DocumentID;
	int32 ListAttempts = 0;

	/** Latest full listing, resent verbatim on connect / disconnect with one entry flipped. */
	TArray<FConvaiKnowledgeBankDocument> LastListing;

	bool bDocumentDeleted = false;
};
