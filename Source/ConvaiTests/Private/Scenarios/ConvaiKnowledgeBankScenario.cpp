// Copyright 2022 Convai Inc. All Rights Reserved.

// The Knowledge Bank REST lifecycle against the backend: upload a document,
// poll the listing until the server has processed it, connect it to the test
// character, disconnect it, delete it, and read every step back through the
// listing. Ported from the in-plugin UConvaiTest_KnowledgeBank.
//
// Opt-in: the endpoints are gated behind an Enterprise plan, so the scenario is
// setup-failed unless -ConvaiKnowledgeBank=1 is on the command line. No session
// is opened and nothing is spawned; the character id is only the key the list
// and status endpoints take.

#include "ConvaiKnowledgeBankTestListener.h"

#include "ConvaiTestEventRecorder.h"
#include "ConvaiTestFixture.h"
#include "ConvaiTestScenario.h"
#include "ConvaiUtils.h"
#include "RestAPI/ConvaiKnowledgeBankProxy.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // Gap between listings while waiting for availability or for a status
    // change to land.
    constexpr float kRetryIntervalSeconds = 3.0f;

    // Listings per phase before the phase is called failed rather than slow.
    constexpr int32 kMaxListAttempts = 30;

    const TCHAR* kFileContents =
        TEXT("Photosynthesis converts light energy into chemical energy stored as sugars.\r\n")
        TEXT("This document exists only to exercise the Convai Knowledge Bank API.\r\n");
}

void UConvaiKnowledgeBankTestListener::HandleUploaded(const FConvaiKnowledgeBankDocument& Document)
{
    UploadedDocument = Document;
    Outcome = EOutcome::Uploaded;
}

void UConvaiKnowledgeBankTestListener::HandleUploadFailed(const FConvaiKnowledgeBankDocument& Document)
{
    UploadedDocument = Document;
    Outcome = EOutcome::UploadFailed;
}

void UConvaiKnowledgeBankTestListener::HandleListed(const TArray<FConvaiKnowledgeBankDocument>& Documents)
{
    Listing = Documents;
    Outcome = EOutcome::Listed;
}

void UConvaiKnowledgeBankTestListener::HandleListFailed(const TArray<FConvaiKnowledgeBankDocument>& Documents)
{
    Listing = Documents;
    Outcome = EOutcome::ListFailed;
}

void UConvaiKnowledgeBankTestListener::HandleStatusSet(FString ResponseString)
{
    Response = MoveTemp(ResponseString);
    Outcome = EOutcome::StatusSet;
}

void UConvaiKnowledgeBankTestListener::HandleStatusFailed(FString ResponseString)
{
    Response = MoveTemp(ResponseString);
    Outcome = EOutcome::StatusFailed;
}

void UConvaiKnowledgeBankTestListener::HandleDeleted(FString ResponseString)
{
    Response = MoveTemp(ResponseString);
    Outcome = EOutcome::Deleted;
}

void UConvaiKnowledgeBankTestListener::HandleDeleteFailed(FString ResponseString)
{
    Response = MoveTemp(ResponseString);
    Outcome = EOutcome::DeleteFailed;
}

UConvaiKnowledgeBankTestListener::EOutcome UConvaiKnowledgeBankTestListener::Take()
{
    const EOutcome Taken = Outcome;
    Outcome = EOutcome::None;
    return Taken;
}

class FConvaiKnowledgeBankScenario : public FConvaiTestScenario
{
    enum class EPhase
    {
        Uploading,
        WaitingForAvailability,
        Connecting,
        VerifyingConnect,
        Disconnecting,
        VerifyingDisconnect,
        Deleting,
        VerifyingDelete,
        Done
    };

    using EOutcome = UConvaiKnowledgeBankTestListener::EOutcome;

public:
    static const TCHAR* StaticName() { return TEXT("knowledge_bank_lifecycle"); }
    virtual const TCHAR* Name() const override { return StaticName(); }

    // Processing is server-side and unhurried: four polled phases of up to
    // kMaxListAttempts listings each, and the watchdog only aborts at twice this.
    virtual double DeadlineSeconds() const override { return 240.0; }
    virtual bool RequiresLiveConnection() const override { return true; }
    virtual bool RequiresTestCharacter() const override { return true; }

