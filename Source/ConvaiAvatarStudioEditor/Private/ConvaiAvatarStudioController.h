// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "UI/SConvaiAvatarStudio.h"
#include "Services/ConvaiAvatarAssetsClient.h"
#include "Services/ConvaiAvatarUploadDefaults.h"
#include "Packaging/ConvaiAvatarPackaging.h"
#include "Services/ConvaiAvatarRemoteConfiguration.h"
#include "Services/ConvaiAvatarLibraryState.h"
#include "Workspace/ConvaiAvatarUnboundBindings.h"
#include "Containers/Ticker.h"
#include <atomic>

struct FConvaiAvatarPublishRun;
struct FConvaiAvatarPendingDownload;
class FConvaiAvatarPortraitCapture;
class IConfigurationService;

class FConvaiAvatarStudioController : public TSharedFromThis<FConvaiAvatarStudioController,ESPMode::ThreadSafe>
{
public:
    TSharedRef<SConvaiAvatarStudio> CreateWidget();
    void Refresh();
    void Shutdown();
    void SetStartupNotice(const FString& Notice) { State.Notice = Notice; }
private:
    void Publish();
    void RefreshLocalRecords();
    /** After a successful listing: binds each pending pair whose cloud avatar is listed, and looks up the ones that are not.
     *  The pending pairs are those RefreshLocalRecords loaded, as Refresh does before it lists the library. */
    void RetryUnboundBindings();
    /** A looked-up cloud avatar that exists is bound; only a not-found answer drops its pair. Any other error keeps it. */
    void CompleteBindingLookup(const FConvaiAvatarUnboundBinding& Pair, const FConvaiAvatarAsset& Asset, const FString& Error);
    /** True when Convai still uses the credential the pair's cloud avatar was created with, so a lookup can prove it gone. */
    bool IsCurrentCredential(const FConvaiAvatarUnboundBinding& Pair) const;
    void RefreshPublishedAsset(const FString& AssetId);
    void ResumeLocalDraft(const FString& Id);
    void DiscardLocalDraft(const FString& Id);
    bool EnsureClient();
    void SignIn();
    bool ReviewUnsavedPackages(FConvaiAvatarPrepareRequest& Request, FString& Error);
    bool ReviewMissingPackages(FConvaiAvatarPrepareRequest& Request, FString& Error);
    bool ReviewSourceChanges(FConvaiAvatarPrepareRequest& Request, FString& Error);
    void Fail(const FString& Error, const FString& FriendlyError = FString());
    /** Writes the redacted detail to a new log under Saved/ConvaiAvatarStudio/Logs and points Open log at it. */
    void WriteDiagnosticLog(const FString& Error);
    /** Only workspace-authored review/preparation errors; never raw remote or child-process output. */
    void FailLocal(const FString& Error);
    /** Retitles the running job; BeginJob starts one. */
    void SetJob(const FString& Title, const FString& Detail, bool bCancellable = true);
    /** Returns the job's Generation, which its callbacks compare against. */
    uint64 BeginJob(const FString& Title, const FString& Detail, bool bCancellable);
    /** Every job ends here, showing Notice or Error and never both. */
    void EndJob(const FString& Notice, const FString& Error);
    /** Retires the running job's callbacks, which then return without ending it, so it ends here. */
    void AbandonJob(const FString& Notice = FString());
    void Select(const FString& Id);
    void ResetLibraryDetails();
    void RequestSelectedDetails(const FString& Id);
    bool CanRequestAsset() const;
    void RequestAsset(const FString& Id, FConvaiAvatarAssetsClient::FAssetCallback Completion);
    void CompleteSelectedDetails(const FString& Id, uint64 Operation, uint64 Selection, const FString& Account,
        FConvaiAvatarAsset Asset, FString Error);
    void CompleteAvatarImages(uint64 Operation, const FString& Account, TArray<FConvaiAvatarImageRecord> Images, FString Error);
    void Create(const FConvaiAvatarStudioCreateRequest& Request);
    void CreateReviewed(const FConvaiAvatarStudioCreateRequest& Request, const TWeakPtr<SConvaiAvatarStudio>& RequestingView);
    /** False vetoes the create's upload. Only the view that asked for the create may, while it is the open tab. */
    bool AcceptCapturedCreateThumbnail(const TWeakPtr<SConvaiAvatarStudio>& RequestingView, const FConvaiAvatarStudioCreateRequest& Request, const FString& File);
    void UploadChanges(const FString& Id, const FConvaiAvatarStudioUploadOptions& Options);
    void UploadChangesReviewed(const FString& Id, const FConvaiAvatarStudioUploadOptions& Options);
    void SaveMetadata(const FConvaiAvatarStudioMetadataRequest& Request);
    void CaptureCreatePortrait(const FConvaiAvatarStudioCreateRequest& Request);
    void CaptureMetadataPortrait(const FConvaiAvatarStudioMetadataRequest& Request);
    void CapturePortrait(const FSoftObjectPath& Blueprint, bool bIsMetaHuman, TFunction<void(FString)> Completion);
    void CompleteCreateCapture(const FConvaiAvatarStudioCreateRequest& Request, const FString& File);
    void CompleteMetadataCapture(const FConvaiAvatarStudioMetadataRequest& Request, const FString& File);
    void BrowseContent(const FString& Id);
    void OpenLog(const FString& Kind);
    void OpenDocumentation();
    bool ValidateUploadOptions(const FConvaiAvatarStudioUploadOptions& Options);
    void RefreshUploadDefaults();
    void ScanDiorama(const FConvaiAvatarStudioCreateRequest& Request);
    FConvaiAvatarDioramaReport InspectDiorama(const FString& BlueprintPath, const FString& LevelPath, const FConvaiAvatarDioramaLimits* Limits) const;
    void ShowDioramaReport(const FConvaiAvatarDioramaReport& Report);
    void WithFreshDioramaScan(FConvaiAvatarPrepareRequest Request, TFunction<void(FConvaiAvatarPrepareRequest)> Completion);
    void RefreshRemoteConfiguration(bool bApplyAfterFetch = false, TFunction<void()> AfterReview = {});
    void ContinueEngineMismatch();
    void UpdateEngineNotice();
    void ApplyReviewedConfiguration();
    void BeginPublish(FConvaiAvatarPreparedAsset Prepared, FConvaiAvatarAsset Existing,
        const FString& Thumbnail, const FConvaiAvatarStudioUploadOptions& Options);
    void PreparePublishArtifacts();
    void UploadNextArtifact(bool bRuntimeContractChecked=false);
    void CompleteUploadedArtifact(const FString& Platform);
    void WriteArtifact(FConvaiAvatarAsset Current);
    /** Binds the draft's local record to the cloud avatar it created; on failure, keeps the draft from creating another. */
    bool BindCreatedAvatar(FConvaiAvatarPreparedAsset& Prepared, const FString& CloudId);
    void FailPublish(const FString& Error, const FString& Step);
    void CompletePublish(const FString& Id);
    void Download(const FString& Id);
    /** Game thread, once the archive worker has staged Job (or failed with Error). Applies it while Op is still the running
     *  job; otherwise deletes its job.json, since Cancel already said nothing was installed and the next editor start would
     *  apply it, then the rest of its folder on a worker. */
    void CompleteStaging(uint64 Op, FConvaiAvatarPendingDownload& Job, const FString& Error);
    bool ResumePendingDownloads();
    void Delete(const FString& Id);
    /** Cancels and waits for the previous archive worker, then makes the next one's own cancel flag current. */
    TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> BeginArchiveWorker();
    void Cancel();
    void ApplyConfiguration();
    void RestoreConfiguration();
    static TSharedPtr<FJsonObject> MetadataFor(const FConvaiAvatarPreparedAsset& Prepared, const FConvaiAvatarAsset& Existing);
    TWeakPtr<SConvaiAvatarStudio> Widget;
    FConvaiAvatarStudioViewState State;
    TArray<FConvaiAvatarAsset> Assets;
    TSet<FString> LocalAssetIds;
    TSet<FString> IncompleteLocalIds;
    TMap<FString,FConvaiAvatarPreparedAsset> LocalDrafts;
    TMap<FString,FConvaiAvatarPreparedAsset> LocalRecords;
    TMap<FString,FString> LocalThumbnails;
    /** Drafts whose cloud avatar exists but is not bound to them yet, keyed by draft ID; saved ones and any that could not be. */
    TMap<FString,FConvaiAvatarUnboundBinding> UnboundBindings;
    /** Assets is this account's complete listing, from the latest Refresh. A pair is bound only after one. Missing from it
     *  never drops a pair: a new avatar may not be listed yet. */
    bool bLibraryListed = false;
    TSharedPtr<FConvaiAvatarAssetsClient> Client;
    TSharedPtr<FConvaiAvatarPackaging,ESPMode::ThreadSafe> Packaging;
    TSharedPtr<FConvaiAvatarPublishRun,ESPMode::ThreadSafe> CurrentPublish;
    TMap<FString,FString> DiagnosticPaths;
    FString LastRecoveryDiagnostic;
    TSharedPtr<FConvaiAvatarUploadDefaults> UploadDefaults;
    FConvaiAvatarDioramaLimits CachedDioramaLimits;
    bool bHasDioramaLimits = false;
    TSharedPtr<FConvaiAvatarRemoteConfiguration> RemoteConfiguration;
    TSharedPtr<FConvaiAvatarRemoteConfiguration> RuntimeContractCheck;
    TSharedPtr<FConvaiAvatarPortraitCapture> PortraitCapture;
    FConvaiAvatarRemoteConfigurationData RemoteConfigurationData;
    FString AcknowledgedEnginePair;
    TFuture<void> ArchiveWorker;
    /** The running archive worker's cancel flag, until its game-thread completion or Cancel clears it. A flag per worker:
     *  resetting a shared one for the next archive could un-cancel a worker that is still stopping. */
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> ArchiveCancelled;
    /** Also the library and account epoch. A running job's callbacks return on a change without ending the job, so it
     *  changes only while idle or through AbandonJob. */
    uint64 Generation = 0;
    FString AuthFingerprint;
    FConvaiAvatarLibraryState LibraryState;
    /** Details are fetched only for a selected asset, once per library refresh. */
    TSet<FString> DetailedAssetIds;
    uint64 SelectionGeneration = 0;
    uint64 DetailsOperation = 0;
    FString PendingDetailsId;
    FString PendingDetailsAccount;
    FString SelectedDetailsError;
#if WITH_DEV_AUTOMATION_TESTS
    friend class FConvaiAvatarSelectedDetailsTest;
    friend class FConvaiAvatarEveryJobEndClearsJobStateTest;
    friend class FConvaiAvatarReopenedTabShowsRunningJobTest;
    friend class FConvaiAvatarOnlyTheRequestingViewVetoesACreateTest;
    friend class FConvaiAvatarStartupNoticeSurvivesAFailedFirstRefreshTest;
    friend class FConvaiAvatarUnboundCloudBindingSurvivesRestartTest;
    friend class FConvaiAvatarUnboundCloudBindingOfADeletedAvatarIsDroppedTest;
    friend class FConvaiAvatarUnsavedUnboundBindingIsRetriedTest;
    friend class FConvaiAvatarUnboundBindingIsDroppedOnlyWhenNotFoundTest;
    friend class FConvaiAvatarUnreadableUnboundBindingWithholdsItsDraftTest;
    friend class FConvaiAvatarLegacyUnboundBindingIsLookedUpTest;
    friend class FConvaiAvatarRedirectedUnboundFolderWithholdsEveryDraftTest;
    friend class FConvaiAvatarCancelAfterStagingDropsTheJobTest;
    friend class FConvaiAvatarSaveReviewIntegrationTest;
    friend class FConvaiAvatarPendingRecoveryTest;
    friend class FConvaiAvatarShutdownCancelsArchiveWorkerTest;
    friend class FConvaiAvatarCancelStopsArchiveWorkerTest;
    friend class FConvaiAvatarNextArchiveStopsStaleWorkerTest;
    /** Offline controller fixture seam for single-asset requests; ordinary requests always use the real client. */
    TFunction<void(const FString&, FConvaiAvatarAssetsClient::FAssetCallback)> AssetRequestOverride;
#endif
    TSharedPtr<IConfigurationService> AuthenticationConfiguration;
    FDelegateHandle AuthenticationChangedHandle;
    FTSTicker::FDelegateHandle AuthenticationRefreshTicker;
    bool bAuthenticationRefreshPending = false;
    bool bShuttingDown = false;
    bool bLocalWorkActive = false;
    bool bCancelRequested = false;
    bool bRemoteActionPending = false;
    bool bDioramaScanPending = false;
};
