// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IHttpRequest.h"
#include "Services/ConvaiAvatarLibraryState.h"
#include "Services/ConvaiAvatarTransferProgress.h"

class FConvaiAvatarFileTransfer;

enum class EConvaiAvatarArtifactPresence : uint8 { Unknown, Missing, Available };

struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarAssetVersion
{
	FString Url;
	int64 SizeBytes = -1;
	/** Storage observation, independent of the preallocated DB versions list. */
	EConvaiAvatarArtifactPresence Presence = EConvaiAvatarArtifactPresence::Unknown;
};

/** One Assets API record. Metadata is retained in full because updates replace it on the server. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarAsset
{
	FString AssetId;
	FString AvatarId;
	FString Name;
	FString Gender;
	FString ThumbnailUrl;
	/** Returned thumbnail_gcp_path used only to compare image identity across signed-URL rotation. Never converted into a URL. */
	FString ThumbnailStoragePath;
	/** Optional server-supplied square image. Never replaces the original portrait or derives a storage path. */
	FString SquareThumbnailUrl;
	/** Distinguishes omitted square data from an explicit null or invalid value that must clear a retained image. */
	bool bHasSquareThumbnailField = false;
	FString EntityType;
	FString Visibility;
	TArray<FString> Versions;
	/** False for omitted/malformed inventory; an empty valid array is authoritative. */
	bool bHasVersionInventory = false;
	/** Lightweight lists deliberately omit storage observations; declared versions are not completed files. */
	bool bHasArtifactDetails = false;
	TArray<FString> Tags;
	TMap<FString, FConvaiAvatarAssetVersion> VersionInfo;
	TSharedPtr<FJsonObject> Metadata;

	/** Prefer source for the running engine, then legacy raw, then another available engine. */
	FString FindSourceVersion() const;
	bool HasSource() const { return !FindSourceVersion().IsEmpty(); }
	bool HasDeclaredSource() const;
	EConvaiAvatarArtifactPresence SourcePresence() const;
	bool IsPrivate() const { return Visibility.Equals(TEXT("private"), ESearchCase::IgnoreCase); }
};

/** Images from one read-only Avatar API batch, already restricted to the private Assets library. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarImageRecord
{
	FString AssetId;
	FString AvatarId;
	FString ThumbnailUrl;
	FString SquareThumbnailUrl;
	bool bHasThumbnailField = false;
	bool bHasSquareThumbnailField = false;
};

struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarAssetWrite
{
	/** Empty creates an avatar; otherwise Metadata must include the existing entity_id/avatar_id. */
	FString AssetId;
	FString Name;
	FString Gender = TEXT("male");
	FString Version;
	/** PNG or JPEG; required for creation, optional for updates. */
	FString ThumbnailPath;
	TArray<FString> Tags;
	TSharedPtr<FJsonObject> Metadata;
    /** No-version metadata write; the details and verified technical-commit builders constrain their own allowed fields. */
    bool bMetadataOnly = false;
	/** Existing-asset PUT reservation: omit metadata and thumbnail entirely until the file upload succeeds. */
	bool bArtifactReservationOnly = false;
};

struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarAssetWriteResult
{
	FConvaiAvatarAsset Asset;
	/** Signed PUT URL for Write.Version. Creating metadata does not upload the artifact. */
	FString UploadUrl;
};

/** Details-only edit. It never creates an asset or requests an artifact upload URL. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarMetadataEdit
{
	FString Name;
	FString Gender;
	/** Empty preserves the existing thumbnail. */
	FString ThumbnailPath;
};

/** Editor-only client. Construct with MakeShared and call from the game thread.
 * BaseUrl is the Assets root (UConvaiURL::GetFullURL("assets", true)).
 * Completion error is empty on success. Account credentials remain in the host;
 * the file worker receives a short-lived signed URL through a private job file.
 */
class CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarAssetsClient : public TSharedFromThis<FConvaiAvatarAssetsClient>
{
public:
	using FListCallback = TFunction<void(TArray<FConvaiAvatarAsset>, FString)>;
	using FAssetCallback = TFunction<void(FConvaiAvatarAsset, FString)>;
	using FImageListCallback = TFunction<void(TArray<FConvaiAvatarImageRecord>, FString)>;
	using FWriteCallback = TFunction<void(FConvaiAvatarAssetWriteResult, FString)>;
	using FCompletion = TFunction<void(FString)>;
	using FProgress = TFunction<void(float)>;
	using FProgressDetails = TFunction<void(const FConvaiAvatarTransferProgress&)>;

