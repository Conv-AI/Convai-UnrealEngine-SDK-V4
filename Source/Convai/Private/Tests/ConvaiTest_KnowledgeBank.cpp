// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTest_KnowledgeBank.h"

#include "ConvaiUtils.h"
#include "RestAPI/ConvaiKnowledgeBankProxy.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

namespace ConvaiKnowledgeBankTestConstants
{
	/** Processing is server-side and unhurried; a 30s run gives up mid-upload. */
	constexpr float MinimumTimeoutSeconds = 120.0f;

	/** Gap between listings while waiting for availability or for a status change to land. */
	constexpr float RetryIntervalSeconds = 3.0f;

	/** Listings per phase before the phase is called failed rather than slow. */
	constexpr int32 MaxListAttempts = 30;

	const TCHAR* FileContents =
		TEXT("Photosynthesis converts light energy into chemical energy stored as sugars.\r\n")
		TEXT("This document exists only to exercise the Convai Knowledge Bank API.\r\n");
}

bool UConvaiTest_KnowledgeBank::Setup()
{
	if (Context.CharacterID.IsEmpty())
	{
		Context.CharacterID = UConvaiUtils::GetTestCharacterID();
		LogEvent(ELogVerbosity::Log,
			FString::Printf(TEXT("No CharacterID supplied; using TestCharacterID from settings: '%s'"), *Context.CharacterID),
			TEXT("setup"));
	}

	if (!Super::Setup())
	{
		return false;
	}

	if (Context.TimeoutSeconds < ConvaiKnowledgeBankTestConstants::MinimumTimeoutSeconds)
	{
		LogEvent(ELogVerbosity::Warning,
			FString::Printf(TEXT("Raising timeout from %.0fs to %.0fs: knowledge bank processing is slower than the harness default"),
				Context.TimeoutSeconds, ConvaiKnowledgeBankTestConstants::MinimumTimeoutSeconds),
			TEXT("setup"));
		Context.TimeoutSeconds = ConvaiKnowledgeBankTestConstants::MinimumTimeoutSeconds;
	}

	TempFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ConvaiTests"),
		FString::Printf(TEXT("KnowledgeBankTest_%s.txt"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"))));

	if (!FFileHelper::SaveStringToFile(ConvaiKnowledgeBankTestConstants::FileContents, *TempFilePath))
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError,
			FString::Printf(TEXT("Could not write the upload fixture to %s"), *TempFilePath));
		return false;
	}

	LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("Wrote upload fixture: %s"), *TempFilePath), TEXT("setup"));
	return true;
}

void UConvaiTest_KnowledgeBank::Execute()
{
	Phase = EPhase::Uploading;
	LogEvent(ELogVerbosity::Log, TEXT("Uploading document"), TEXT("knowledgebank.upload"));

	UConvaiUploadKnowledgeBankDocument* Proxy =
		UConvaiUploadKnowledgeBankDocument::ConvaiUploadKnowledgeBankDocumentProxy(TempFilePath);
	Proxy->OnSuccess.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnUploaded);
	Proxy->OnFailure.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnUploadFailed);
	Proxy->Activate();
}

void UConvaiTest_KnowledgeBank::Teardown()
{
	StopRetryTimer();

	// Fire-and-forget: the proxy roots itself for the lifetime of the request, so
	// it outlives this test object and still removes the document the run created.
	if (!DocumentID.IsEmpty() && !bDocumentDeleted)
	{
		LogEvent(ELogVerbosity::Warning,
			FString::Printf(TEXT("Cleaning up leftover document %s"), *DocumentID),
			TEXT("knowledgebank.cleanup"));
		UConvaiDeleteKnowledgeBankDocument::ConvaiDeleteKnowledgeBankDocumentProxy(DocumentID)->Activate();
		bDocumentDeleted = true;
	}

	if (!TempFilePath.IsEmpty())
	{
		IFileManager::Get().Delete(*TempFilePath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	}
}

void UConvaiTest_KnowledgeBank::StartList()
{
	++ListAttempts;

	UConvaiListKnowledgeBankDocuments* Proxy =
		UConvaiListKnowledgeBankDocuments::ConvaiListKnowledgeBankDocumentsProxy(Context.CharacterID);
	Proxy->OnSuccess.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnListed);
	Proxy->OnFailure.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnListFailed);
	Proxy->Activate();
}

