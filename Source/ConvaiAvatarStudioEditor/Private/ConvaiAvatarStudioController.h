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
#include "Containers/Ticker.h"

struct FConvaiAvatarPublishRun;
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
    void RefreshPublishedAsset(const FString& AssetId);
    void ResumeLocalDraft(const FString& Id);
    void DiscardLocalDraft(const FString& Id);
    bool EnsureClient();
    void SignIn();
    bool ReviewUnsavedPackages(const FConvaiAvatarPrepareRequest& Request, FString& Error);
    bool ReviewMissingPackages(FConvaiAvatarPrepareRequest& Request, FString& Error);
    bool ReviewSourceChanges(FConvaiAvatarPrepareRequest& Request, FString& Error);
    void Fail(const FString& Error, const FString& FriendlyError = FString());
    /** Only workspace-authored review/preparation errors; never raw remote or child-process output. */
    void FailLocal(const FString& Error);
    void SetJob(const FString& Title, const FString& Detail);
    void Select(const FString& Id);
    void ResetLibraryDetails();
    void RequestSelectedDetails(const FString& Id);
    void CompleteSelectedDetails(const FString& Id, uint64 Operation, uint64 Selection, const FString& Account,
        FConvaiAvatarAsset Asset, FString Error);
    void CompleteAvatarImages(uint64 Operation, const FString& Account, TArray<FConvaiAvatarImageRecord> Images, FString Error);
    void Create(const FConvaiAvatarStudioCreateRequest& Request);
    void CreateReviewed(const FConvaiAvatarStudioCreateRequest& Request);
    void UploadChanges(const FString& Id, const FConvaiAvatarStudioUploadOptions& Options);
    void UploadChangesReviewed(const FString& Id, const FConvaiAvatarStudioUploadOptions& Options);
    void SaveMetadata(const FConvaiAvatarStudioMetadataRequest& Request);
    void CaptureCreatePortrait(const FConvaiAvatarStudioCreateRequest& Request);
    void CaptureMetadataPortrait(const FConvaiAvatarStudioMetadataRequest& Request);
    void CapturePortrait(const FSoftObjectPath& Blueprint, bool bIsMetaHuman, TFunction<void(FString)> Completion);
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
    void UploadNextArtifact();
    void CompleteUploadedArtifact(const FString& Platform);
    void WriteArtifact(FConvaiAvatarAsset Current);
    void FailPublish(const FString& Error, const FString& Step);
    void CompletePublish(const FString& Id);
    void Download(const FString& Id);
    bool ResumePendingDownloads();
    void Delete(const FString& Id);
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
    TMap<FString,FString> UnboundCloudIds;
    TSharedPtr<FConvaiAvatarAssetsClient> Client;
    TSharedPtr<FConvaiAvatarPackaging,ESPMode::ThreadSafe> Packaging;
    TSharedPtr<FConvaiAvatarPublishRun,ESPMode::ThreadSafe> CurrentPublish;
    TMap<FString,FString> DiagnosticPaths;
    TSharedPtr<FConvaiAvatarUploadDefaults> UploadDefaults;
    FConvaiAvatarDioramaLimits CachedDioramaLimits;
    bool bHasDioramaLimits = false;
    TSharedPtr<FConvaiAvatarRemoteConfiguration> RemoteConfiguration;
    TSharedPtr<FConvaiAvatarPortraitCapture> PortraitCapture;
    FConvaiAvatarRemoteConfigurationData RemoteConfigurationData;
    FString AcknowledgedEnginePair;
    TFuture<void> ArchiveWorker;
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
    friend class FConvaiAvatarSaveReviewIntegrationTest;
    /** Offline controller fixture seam; ordinary requests always use the real client. */
    TFunction<void(const FString&, FConvaiAvatarAssetsClient::FAssetCallback)> SelectionRequestOverride;
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