	FConvaiAvatarAssetsClient(FString InApiKey, FString InBaseUrl, FString InAuthHeader = TEXT("CONVAI-API-KEY"));
	~FConvaiAvatarAssetsClient();
	void ListAvatars(FListCallback Completion);
	/** One owner-only Avatar API list for images; it never adds records outside this private Assets snapshot. */
	void ListAvatarImages(const TArray<FConvaiAvatarAsset>& PrivateAssets, FImageListCallback Completion);
	void GetAsset(const FString& AssetId, FAssetCallback Completion);
	void CreateOrUpdate(const FConvaiAvatarAssetWrite& Write, FWriteCallback Completion);
	void UpdateMetadata(const FString& AssetId, const FConvaiAvatarMetadataEdit& Edit, FWriteCallback Completion);
	/** Call only after verified Windows PUT. The existing Assets update route commits technical fields without another file/version request. */
	void CommitWindowsMetadata(const FString& AssetId, const FString& Version, int64 ExpectedBytes,
		TSharedPtr<FJsonObject> PreparedMetadata, FWriteCallback Completion);
	void DeleteAsset(const FString& AssetId, FCompletion Completion);
	void UploadFile(const FString& SignedUrl, const FString& FilePath, FCompletion Completion, FProgress Progress = {}, FString DependencyResolutionFile = {}, FProgressDetails ProgressDetails = {});
	/** Download streams to a unique adjacent .part file, then installs the completed file.
	 * Never overwrites an existing destination. Caller obtains fresh signed URLs from GetAsset.
	 */
	void DownloadFile(const FString& SignedUrl, const FString& FilePath, FCompletion Completion, FProgress Progress = {}, FProgressDetails ProgressDetails = {});
	void CancelAll();
	bool IsBusy() const;

	static FString MakeVersion(const FString& Platform);
	/** Preserve V0's asset classification even when the first upload contains only source. */
	static TArray<FString> MakeArtifactTags(const TArray<FString>& ExistingTags, const FString& Platform);
	static bool ParseAsset(const TSharedPtr<FJsonObject>& Json, FConvaiAvatarAsset& OutAsset, FString& OutError);
	static bool ParseAssetResponse(const FString& Json, TArray<FConvaiAvatarAsset>& OutAssets, FString& OutError);
	static bool ParsePrivateAssetResponse(const FString& Json, const FString& AssetId, FConvaiAvatarAsset& OutAsset, FString& OutError);
	/** The library requests and admits only this user's private avatars, regardless of source availability. */
	static TSharedRef<FJsonObject> BuildLibraryQuery(int32 Page);
	static bool ParseLibraryResponse(const FString& Json, TArray<FConvaiAvatarAsset>& OutAssets, FString& OutError);
	static FString AvatarImagesUrlForAssetsRoot(const FString& AssetsRoot);
	static bool ParseAvatarImagesResponse(const FString& Json, const TArray<FConvaiAvatarAsset>& PrivateAssets,
		TArray<FConvaiAvatarImageRecord>& OutImages, FString& OutError);
	static void ApplyAvatarImages(const TArray<FConvaiAvatarImageRecord>& Images, TArray<FConvaiAvatarAsset>& Assets);
	static bool BuildWriteMetadata(const FConvaiAvatarAssetWrite& Write, TSharedPtr<FJsonObject>& OutMetadata, FString& OutError);
	static bool BuildMetadataEdit(const FConvaiAvatarAsset& Current, const FConvaiAvatarMetadataEdit& Edit,
		FConvaiAvatarAssetWrite& OutWrite, FString& OutError);
	static bool BuildArtifactReservation(const FConvaiAvatarAsset& Current, const FString& Version,
		FConvaiAvatarAssetWrite& OutWrite, FString& OutError);
	static bool BuildWindowsMetadataCommit(const FConvaiAvatarAsset& Current, const FString& Version, int64 ExpectedBytes,
		const TSharedPtr<FJsonObject>& PreparedMetadata, FConvaiAvatarAssetWrite& OutWrite, FString& OutError);
	static bool BuildMultipart(const FConvaiAvatarAssetWrite& Write, const FString& Boundary, TArray<uint8>& OutBody, FString& OutError);
	static bool IsStorageUrl(const FString& Url);
	/** Shared display-image gate; unlike artifact URLs, avatar images can use another HTTPS bucket. */
	static bool IsThumbnailUrl(const FString& Url);
	/** Session reconciliation only. Carries an omitted square across reads of the same private avatar and portrait identity. */
	static void PreserveOptionalSquareThumbnail(const FConvaiAvatarAsset& Previous, FConvaiAvatarAsset& Refreshed);

private:
	using FResponseCallback = TFunction<void(FHttpResponsePtr, FString)>;
	using FJsonCallback = TFunction<void(TSharedPtr<FJsonObject>, FString)>;
	void ListPage(int32 Page, TSharedRef<TArray<FConvaiAvatarAsset>> Accumulated, TSharedRef<FListCallback> Completion);
	void JsonRequest(const FString& Endpoint, const TSharedRef<FJsonObject>& Payload, FJsonCallback Completion);
	void Send(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, FResponseCallback Completion);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const FString& Url, const FString& Verb, bool bAuthenticate) const;
	void StartFileTransfer(bool bUpload, const FString& Url, const FString& Path, FCompletion Completion, FProgress Progress, FString DependencyResolutionFile = {}, FProgressDetails ProgressDetails = {});
	void SendWriteRequest(const FConvaiAvatarAssetWrite& Write, FWriteCallback Completion);
	FString ApiKey;
	FString AuthHeader;
	FString BaseUrl;
	uint64 RequestEpoch = 0;
	FConvaiAvatarLibraryState LibraryState;
	TArray<TSharedRef<IHttpRequest, ESPMode::ThreadSafe>> Requests;
	TSharedPtr<FConvaiAvatarFileTransfer, ESPMode::ThreadSafe> FileTransfer;
};