void UConvaiTest_KnowledgeBank::RetryListAfterDelay(const TCHAR* Why)
{
	if (ListAttempts >= ConvaiKnowledgeBankTestConstants::MaxListAttempts)
	{
		FinishWithFailure(EConvaiTestFailureReason::Timeout,
			FString::Printf(TEXT("%s after %d listings"), Why, ListAttempts));
		return;
	}

	UWorld* World = GetTestWorld();
	if (!World)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("No world available to schedule the next listing"));
		return;
	}

	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("%s; listing again in %.0fs (attempt %d)"), Why,
			ConvaiKnowledgeBankTestConstants::RetryIntervalSeconds, ListAttempts + 1),
		TEXT("knowledgebank.poll"));

	World->GetTimerManager().SetTimer(RetryHandle, this, &UConvaiTest_KnowledgeBank::StartList,
		ConvaiKnowledgeBankTestConstants::RetryIntervalSeconds, /*bLoop*/ false);
}

void UConvaiTest_KnowledgeBank::StopRetryTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(RetryHandle);
	}
}

void UConvaiTest_KnowledgeBank::SendStatus(bool bConnect)
{
	TArray<FConvaiKnowledgeBankDocument> Documents = LastListing;
	for (FConvaiKnowledgeBankDocument& Document : Documents)
	{
		if (Document.DocumentID == DocumentID)
		{
			Document.bConnected = bConnect;
		}
	}

	UConvaiSetKnowledgeBankDocumentStatus* Proxy =
		UConvaiSetKnowledgeBankDocumentStatus::ConvaiSetKnowledgeBankDocumentStatusProxy(Context.CharacterID, Documents);
	Proxy->OnSuccess.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnStatusSet);
	Proxy->OnFailure.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnStatusFailed);
	Proxy->Activate();
}

void UConvaiTest_KnowledgeBank::SendDelete()
{
	Phase = EPhase::Deleting;
	LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("Deleting document %s"), *DocumentID), TEXT("knowledgebank.delete"));

	UConvaiDeleteKnowledgeBankDocument* Proxy =
		UConvaiDeleteKnowledgeBankDocument::ConvaiDeleteKnowledgeBankDocumentProxy(DocumentID);
	Proxy->OnSuccess.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnDeleted);
	Proxy->OnFailure.AddDynamic(this, &UConvaiTest_KnowledgeBank::OnDeleteFailed);
	Proxy->Activate();
}

const FConvaiKnowledgeBankDocument* UConvaiTest_KnowledgeBank::FindOwnDocument(const TArray<FConvaiKnowledgeBankDocument>& Documents) const
{
	return Documents.FindByPredicate([this](const FConvaiKnowledgeBankDocument& Document)
	{
		return Document.DocumentID == DocumentID;
	});
}

void UConvaiTest_KnowledgeBank::OnUploaded(const FConvaiKnowledgeBankDocument& Document)
{
	if (Document.DocumentID.IsEmpty())
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Upload succeeded but returned no document ID"));
		return;
	}

	DocumentID = Document.DocumentID;
	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("Uploaded '%s' as %s (%lld bytes, available=%s)"),
			*Document.FileName, *Document.DocumentID, Document.FileSizeBytes,
			Document.bIsAvailable ? TEXT("true") : TEXT("false")),
		TEXT("knowledgebank.upload"));

	const FString ExpectedFileName = FPaths::GetCleanFilename(TempFilePath);
	if (!Document.FileName.IsEmpty() && Document.FileName != ExpectedFileName)
	{
		LogEvent(ELogVerbosity::Warning,
			FString::Printf(TEXT("Server reported file name '%s', expected '%s'"), *Document.FileName, *ExpectedFileName),
			TEXT("knowledgebank.upload"));
	}

	Phase = EPhase::WaitingForAvailability;
	ListAttempts = 0;
	StartList();
}

void UConvaiTest_KnowledgeBank::OnUploadFailed(const FConvaiKnowledgeBankDocument& Document)
{
	FinishWithFailure(EConvaiTestFailureReason::UnexpectedState,
		TEXT("Upload request failed (knowledge bank requires an Enterprise plan API key)"));
}