    virtual void Start(UWorld* /*InWorld*/, FConvaiTestEventRecorder& InRecorder) override
    {
        Recorder = &InRecorder;
        Latency.Begin();

        if (UConvaiUtils::GetCustomParam(TEXT("ConvaiKnowledgeBank")) != TEXT("1"))
        {
            Recorder->Record(TEXT("setup_failed"),
                             TEXT("Knowledge Bank scenarios are opt-in: pass -ConvaiKnowledgeBank=1 (the "
                                  "API key needs an Enterprise plan)"));
            bSetupFailed = true;
            return;
        }

        FString Error;
        if (!FConvaiTestFixture::HasCredentials(Error))
        {
            Recorder->Record(TEXT("setup_failed"), Error);
            bSetupFailed = true;
            return;
        }
        CharacterID = UConvaiUtils::GetTestCharacterID();

        // The UTC stamp in the name makes a leaked document attributable to a run.
        TempFilePath = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("ConvaiTests"),
            FString::Printf(TEXT("KnowledgeBankTest_%s.txt"),
                            *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"))));
        if (!FFileHelper::SaveStringToFile(kFileContents, *TempFilePath))
        {
            Recorder->Record(TEXT("setup_failed"),
                             FString::Printf(TEXT("could not write the upload fixture to %s"), *TempFilePath));
            bSetupFailed = true;
            return;
        }
        Recorder->Record(TEXT("fixture_written"), TempFilePath);

        Listener.Reset(NewObject<UConvaiKnowledgeBankTestListener>());

        Phase = EPhase::Uploading;
        UConvaiUploadKnowledgeBankDocument* Proxy =
            UConvaiUploadKnowledgeBankDocument::ConvaiUploadKnowledgeBankDocumentProxy(TempFilePath);
        Proxy->OnSuccess.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleUploaded);
        Proxy->OnFailure.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleUploadFailed);
        Proxy->Activate();
        Recorder->Record(TEXT("upload_sent"), FPaths::GetCleanFilename(TempFilePath));
    }

    virtual bool Poll(float DeltaSeconds) override
    {
        if (bSetupFailed)
        {
            return true;
        }
        if (!Listener.IsValid())
        {
            FailReason = TEXT("listener went away");
            return true;
        }

        if (RetryRemainingSeconds > 0.0f)
        {
            RetryRemainingSeconds -= DeltaSeconds;
            if (RetryRemainingSeconds <= 0.0f)
            {
                SendList();
            }
            return false;
        }

        switch (Listener->Take())
        {
        case EOutcome::None:
            return false;

        case EOutcome::Uploaded:
            return OnUploaded(Listener->UploadedDocument);

        case EOutcome::UploadFailed:
            bUploadRejected = true;
            FailReason = TEXT("the upload was rejected");
            return true;

        case EOutcome::Listed:
            LastListing = Listener->Listing;
            return OnListed();

        case EOutcome::ListFailed:
            FailReason = FString::Printf(TEXT("list request failed while %s"), PhaseName());
            return true;

        case EOutcome::StatusSet:
            return OnStatusSet(Listener->Response);

        case EOutcome::StatusFailed:
            FailReason = FString::Printf(TEXT("character update failed while %s: %s"), PhaseName(),
                                         *Listener->Response);
            return true;

        case EOutcome::Deleted:
            bDocumentDeleted = true;
            Latency.Mark(TEXT("delete_ms"));
            Recorder->Record(TEXT("delete_accepted"), Listener->Response);
            Phase = EPhase::VerifyingDelete;
            SendList();
            return false;

        case EOutcome::DeleteFailed:
            FailReason = FString::Printf(TEXT("delete request failed: %s"), *Listener->Response);
            return true;
        }
        return false;
    }

    virtual FConvaiScenarioResult Finish() override
    {
        FConvaiScenarioResult Result;

        // Fire-and-forget: the proxy roots itself for the request's lifetime,
        // so it outlives this scenario and still removes the document the run
        // created.
        if (!DocumentID.IsEmpty() && !bDocumentDeleted)
        {
            Recorder->Record(TEXT("cleanup_delete"), DocumentID);
            UConvaiDeleteKnowledgeBankDocument::ConvaiDeleteKnowledgeBankDocumentProxy(DocumentID)->Activate();
        }
        if (!TempFilePath.IsEmpty())
        {
            IFileManager::Get().Delete(*TempFilePath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
        }
        Listener.Reset();

        const TMap<FString, double> Marks = Latency.Snapshot();
        auto Reached = [&Marks](const TCHAR* Mark) { return Marks.Contains(Mark) ? 1.0 : 0.0; };
        Result.Metrics.Add(TEXT("list_attempts"), TotalListings);
        Result.Metrics.Add(TEXT("uploaded"), Reached(TEXT("upload_ms")));
        Result.Metrics.Add(TEXT("available"), Reached(TEXT("available_ms")));
        Result.Metrics.Add(TEXT("connect_read_back"), Reached(TEXT("connect_readback_ms")));
        Result.Metrics.Add(TEXT("disconnect_read_back"), Reached(TEXT("disconnect_readback_ms")));
        Result.Metrics.Add(TEXT("deleted"), Reached(TEXT("delete_ms")));
        Result.Metrics.Add(TEXT("delete_read_back"), Reached(TEXT("delete_readback_ms")));
        for (const TPair<FString, double>& Pair : Marks)
        {
            Result.Metrics.Add(Pair.Key, Pair.Value);
        }

        Result.MetricNotes.Add(
            TEXT("available_ms"),
            TEXT("Covers: how long the server took to process the upload into a document that "
                 "can be connected. Moves with the backend, not the build: a slow value is not a "
                 "plugin regression, and a fast one says nothing about the plugin. Does NOT "
                 "cover: the plugin's upload or list code, which ran before this mark."));

        if (bSetupFailed)
        {
            Result.bSetupFailed = true;
            Result.FailReason = TEXT("setup failed; see trace");
            return Result;
        }

        if (bUploadRejected)
        {
            FConvaiScenarioFinding Finding;
            Finding.DedupKey = TEXT("knowledge-bank-upload-rejected");
            Finding.Summary = TEXT("the Knowledge Bank upload was rejected, so nothing after it could "
                                   "be graded");
            Finding.Evidence = FString::Printf(
                TEXT("UConvaiUploadKnowledgeBankDocument broadcast OnFailure for %s while %s. The "
                     "endpoint requires an Enterprise-plan API key; a key on any other plan fails "
                     "here before the plugin's parsing or polling runs."),
                *FPaths::GetCleanFilename(TempFilePath), PhaseName());
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FailReason;
            return Result;
        }

        if (bListingsExhausted)
        {
            FConvaiScenarioFinding Finding;
            switch (Phase)
            {
            case EPhase::WaitingForAvailability:
                Finding.DedupKey = TEXT("knowledge-bank-document-never-available");
                Finding.Summary = TEXT("an uploaded document never became available");
                Finding.Evidence = FString::Printf(
                    TEXT("Document %s was uploaded, but %d listings %.0f s apart never showed it with "
                         "bIsAvailable set. Last listing: %s."),
                    *DocumentID, ListAttempts, kRetryIntervalSeconds, *OwnDocumentState());
                break;

            case EPhase::VerifyingConnect:
            case EPhase::VerifyingDisconnect:
                Finding.DedupKey = TEXT("knowledge-bank-status-not-read-back");
                Finding.Summary = TEXT("a document status change was accepted but the listing never "
                                       "reflected it");
                Finding.Evidence = FString::Printf(
                    TEXT("SetStatus with bConnected=%s for %s was accepted, but %d listings %.0f s "
                         "apart never read it back. Last listing: %s."),
                    Phase == EPhase::VerifyingConnect ? TEXT("true") : TEXT("false"), *DocumentID,
                    ListAttempts, kRetryIntervalSeconds, *OwnDocumentState());
                break;

            default:
                Finding.DedupKey = TEXT("knowledge-bank-delete-not-read-back");
                Finding.Summary = TEXT("a document delete was accepted but the document stayed in "
                                       "the listing");
                Finding.Evidence = FString::Printf(
                    TEXT("Delete of %s was accepted, but %d listings %.0f s apart still returned it. "
                         "Last listing: %s."),
                    *DocumentID, ListAttempts, kRetryIntervalSeconds, *OwnDocumentState());
                break;
            }
            Finding.Metrics = Result.Metrics;
            Result.Findings.Add(MoveTemp(Finding));

            Result.FailReason = FString::Printf(TEXT("%s after %d listings"), *ExhaustedWhy, ListAttempts);
            return Result;
        }

        if (!FailReason.IsEmpty())
        {
            Result.FailReason = FailReason;
            return Result;
        }

        Result.bPassed = true;
        return Result;
    }

private:
    bool OnUploaded(const FConvaiKnowledgeBankDocument& Document)
    {
        if (Document.DocumentID.IsEmpty())
        {
            FailReason = TEXT("upload succeeded but returned no document ID");
            return true;
        }

        DocumentID = Document.DocumentID;
        Latency.Mark(TEXT("upload_ms"));
        Recorder->Record(TEXT("uploaded"),
                         FString::Printf(TEXT("%s as %s (%lld bytes, available=%d)"), *Document.FileName,
                                         *DocumentID, Document.FileSizeBytes, Document.bIsAvailable ? 1 : 0));

        const FString ExpectedFileName = FPaths::GetCleanFilename(TempFilePath);
        if (!Document.FileName.IsEmpty() && Document.FileName != ExpectedFileName)
        {
            Recorder->Record(TEXT("file_name_mismatch"),
                             FString::Printf(TEXT("server '%s', sent '%s'"), *Document.FileName, *ExpectedFileName));
        }

        Phase = EPhase::WaitingForAvailability;
        SendList();
        return false;
    }

    bool OnListed()
    {
        const FConvaiKnowledgeBankDocument* Own = FindOwnDocument();
        switch (Phase)
        {
        case EPhase::WaitingForAvailability:
            if (!Own)
            {
                return RetryList(TEXT("uploaded document is not in the listing yet"));
            }
            if (!Own->bIsAvailable)
            {
                return RetryList(TEXT("uploaded document is still processing"));
            }
            Latency.Mark(TEXT("available_ms"));
            Recorder->Record(TEXT("available"),
                             FString::Printf(TEXT("%s after %d listing(s)"), *DocumentID, ListAttempts));
            SendStatus(/*bConnect*/ true);
            return false;

        case EPhase::VerifyingConnect:
            if (!Own)
            {
                return RetryList(TEXT("document vanished from the listing after connecting"));
            }
            if (!Own->bConnected)
            {
                return RetryList(TEXT("document does not read back as connected yet"));
            }
            Latency.Mark(TEXT("connect_readback_ms"));
            Recorder->Record(TEXT("connect_read_back"),
                             FString::Printf(TEXT("after %d listing(s)"), ListAttempts));
            SendStatus(/*bConnect*/ false);
            return false;

        case EPhase::VerifyingDisconnect:
            if (Own && Own->bConnected)
            {
                return RetryList(TEXT("document still reads back as connected"));
            }
            Latency.Mark(TEXT("disconnect_readback_ms"));
            Recorder->Record(TEXT("disconnect_read_back"),
                             FString::Printf(TEXT("after %d listing(s)"), ListAttempts));
            SendDelete();
            return false;

        case EPhase::VerifyingDelete:
            if (Own)
            {
                return RetryList(TEXT("deleted document is still in the listing"));
            }
            Latency.Mark(TEXT("delete_readback_ms"));
            Recorder->Record(TEXT("delete_read_back"),
                             FString::Printf(TEXT("after %d listing(s)"), ListAttempts));
            Phase = EPhase::Done;
            return true;

        default:
            FailReason = FString::Printf(TEXT("a listing arrived while %s"), PhaseName());
            return true;
        }
    }

    bool OnStatusSet(const FString& Response)
    {
        if (Phase == EPhase::Connecting)
        {
            Recorder->Record(TEXT("connect_accepted"), Response);
            Phase = EPhase::VerifyingConnect;
        }
        else if (Phase == EPhase::Disconnecting)
        {
            Recorder->Record(TEXT("disconnect_accepted"), Response);
            Phase = EPhase::VerifyingDisconnect;
        }
        else
        {
            FailReason = FString::Printf(TEXT("a status response arrived while %s"), PhaseName());
            return true;
        }
        SendList();
        return false;
    }

    // True when the phase has used up its listings; Phase then names which.
    bool RetryList(const TCHAR* Why)
    {
        if (ListAttempts >= kMaxListAttempts)
        {
            bListingsExhausted = true;
            ExhaustedWhy = Why;
            return true;
        }
        Recorder->Record(TEXT("list_retry"),
                         FString::Printf(TEXT("%s; listing again in %.0f s (attempt %d)"), Why,
                                         kRetryIntervalSeconds, ListAttempts + 1));
        RetryRemainingSeconds = kRetryIntervalSeconds;
        return false;
    }

    void SendList()
    {
        ++ListAttempts;
        ++TotalListings;
        UConvaiListKnowledgeBankDocuments* Proxy =
            UConvaiListKnowledgeBankDocuments::ConvaiListKnowledgeBankDocumentsProxy(CharacterID);
        Proxy->OnSuccess.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleListed);
        Proxy->OnFailure.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleListFailed);
        Proxy->Activate();
    }

    void SendStatus(bool bConnect)
    {
        // The whole last listing with only our flag flipped; see the proxy's
        // note on merge-versus-replace.
        TArray<FConvaiKnowledgeBankDocument> Documents = LastListing;
        for (FConvaiKnowledgeBankDocument& Document : Documents)
        {
            if (Document.DocumentID == DocumentID)
            {
                Document.bConnected = bConnect;
            }
        }

        Phase = bConnect ? EPhase::Connecting : EPhase::Disconnecting;
        ListAttempts = 0;
        UConvaiSetKnowledgeBankDocumentStatus* Proxy =
            UConvaiSetKnowledgeBankDocumentStatus::ConvaiSetKnowledgeBankDocumentStatusProxy(CharacterID, Documents);
        Proxy->OnSuccess.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleStatusSet);
        Proxy->OnFailure.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleStatusFailed);
        Proxy->Activate();
        Recorder->Record(bConnect ? TEXT("connect_sent") : TEXT("disconnect_sent"), DocumentID);
    }

    void SendDelete()
    {
        Phase = EPhase::Deleting;
        ListAttempts = 0;
        UConvaiDeleteKnowledgeBankDocument* Proxy =
            UConvaiDeleteKnowledgeBankDocument::ConvaiDeleteKnowledgeBankDocumentProxy(DocumentID);
        Proxy->OnSuccess.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleDeleted);
        Proxy->OnFailure.AddDynamic(Listener.Get(), &UConvaiKnowledgeBankTestListener::HandleDeleteFailed);
        Proxy->Activate();
        Recorder->Record(TEXT("delete_sent"), DocumentID);
    }

    const FConvaiKnowledgeBankDocument* FindOwnDocument() const
    {
        return LastListing.FindByPredicate([this](const FConvaiKnowledgeBankDocument& Document)
                                           { return Document.DocumentID == DocumentID; });
    }

    FString OwnDocumentState() const
    {
        const FConvaiKnowledgeBankDocument* Own = FindOwnDocument();
        if (!Own)
        {
            return TEXT("document absent");
        }
        return FString::Printf(TEXT("present, bIsAvailable=%s, bConnected=%s"),
                               Own->bIsAvailable ? TEXT("true") : TEXT("false"),
                               Own->bConnected ? TEXT("true") : TEXT("false"));
    }

    const TCHAR* PhaseName() const
    {
        switch (Phase)
        {
        case EPhase::Uploading:              return TEXT("uploading");
        case EPhase::WaitingForAvailability: return TEXT("waiting for availability");
        case EPhase::Connecting:             return TEXT("connecting");
        case EPhase::VerifyingConnect:       return TEXT("verifying connect");
        case EPhase::Disconnecting:          return TEXT("disconnecting");
        case EPhase::VerifyingDisconnect:    return TEXT("verifying disconnect");
        case EPhase::Deleting:               return TEXT("deleting");
        case EPhase::VerifyingDelete:        return TEXT("verifying delete");
        default:                             return TEXT("done");
        }
    }

    FConvaiTestEventRecorder* Recorder = nullptr;
    FConvaiTestLatencyTracker Latency;

    // Strong: nothing else references the listener, and a GC mid-request would
    // drop the reply the scenario is waiting on.
    TStrongObjectPtr<UConvaiKnowledgeBankTestListener> Listener;

    EPhase Phase = EPhase::Uploading;
    FString CharacterID;
    FString TempFilePath;
    FString DocumentID;
    TArray<FConvaiKnowledgeBankDocument> LastListing;

    float RetryRemainingSeconds = 0.0f;
    int32 ListAttempts = 0;
    int32 TotalListings = 0;
    bool bDocumentDeleted = false;
    bool bUploadRejected = false;
    bool bListingsExhausted = false;
    FString ExhaustedWhy;
    bool bSetupFailed = false;
    FString FailReason;
};

CONVAI_REGISTER_SCENARIO(FConvaiKnowledgeBankScenario)