void UConvaiTest_KnowledgeBank::OnListed(const TArray<FConvaiKnowledgeBankDocument>& Documents)
{
	LastListing = Documents;
	const FConvaiKnowledgeBankDocument* Own = FindOwnDocument(Documents);

	switch (Phase)
	{
	case EPhase::WaitingForAvailability:
		if (!Own)
		{
			RetryListAfterDelay(TEXT("Uploaded document is not in the listing yet"));
			return;
		}
		if (!Own->bIsAvailable)
		{
			RetryListAfterDelay(TEXT("Uploaded document is still processing"));
			return;
		}
		LogEvent(ELogVerbosity::Log,
			FString::Printf(TEXT("Document %s is available after %d listings"), *DocumentID, ListAttempts),
			TEXT("knowledgebank.poll"));

		Phase = EPhase::Connecting;
		ListAttempts = 0;
		SendStatus(/*bConnect*/ true);
		return;

	case EPhase::VerifyingConnect:
		if (!Own)
		{
			RetryListAfterDelay(TEXT("Document vanished from the listing after connecting"));
			return;
		}
		if (!Own->bConnected)
		{
			RetryListAfterDelay(TEXT("Document does not read back as connected yet"));
			return;
		}
		LogEvent(ELogVerbosity::Log, TEXT("Document reads back as connected"), TEXT("knowledgebank.connect"));

		Phase = EPhase::Disconnecting;
		ListAttempts = 0;
		SendStatus(/*bConnect*/ false);
		return;

	case EPhase::VerifyingDisconnect:
		if (Own && Own->bConnected)
		{
			RetryListAfterDelay(TEXT("Document still reads back as connected"));
			return;
		}
		LogEvent(ELogVerbosity::Log, TEXT("Document reads back as disconnected"), TEXT("knowledgebank.connect"));

		ListAttempts = 0;
		SendDelete();
		return;

	case EPhase::VerifyingDelete:
		if (Own)
		{
			RetryListAfterDelay(TEXT("Deleted document is still in the listing"));
			return;
		}
		FinishWithPass(FString::Printf(
			TEXT("Uploaded, polled to available, connected, disconnected and deleted document %s"), *DocumentID));
		return;

	default:
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState,
			FString::Printf(TEXT("Received a listing in an unexpected phase (%d)"), static_cast<int32>(Phase)));
		return;
	}
}

void UConvaiTest_KnowledgeBank::OnListFailed(const TArray<FConvaiKnowledgeBankDocument>& Documents)
{
	FinishWithFailure(EConvaiTestFailureReason::UnexpectedState,
		FString::Printf(TEXT("List request failed in phase %d"), static_cast<int32>(Phase)));
}

void UConvaiTest_KnowledgeBank::OnStatusSet(FString Response)
{
	if (Phase == EPhase::Connecting)
	{
		LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("Connect accepted: %s"), *Response), TEXT("knowledgebank.connect"));
		Phase = EPhase::VerifyingConnect;
	}
	else if (Phase == EPhase::Disconnecting)
	{
		LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("Disconnect accepted: %s"), *Response), TEXT("knowledgebank.connect"));
		Phase = EPhase::VerifyingDisconnect;
	}
	else
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState,
			FString::Printf(TEXT("Status response arrived in an unexpected phase (%d)"), static_cast<int32>(Phase)));
		return;
	}

	StartList();
}

void UConvaiTest_KnowledgeBank::OnStatusFailed(FString Response)
{
	FinishWithFailure(EConvaiTestFailureReason::UnexpectedState,
		FString::Printf(TEXT("Character update failed in phase %d: %s"), static_cast<int32>(Phase), *Response));
}

void UConvaiTest_KnowledgeBank::OnDeleted(FString Response)
{
	bDocumentDeleted = true;
	LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("Delete accepted: %s"), *Response), TEXT("knowledgebank.delete"));

	Phase = EPhase::VerifyingDelete;
	StartList();
}

void UConvaiTest_KnowledgeBank::OnDeleteFailed(FString Response)
{
	FinishWithFailure(EConvaiTestFailureReason::UnexpectedState,
		FString::Printf(TEXT("Delete request failed: %s"), *Response));
}
