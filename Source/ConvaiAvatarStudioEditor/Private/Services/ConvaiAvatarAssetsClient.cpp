// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarAssetsClient.h"
#include "Utility/ConvaiEditorHttpCompat.h"
#include "Services/ConvaiAvatarFileTransfer.h"

#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr int64 MaxAvatarBytes = 10485760000LL;
	constexpr int64 MaxThumbnailBytes = 10 * 1024 * 1024;
	constexpr int32 PageSize = 100;

	FString JsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
		return Result;
	}

	bool ReadJson(const FString& Text, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) || !Out.IsValid())
		{
			Error = TEXT("The Assets service returned an unreadable response. Refresh and try again.");
			return false;
		}
		return true;
	}

	FString HttpError(const FHttpResponsePtr& Response, bool bSucceeded)
	{
		if (!bSucceeded || !Response.IsValid())
		{
			return TEXT("The request did not complete. Check your connection, then refresh before retrying an upload.");
		}
		const int32 Code = Response->GetResponseCode();
		if (Code >= 200 && Code < 300) return FString();
		if (Code == 401 || Code == 403) return TEXT("Access was denied. Sign in to Convai again, then refresh your library.");
		if (Code == 404) return TEXT("This avatar or file is no longer available. Refresh your library.");
		if (Code == 413) return TEXT("The file exceeds the upload size allowed by the service.");
		if (Code == 429) return TEXT("Too many requests. Wait a moment and try again.");
		// Do not echo full server responses: they can contain signed URLs or internal exception details.
		return FString::Printf(TEXT("The Assets service could not complete the request (HTTP %d). Refresh before retrying."), Code);
	}

	void AppendUtf8(TArray<uint8>& Body, const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	void AddField(TArray<uint8>& Body, const FString& Boundary, const TCHAR* Name, const FString& Value)
	{
		AppendUtf8(Body, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"), *Boundary, Name, *Value));
	}

	bool ArtifactPlatform(const FString& Version, FString& OutPlatform)
	{
		int32 Separator = INDEX_NONE;
		if (!Version.StartsWith(TEXT("ue-"), ESearchCase::CaseSensitive) || !Version.FindLastChar(TEXT('-'), Separator)) return false;
		const FString Platform = Version.Mid(Separator + 1);
		if (Platform != TEXT("Windows") && Platform != TEXT("Linux") && Platform != TEXT("Raw")) return false;
		TArray<FString> Parts; Version.Mid(3, Separator - 3).ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() != 2) return false;
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || Part.Len() > 5) return false;
			for (TCHAR C : Part) if (C < TEXT('0') || C > TEXT('9')) return false;
		}
		if (FCString::Atoi(*Parts[0]) == 0) return false;
		OutPlatform = Platform;
		return true;
	}

	bool SourceVersion(const FString& Version)
	{
		FString Platform;
		return Version.Equals(TEXT("raw"), ESearchCase::CaseSensitive) ||
			(ArtifactPlatform(Version, Platform) && Platform.Equals(TEXT("Raw"), ESearchCase::CaseSensitive));
	}

	TMap<FString, FString> PrivateAvatarIds(const TArray<FConvaiAvatarAsset>& Assets)
	{
		TMap<FString, FString> Result;
		TSet<FString> Ambiguous;
		for (const FConvaiAvatarAsset& Asset : Assets)
		{
			if (!Asset.IsPrivate() || Asset.EntityType != TEXT("avatar") || Asset.AssetId.IsEmpty() || Asset.AvatarId.IsEmpty()) continue;
			if (const FString* Existing = Result.Find(Asset.AvatarId))
			{
				if (*Existing != Asset.AssetId) Ambiguous.Add(Asset.AvatarId);
			}
			else Result.Add(Asset.AvatarId, Asset.AssetId);
		}
		for (const FString& Id : Ambiguous) Result.Remove(Id);
		return Result;
	}

	bool SafeMetadataName(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > 100) return false;
		for (TCHAR C : Name) if (!(C >= TEXT('A') && C <= TEXT('Z')) && !(C >= TEXT('a') && C <= TEXT('z')) &&
			!(C >= TEXT('0') && C <= TEXT('9')) && C != TEXT('_')) return false;
		return true;
	}

	void ReadVersionInfo(const TSharedPtr<FJsonObject>& Json, FConvaiAvatarAsset& Asset)
	{
		struct FObservation { TSharedPtr<FJsonValue> Url; TSharedPtr<FJsonValue> Size; bool bMalformed = false; };
		TMap<FString, FObservation> Observations;
		const TSharedPtr<FJsonObject>* Versions = nullptr;
		if (Json->TryGetObjectField(TEXT("version_urls"), Versions))
		{
			Asset.bHasArtifactDetails = true;
			for (const auto& Pair : (*Versions)->Values)
				Observations.FindOrAdd(FString(*Pair.Key)).Url = Pair.Value;
		}
		if (Json->TryGetObjectField(TEXT("version_info"), Versions))
		{
			Asset.bHasArtifactDetails = true;
			for (const auto& Pair : (*Versions)->Values)
			{
				FObservation& Observation = Observations.FindOrAdd(FString(*Pair.Key));
				const TSharedPtr<FJsonObject>* Info = nullptr;
				if (!Pair.Value || !Pair.Value->TryGetObject(Info) || !Info || !Info->IsValid()) { Observation.bMalformed = true; continue; }
				if ((*Info)->HasField(TEXT("url"))) Observation.Url = (*Info)->TryGetField(TEXT("url"));
				Observation.Size = (*Info)->TryGetField(TEXT("size"));
			}
		}
		for (const auto& Pair : Observations)
		{
			FConvaiAvatarAssetVersion& Version = Asset.VersionInfo.FindOrAdd(Pair.Key);
			const FObservation& Observation = Pair.Value;
			if (Observation.bMalformed) continue;
			const bool bNullUrl = Observation.Url && Observation.Url->Type == EJson::Null;
			FString Url;
			if (Observation.Url && !bNullUrl && (Observation.Url->Type != EJson::String ||
				!Observation.Url->TryGetString(Url) || !FConvaiAvatarAssetsClient::IsStorageUrl(Url))) continue;
			int64 Size = -1;
			if (Observation.Size)
			{
				double Number = -1;
				if (Observation.Size->Type != EJson::Number || !Observation.Size->TryGetNumber(Number) ||
					!FMath::IsFinite(Number) || Number < 0 || Number > static_cast<double>(MaxAvatarBytes) || FMath::FloorToDouble(Number) != Number) continue;
				Size = static_cast<int64>(Number);
			}
			// The backend allocates versions before PUT. Only observed storage objects count;
			// an explicit null download URL means blob.exists() returned false. Conflicts stay unknown.
			if (bNullUrl && Size > 0) continue;
			if (bNullUrl || Size == 0) Version.Presence = EConvaiAvatarArtifactPresence::Missing;
			else if (!Url.IsEmpty() || Size > 0)
			{
				Version.Presence = EConvaiAvatarArtifactPresence::Available;
				Version.Url = MoveTemp(Url);
				Version.SizeBytes = Size;
			}
		}
	}
}

FString FConvaiAvatarAsset::FindSourceVersion() const
{
	const FString Current = FConvaiAvatarAssetsClient::MakeVersion(TEXT("Raw"));
	auto Available = [this](const FString& Key)
	{
		// FString map lookup is case-insensitive; artifact keys are not. Inspect
		// the returned key itself before offering its URL as a source version.
		for (const auto& Pair : VersionInfo)
			if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive)) return !Pair.Value.Url.IsEmpty();
		return false;
	};
	if (Available(Current)) return Current;
	if (Available(TEXT("raw"))) return TEXT("raw");
	FString Best;
	int32 BestMajor = -1, BestMinor = -1;
	for (const auto& Pair : VersionInfo)
	{
		if (Pair.Value.Url.IsEmpty() || !SourceVersion(Pair.Key) || Pair.Key.Equals(TEXT("raw"), ESearchCase::CaseSensitive)) continue;
		FString Major, Minor;
		const FString Engine = Pair.Key.Mid(3, Pair.Key.Len() - 7);
		if (!Engine.Split(TEXT("."), &Major, &Minor) || !Major.IsNumeric() || !Minor.IsNumeric()) continue;
		const int32 M = FCString::Atoi(*Major), N = FCString::Atoi(*Minor);
		if (M > BestMajor || (M == BestMajor && N > BestMinor)) { Best = Pair.Key; BestMajor = M; BestMinor = N; }
	}
	return Best;
}

bool FConvaiAvatarAsset::HasDeclaredSource() const
{
	return Versions.ContainsByPredicate([](const FString& Version) { return SourceVersion(Version); });
}

EConvaiAvatarArtifactPresence FConvaiAvatarAsset::SourcePresence() const
{
	if (!bHasArtifactDetails) return EConvaiAvatarArtifactPresence::Unknown;
	bool bUnknown = !bHasVersionInventory;
	for (const FString& Version : Versions)
	{
		if (!SourceVersion(Version)) continue;
		const FConvaiAvatarAssetVersion* Info = VersionInfo.Find(Version);
		if (!Info || Info->Presence == EConvaiAvatarArtifactPresence::Unknown) bUnknown = true;
	}
	for (const auto& Pair : VersionInfo)
	{
		if (!SourceVersion(Pair.Key)) continue;
		if (Pair.Value.Presence == EConvaiAvatarArtifactPresence::Available) return EConvaiAvatarArtifactPresence::Available;
		if (Pair.Value.Presence == EConvaiAvatarArtifactPresence::Unknown) bUnknown = true;
	}
	return bUnknown ? EConvaiAvatarArtifactPresence::Unknown : EConvaiAvatarArtifactPresence::Missing;
}

FConvaiAvatarAssetsClient::FConvaiAvatarAssetsClient(FString InApiKey, FString InBaseUrl, FString InAuthHeader)
	: ApiKey(MoveTemp(InApiKey)), AuthHeader(InAuthHeader.ToUpper()), BaseUrl(MoveTemp(InBaseUrl))
{
	while (BaseUrl.EndsWith(TEXT("/"))) BaseUrl.LeftChopInline(1);
	if (AuthHeader != TEXT("CONVAI-API-KEY") && AuthHeader != TEXT("API-AUTH-TOKEN")) AuthHeader.Reset();
}

FConvaiAvatarAssetsClient::~FConvaiAvatarAssetsClient()
{
	CancelAll();
	if (FileTransfer) FileTransfer->Shutdown();
}

bool FConvaiAvatarAssetsClient::IsBusy() const
{
	return !Requests.IsEmpty() || (FileTransfer && FileTransfer->IsBusy());
}

FString FConvaiAvatarAssetsClient::MakeVersion(const FString& Platform)
{
	return FString::Printf(TEXT("ue-%d.%d-%s"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor(), *Platform);
}

TArray<FString> FConvaiAvatarAssetsClient::MakeArtifactTags(const TArray<FString>& ExistingTags, const FString& Platform)
{
	TArray<FString> Tags = ExistingTags;
	Tags.AddUnique(TEXT("Pak"));
	Tags.AddUnique(TEXT("Avatar"));
	if (Platform == TEXT("Raw")) Tags.AddUnique(TEXT("Raw"));
	return Tags;
}

bool FConvaiAvatarAssetsClient::IsStorageUrl(const FString& Url)
{
	// Backend gcp_utils.py signs this bucket. Credentials must never be sent to a URL in an asset record.
	return Url.StartsWith(TEXT("https://storage.googleapis.com/user-assets-storage/"), ESearchCase::IgnoreCase)
		|| Url.StartsWith(TEXT("https://user-assets-storage.storage.googleapis.com/"), ESearchCase::IgnoreCase);
}

bool FConvaiAvatarAssetsClient::IsThumbnailUrl(const FString& Url)
{
	if (!Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) return false;
	for (const TCHAR Character : Url)
		if (Character <= 0x20 || Character == 0x7f || Character == TEXT('\\')) return false;
	int32 AuthorityEnd = 8;
	while (AuthorityEnd < Url.Len() && Url[AuthorityEnd] != TEXT('/') && Url[AuthorityEnd] != TEXT('?') && Url[AuthorityEnd] != TEXT('#')) ++AuthorityEnd;
	const FString Authority = Url.Mid(8, AuthorityEnd - 8);
	return !Authority.IsEmpty() && !Authority.Contains(TEXT("@"));
}

void FConvaiAvatarAssetsClient::PreserveOptionalSquareThumbnail(const FConvaiAvatarAsset& Previous, FConvaiAvatarAsset& Refreshed)
{
	if (Refreshed.bHasSquareThumbnailField || !Refreshed.SquareThumbnailUrl.IsEmpty() ||
		!IsThumbnailUrl(Previous.SquareThumbnailUrl) || !Previous.IsPrivate() || !Refreshed.IsPrivate() ||
		Previous.AssetId.IsEmpty() || Previous.AssetId != Refreshed.AssetId ||
		Previous.AvatarId.IsEmpty() || Previous.AvatarId != Refreshed.AvatarId) return;
	// The explicit storage path bridges create (no signed portrait URL) and later
	// URL rotation. If either read supplies it, both must agree; absence or mismatch
	// cannot be papered over with an older URL. No URL is ever made from this path.
	const bool bHasStorageIdentity = !Previous.ThumbnailStoragePath.IsEmpty() || !Refreshed.ThumbnailStoragePath.IsEmpty();
	const bool bSamePortrait = bHasStorageIdentity
		? !Previous.ThumbnailStoragePath.IsEmpty() && Previous.ThumbnailStoragePath == Refreshed.ThumbnailStoragePath
		: IsThumbnailUrl(Previous.ThumbnailUrl) && Previous.ThumbnailUrl == Refreshed.ThumbnailUrl;
	if (bSamePortrait) Refreshed.SquareThumbnailUrl = Previous.SquareThumbnailUrl;
}

bool FConvaiAvatarAssetsClient::ParseAsset(const TSharedPtr<FJsonObject>& Json, FConvaiAvatarAsset& OutAsset, FString& OutError)
{
	OutError.Reset();
	if (!Json.IsValid()) { OutError = TEXT("The Assets service returned an empty avatar record."); return false; }
	const TSharedPtr<FJsonObject>* Nested = nullptr;
	const TSharedPtr<FJsonObject> Record = Json->TryGetObjectField(TEXT("asset"), Nested) ? *Nested : Json;
	FConvaiAvatarAsset Asset;
	if (!Record->TryGetStringField(TEXT("asset_id"), Asset.AssetId) || Asset.AssetId.IsEmpty())
	{
		OutError = TEXT("The Assets service returned an avatar without an asset ID.");
		return false;
	}
	Record->TryGetStringField(TEXT("entity_type"), Asset.EntityType);
	Record->TryGetStringField(TEXT("visibility"), Asset.Visibility);
	Asset.Visibility = Asset.Visibility.TrimStartAndEnd().ToLower();
	Record->TryGetStringField(TEXT("thumbnail_url"), Asset.ThumbnailUrl);
	Record->TryGetStringField(TEXT("thumbnail_gcp_path"), Asset.ThumbnailStoragePath);
	const TArray<TSharedPtr<FJsonValue>>* VersionValues = nullptr;
	if (Record->TryGetArrayField(TEXT("versions"), VersionValues))
	{
		Asset.bHasVersionInventory = true;
		for (const auto& Value : *VersionValues)
		{
			FString Version;
			if (!Value || Value->Type != EJson::String || !Value->TryGetString(Version) || Version.IsEmpty())
			{ Asset.bHasVersionInventory = false; Asset.Versions.Reset(); break; }
			Asset.Versions.AddUnique(Version);
		}
	}
	Record->TryGetStringArrayField(TEXT("tags"), Asset.Tags);
	if (Record->TryGetObjectField(TEXT("metadata"), Nested)) Asset.Metadata = *Nested;
	else
	{
		FString MetadataText;
		if (Record->TryGetStringField(TEXT("metadata"), MetadataText))
		{
			if (!ReadJson(MetadataText, Asset.Metadata, OutError)) return false;
		}
		else Asset.Metadata = MakeShared<FJsonObject>();
	}
	Asset.Metadata->TryGetStringField(TEXT("avatar_id"), Asset.AvatarId);
	Asset.Metadata->TryGetStringField(TEXT("asset_name"), Asset.Name);
	if (Asset.Metadata->TryGetObjectField(TEXT("entity_data"), Nested))
	{
		// Staging uses avatar_name as the Blueprint package leaf. asset_name is the creator's label.
		if (Asset.Name.IsEmpty()) (*Nested)->TryGetStringField(TEXT("avatar_name"), Asset.Name);
		(*Nested)->TryGetStringField(TEXT("gender"), Asset.Gender);
	}
	// GET/list currently omit this field. Accept a supplied value only; creation also
	// returns the linked avatar row beside "asset". A present null/invalid field is
	// authoritative, so a stale lower-priority metadata value cannot resurrect it.
	TSharedPtr<FJsonObject> SquareRecord;
	if (Record->HasField(TEXT("avatar_image_square"))) SquareRecord = Record;
	else if (Json != Record && Json->TryGetObjectField(TEXT("avatar"), Nested))
	{
		FString LinkedAvatarId;
		if ((*Nested)->TryGetStringField(TEXT("avatar_id"), LinkedAvatarId) && !Asset.AvatarId.IsEmpty() &&
			LinkedAvatarId == Asset.AvatarId && (*Nested)->HasField(TEXT("avatar_image_square"))) SquareRecord = *Nested;
	}
	if (!SquareRecord && Asset.Metadata->HasField(TEXT("avatar_image_square"))) SquareRecord = Asset.Metadata;
	if (!SquareRecord && Asset.Metadata->TryGetObjectField(TEXT("entity_data"), Nested) &&
		(*Nested)->HasField(TEXT("avatar_image_square"))) SquareRecord = *Nested;
	FString SquareUrl;
	Asset.bHasSquareThumbnailField = SquareRecord.IsValid();
	if (SquareRecord && SquareRecord->TryGetStringField(TEXT("avatar_image_square"), SquareUrl) && IsThumbnailUrl(SquareUrl))
		Asset.SquareThumbnailUrl = MoveTemp(SquareUrl);
	if (Asset.Name.IsEmpty()) Asset.Name = Asset.AssetId;
	ReadVersionInfo(Record, Asset);
	OutAsset = MoveTemp(Asset);
	return true;
}

bool FConvaiAvatarAssetsClient::ParseAssetResponse(const FString& Json, TArray<FConvaiAvatarAsset>& OutAssets, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!ReadJson(Json, Root, OutError)) return false;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("assets"), Items)) { OutError = TEXT("The Assets response does not contain an asset list."); return false; }
	TArray<FConvaiAvatarAsset> Result;
	for (const TSharedPtr<FJsonValue>& Value : *Items)
	{
		const TSharedPtr<FJsonObject>* Item = nullptr;
		FConvaiAvatarAsset Asset;
		if (!Value->TryGetObject(Item) || !Item || !ParseAsset(*Item, Asset, OutError))
		{
			if (OutError.IsEmpty()) OutError = TEXT("The Assets service returned an invalid asset record.");
			return false;
		}
		Result.Add(MoveTemp(Asset));
	}
	OutAssets = MoveTemp(Result);
	OutError.Reset();
	return true;
}

TSharedRef<FJsonObject> FConvaiAvatarAssetsClient::BuildLibraryQuery(int32 Page)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("page"), Page);
	Payload->SetNumberField(TEXT("per_page"), PageSize);
	// Listing only needs the declared inventory. Signed URLs and blob-size probes
	// are deferred to GetAsset for the selected avatar or an explicit file action.
	Payload->SetBoolField(TEXT("generate_urls"), false);
	Payload->SetBoolField(TEXT("generate_urls_for_versions"), false);
	// The backend keeps the owner filter for private visibility. Never request its public catalog.
	Payload->SetStringField(TEXT("visibility"), TEXT("private"));
	return Payload;
}

bool FConvaiAvatarAssetsClient::ParsePrivateAssetResponse(const FString& Json, const FString& AssetId, FConvaiAvatarAsset& OutAsset, FString& OutError)
{
	OutAsset = {};
	TArray<FConvaiAvatarAsset> Assets;
	if (!ParseAssetResponse(Json, Assets, OutError)) return false;
	if (Assets.Num() != 1 || Assets[0].AssetId != AssetId || Assets[0].EntityType != TEXT("avatar"))
	{
		OutError = TEXT("The service returned a different asset. Refresh the avatar library.");
		return false;
	}
	if (!Assets[0].IsPrivate())
	{
		OutError = TEXT("Cloud Avatars can open or change only your private avatars. Refresh the library and select a private avatar.");
		return false;
	}
	OutAsset = MoveTemp(Assets[0]);
	return true;
}

bool FConvaiAvatarAssetsClient::ParseLibraryResponse(const FString& Json, TArray<FConvaiAvatarAsset>& OutAssets, FString& OutError)
{
	OutAssets.Reset();
	TArray<FConvaiAvatarAsset> Assets;
	if (!ParseAssetResponse(Json, Assets, OutError)) return false;
	Assets.RemoveAll([](const FConvaiAvatarAsset& Asset) { return Asset.EntityType != TEXT("avatar") || !Asset.IsPrivate(); });
	OutAssets = MoveTemp(Assets);
	return true;
}

FString FConvaiAvatarAssetsClient::AvatarImagesUrlForAssetsRoot(const FString& AssetsRoot)
{
	FString Root = AssetsRoot;
	while (Root.EndsWith(TEXT("/"))) Root.LeftChopInline(1);
	const int32 SchemeLength = Root.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase) ? 8 :
		(Root.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ? 7 : 0);
	if (!SchemeLength || !Root.EndsWith(TEXT("/assets"), ESearchCase::CaseSensitive) || Root.Contains(TEXT("?")) || Root.Contains(TEXT("#"))) return FString();
	for (TCHAR Character : Root) if (Character <= 0x20 || Character == 0x7f || Character == TEXT('\\')) return FString();
	int32 AuthorityEnd = SchemeLength;
	while (AuthorityEnd < Root.Len() && Root[AuthorityEnd] != TEXT('/')) ++AuthorityEnd;
	const FString Authority = Root.Mid(SchemeLength, AuthorityEnd - SchemeLength);
	if (Authority.IsEmpty() || Authority.Contains(TEXT("@")) || AuthorityEnd > Root.Len() - 7) return FString();
	// The constructor's configured Assets server is trusted. Never obtain an API
	// origin from a record, and never switch a beta/custom Assets host to production.
	return Root.LeftChop(7) + TEXT("/character/avatars/list");
}

bool FConvaiAvatarAssetsClient::ParseAvatarImagesResponse(const FString& Json, const TArray<FConvaiAvatarAsset>& PrivateAssets,
	TArray<FConvaiAvatarImageRecord>& OutImages, FString& OutError)
{
	OutImages.Reset();
	OutError.Reset();
	if (Json.Len() > 32 * 1024 * 1024) { OutError = TEXT("The avatar image list is too large to read. The rest of your library is still available."); return false; }
	TSharedPtr<FJsonObject> Root;
	if (!ReadJson(Json, Root, OutError)) return false;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Root->TryGetArrayField(TEXT("avatars"), Rows) || Rows->Num() > 100000)
	{ OutError = TEXT("The service returned an unreadable avatar image list. Refresh to try again."); return false; }
	const TMap<FString, FString> Allowed = PrivateAvatarIds(PrivateAssets);
	TSet<FString> Seen;
	TSet<FString> DuplicateIds;
	for (const auto& Value : *Rows)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (!Value || !Value->TryGetObject(Row) || !Row || !Row->IsValid()) continue;
		FString AvatarId, Visibility;
		if (!(*Row)->TryGetStringField(TEXT("avatar_id"), AvatarId) || !(*Row)->TryGetStringField(TEXT("visibility"), Visibility) ||
			!Visibility.TrimStartAndEnd().Equals(TEXT("private"), ESearchCase::IgnoreCase)) continue;
		const FString* AssetId = Allowed.Find(AvatarId);
		if (!AssetId) continue;
		if (Seen.Contains(AvatarId)) { DuplicateIds.Add(AvatarId); continue; }
		Seen.Add(AvatarId);
		// Current DB values are objects; older records can contain serialized JSON.
		// A supplied asset_id must match even though the private Assets row already
		// supplies a unique avatar_id -> asset_id join for legacy config omissions.
		if ((*Row)->HasField(TEXT("avatar_config")))
		{
			const TSharedPtr<FJsonValue> ConfigValue = (*Row)->TryGetField(TEXT("avatar_config"));
			TSharedPtr<FJsonObject> Config;
			if (ConfigValue && ConfigValue->Type == EJson::Object) Config = ConfigValue->AsObject();
			else if (ConfigValue && ConfigValue->Type == EJson::String)
			{
				FString IgnoredError;
				if (!ReadJson(ConfigValue->AsString(), Config, IgnoredError)) continue;
			}
			else if (!ConfigValue || ConfigValue->Type != EJson::Null) continue;
			if (Config && Config->HasField(TEXT("asset_id")))
			{
				FString ConfigAssetId;
				if (!Config->TryGetStringField(TEXT("asset_id"), ConfigAssetId) || ConfigAssetId != *AssetId) continue;
			}
		}
		FConvaiAvatarImageRecord Image;
		Image.AssetId = *AssetId;
		Image.AvatarId = AvatarId;
		Image.bHasThumbnailField = (*Row)->HasField(TEXT("avatar_image"));
		Image.bHasSquareThumbnailField = (*Row)->HasField(TEXT("avatar_image_square"));
		FString Url;
		if ((*Row)->TryGetStringField(TEXT("avatar_image"), Url) && IsThumbnailUrl(Url)) Image.ThumbnailUrl = MoveTemp(Url);
		if ((*Row)->TryGetStringField(TEXT("avatar_image_square"), Url) && IsThumbnailUrl(Url)) Image.SquareThumbnailUrl = MoveTemp(Url);
		OutImages.Add(MoveTemp(Image));
	}
	OutImages.RemoveAll([&DuplicateIds](const FConvaiAvatarImageRecord& Image) { return DuplicateIds.Contains(Image.AvatarId); });
	return true;
}

void FConvaiAvatarAssetsClient::ApplyAvatarImages(const TArray<FConvaiAvatarImageRecord>& Images, TArray<FConvaiAvatarAsset>& Assets)
{
	const TMap<FString, FString> Allowed = PrivateAvatarIds(Assets);
	TMap<FString, const FConvaiAvatarImageRecord*> ByAvatar;
	TSet<FString> DuplicateIds;
	for (const FConvaiAvatarImageRecord& Image : Images)
	{
		const FString* AssetId = Allowed.Find(Image.AvatarId);
		if (!AssetId || *AssetId != Image.AssetId) continue;
		if (ByAvatar.Contains(Image.AvatarId)) DuplicateIds.Add(Image.AvatarId);
		else ByAvatar.Add(Image.AvatarId, &Image);
	}
	for (FConvaiAvatarAsset& Asset : Assets)
	{
		if (!Asset.IsPrivate() || Asset.EntityType != TEXT("avatar") || DuplicateIds.Contains(Asset.AvatarId)) continue;
		const FConvaiAvatarImageRecord* const* Found = ByAvatar.Find(Asset.AvatarId);
		if (!Found || (*Found)->AssetId != Asset.AssetId) continue;
		const FConvaiAvatarImageRecord& Image = **Found;
		if (Image.bHasThumbnailField)
		{
			const FString Portrait = IsThumbnailUrl(Image.ThumbnailUrl) ? Image.ThumbnailUrl : FString();
			// A partial image response supplies no proof that an older square still
			// belongs to a changed portrait. Full owner rows normally supply both.
			if (!Image.bHasSquareThumbnailField && Asset.ThumbnailUrl != Portrait) Asset.SquareThumbnailUrl.Reset();
			Asset.ThumbnailUrl = Portrait;
		}
		if (Image.bHasSquareThumbnailField)
		{
			Asset.bHasSquareThumbnailField = true;
			Asset.SquareThumbnailUrl = IsThumbnailUrl(Image.SquareThumbnailUrl) ? Image.SquareThumbnailUrl : FString();
		}
	}
}

bool FConvaiAvatarAssetsClient::BuildWriteMetadata(const FConvaiAvatarAssetWrite& Write, TSharedPtr<FJsonObject>& OutMetadata, FString& OutError)
{
	OutError.Reset();
	if (Write.bArtifactReservationOnly)
	{
		FString Platform;
		if (Write.AssetId.IsEmpty() || !ArtifactPlatform(Write.Version, Platform) || Write.bMetadataOnly || !Write.ThumbnailPath.IsEmpty())
		{ OutError = TEXT("An upload reservation requires an existing avatar and one artifact version, without metadata or thumbnail changes."); return false; }
		// Kept only for the local response model. BuildMultipart deliberately does not send it.
		if (Write.Metadata) return ReadJson(JsonString(Write.Metadata.ToSharedRef()), OutMetadata, OutError);
		OutMetadata = MakeShared<FJsonObject>();
		return true;
	}
	if (Write.Name.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("Enter a name for this avatar."); return false; }
	if (Write.bMetadataOnly && (Write.AssetId.IsEmpty() || !Write.Version.IsEmpty())) { OutError=TEXT("A details-only save cannot create an avatar or replace uploaded files."); return false; }
	if (Write.Gender != TEXT("male") && Write.Gender != TEXT("female")) { OutError = TEXT("Choose a Gender for this avatar."); return false; }
	if (Write.AssetId.IsEmpty() && Write.Version.IsEmpty()) { OutError = TEXT("Choose Source, Windows, or Linux to upload."); return false; }
	TSharedPtr<FJsonObject> Metadata;
	if (Write.Metadata.IsValid())
	{
		if (!ReadJson(JsonString(Write.Metadata.ToSharedRef()), Metadata, OutError)) return false;
	}
	else Metadata = MakeShared<FJsonObject>();
	if (!Write.AssetId.IsEmpty())
	{
		FString EntityId, AvatarId;
		if (!Metadata->TryGetStringField(TEXT("entity_id"), EntityId) || EntityId.IsEmpty()
			|| !Metadata->TryGetStringField(TEXT("avatar_id"), AvatarId) || AvatarId.IsEmpty())
		{
			OutError = TEXT("Refresh this avatar before updating it. Its existing server metadata is required.");
			return false;
		}
	}
	Metadata->SetStringField(TEXT("asset_type"), TEXT("avatar"));
	Metadata->SetStringField(TEXT("asset_name"), Write.Name.TrimStartAndEnd());
	const TSharedPtr<FJsonObject>* ExistingEntity = nullptr;
	TSharedPtr<FJsonObject> Entity = Metadata->TryGetObjectField(TEXT("entity_data"), ExistingEntity) ? *ExistingEntity : MakeShared<FJsonObject>();
	if (Write.bMetadataOnly)
	{
		FString ExistingName;
		const TSharedPtr<FJsonObject>* ExistingConfig = nullptr;
		if (!Entity->TryGetStringField(TEXT("avatar_name"),ExistingName) || ExistingName.IsEmpty() || !Entity->TryGetObjectField(TEXT("avatar_config"),ExistingConfig))
		{
			OutError=TEXT("This avatar's saved details are incomplete. Refresh it before editing; its existing Blueprint configuration must be preserved.");
			return false;
		}
	}
	FString ClassPath;
	Metadata->TryGetStringField(TEXT("blueprint_class_path"), ClassPath);
	if (!Write.bMetadataOnly && !ClassPath.IsEmpty())
	{
		int32 LastSlash = INDEX_NONE;
		Entity->SetStringField(TEXT("avatar_name"), ClassPath.FindLastChar(TEXT('/'), LastSlash) ? ClassPath.Mid(LastSlash + 1) : ClassPath);
	}
	else if (!Write.bMetadataOnly)
	{
		FString ExistingName;
		if (!Entity->TryGetStringField(TEXT("avatar_name"), ExistingName) || ExistingName.IsEmpty()) Entity->SetStringField(TEXT("avatar_name"), Write.Name.TrimStartAndEnd());
	}
	Entity->SetStringField(TEXT("gender"), Write.Gender);
	const TSharedPtr<FJsonObject>* Config = nullptr;
	if (!Entity->TryGetObjectField(TEXT("avatar_config"), Config)) Entity->SetObjectField(TEXT("avatar_config"), MakeShared<FJsonObject>());
	Metadata->SetObjectField(TEXT("entity_data"), Entity);
	OutMetadata = MoveTemp(Metadata);
	return true;
}

bool FConvaiAvatarAssetsClient::BuildMultipart(const FConvaiAvatarAssetWrite& Write, const FString& Boundary, TArray<uint8>& OutBody, FString& OutError)
{
	if (Boundary.IsEmpty() || Boundary.Contains(TEXT("\r")) || Boundary.Contains(TEXT("\n"))) { OutError = TEXT("Invalid upload request boundary."); return false; }
	TSharedPtr<FJsonObject> Metadata;
	if (!BuildWriteMetadata(Write, Metadata, OutError)) return false;
	if (Write.AssetId.IsEmpty() && Write.ThumbnailPath.IsEmpty()) { OutError = TEXT("Choose a PNG or JPEG preview image before creating this avatar."); return false; }
	TArray<uint8> Body;
	if (!Write.bArtifactReservationOnly) AddField(Body, Boundary, TEXT("metadata"), JsonString(Metadata.ToSharedRef()));
	TArray<TSharedPtr<FJsonValue>> Tags;
	for (const FString& Tag : Write.Tags) Tags.Add(MakeShared<FJsonValueString>(Tag));
	if (Tags.IsEmpty() && Write.AssetId.IsEmpty()) Tags.Add(MakeShared<FJsonValueString>(TEXT("avatar")));
	FString TagsText;
	FJsonSerializer::Serialize(Tags, TJsonWriterFactory<>::Create(&TagsText));
	// An omitted tags field preserves existing tags on update; sending a default would replace them.
	if (!Tags.IsEmpty()) AddField(Body, Boundary, TEXT("tags"), TagsText);
	if (!Write.Version.IsEmpty()) AddField(Body, Boundary, TEXT("version"), Write.Version);
	if (Write.AssetId.IsEmpty())
	{
		AddField(Body, Boundary, TEXT("entity_type"), TEXT("avatar"));
		AddField(Body, Boundary, TEXT("visibility"), TEXT("private"));
	}
	else AddField(Body, Boundary, TEXT("asset_id"), Write.AssetId);
	if (!Write.ThumbnailPath.IsEmpty())
	{
		const FString Extension = FPaths::GetExtension(Write.ThumbnailPath).ToLower();
		const int64 Size = IFileManager::Get().FileSize(*Write.ThumbnailPath);
		if ((Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg")) || Size <= 0 || Size > MaxThumbnailBytes)
		{
			OutError = TEXT("Choose an existing PNG or JPEG preview image smaller than 10 MB.");
			return false;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Write.ThumbnailPath)) { OutError = TEXT("The preview image could not be read."); return false; }
		AppendUtf8(Body, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"thumbnail\"; filename=\"thumbnail.%s\"\r\nContent-Type: %s\r\n\r\n"), *Boundary, *Extension, Extension == TEXT("png") ? TEXT("image/png") : TEXT("image/jpeg")));
		Body.Append(Bytes);
		AppendUtf8(Body, TEXT("\r\n"));
	}
	AppendUtf8(Body, FString::Printf(TEXT("--%s--\r\n"), *Boundary));
	OutBody = MoveTemp(Body);
	return true;
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> FConvaiAvatarAssetsClient::MakeRequest(const FString& Url, const FString& Verb, bool bAuthenticate) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	Request->SetTimeout(bAuthenticate ? 120.0f : 3600.0f);
	if (bAuthenticate && !AuthHeader.IsEmpty()) Request->SetHeader(AuthHeader, ApiKey);
	return Request;
}

void FConvaiAvatarAssetsClient::Send(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, FResponseCallback Completion)
{
	check(IsInGameThread());
	TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	const uint64 Epoch = RequestEpoch;
	const TSharedRef<FResponseCallback> Callback = MakeShared<FResponseCallback>(MoveTemp(Completion));
	const TSharedRef<bool> Completed = MakeShared<bool>(false);
	Request->OnProcessRequestComplete().BindLambda([WeakOwner, Epoch, Callback, Completed](FHttpRequestPtr Finished, FHttpResponsePtr Response, bool bSucceeded)
	{
		if (*Completed) return;
		*Completed = true;
		if (const TSharedPtr<FConvaiAvatarAssetsClient> Self = WeakOwner.Pin())
		{
			Self->Requests.RemoveAll([&Finished](const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Item) { return Item == Finished; });
			(*Callback)(Response, Self->RequestEpoch == Epoch ? HttpError(Response, bSucceeded) : TEXT("Cancelled."));
		}
	});
	Requests.Add(Request);
	if (!Request->ProcessRequest())
	{
		Requests.Remove(Request);
		if (!*Completed) { *Completed = true; (*Callback)(nullptr, TEXT("The request could not start. Check your connection and try again.")); }
	}
}

void FConvaiAvatarAssetsClient::JsonRequest(const FString& Endpoint, const TSharedRef<FJsonObject>& Payload, FJsonCallback Completion)
{
	if (ApiKey.TrimStartAndEnd().IsEmpty() || AuthHeader.IsEmpty()) { Completion(nullptr, TEXT("Sign in to Convai to open your avatar library.")); return; }
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(BaseUrl / Endpoint, TEXT("POST"), true);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(JsonString(Payload));
	Send(Request, [Completion = MoveTemp(Completion)](FHttpResponsePtr Response, FString Error)
	{
		TSharedPtr<FJsonObject> Root;
		if (Error.IsEmpty()) ReadJson(Response->GetContentAsString(), Root, Error);
		Completion(Root, MoveTemp(Error));
	});
}

void FConvaiAvatarAssetsClient::ListAvatars(FListCallback Completion)
{
	ListPage(1, MakeShared<TArray<FConvaiAvatarAsset>>(), MakeShared<FListCallback>(MoveTemp(Completion)));
}

void FConvaiAvatarAssetsClient::ListAvatarImages(const TArray<FConvaiAvatarAsset>& PrivateAssets, FImageListCallback Completion)
{
	check(IsInGameThread());
	if (PrivateAvatarIds(PrivateAssets).IsEmpty()) { Completion({}, FString()); return; }
	if (ApiKey.TrimStartAndEnd().IsEmpty() || AuthHeader.IsEmpty()) { Completion({}, TEXT("Sign in to Convai to load your avatar images.")); return; }
	const FString Url = AvatarImagesUrlForAssetsRoot(BaseUrl);
	if (Url.IsEmpty()) { Completion({}, TEXT("Avatar images could not be requested from the configured service. The rest of your library is still available.")); return; }
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(Url, TEXT("POST"), true);
	Request->SetTimeout(30.0f);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	// Empty input selects this signed-in user's avatars. Never send type=public.
	// This endpoint returns the owner list in one batch and has no pagination.
	Request->SetContentAsString(TEXT("{}"));
	Send(Request, [PrivateAssets, Completion = MoveTemp(Completion)](FHttpResponsePtr Response, FString Error)
	{
		TArray<FConvaiAvatarImageRecord> Images;
		if (Error.IsEmpty()) ParseAvatarImagesResponse(Response->GetContentAsString(), PrivateAssets, Images, Error);
		Completion(MoveTemp(Images), MoveTemp(Error));
	});
}

void FConvaiAvatarAssetsClient::ListPage(int32 Page, TSharedRef<TArray<FConvaiAvatarAsset>> Accumulated, TSharedRef<FListCallback> Completion)
{
	const TSharedRef<FJsonObject> Payload = BuildLibraryQuery(Page);
	TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	JsonRequest(TEXT("list"), Payload, [WeakOwner, Page, Accumulated, Completion](TSharedPtr<FJsonObject> Root, FString Error)
	{
		const TSharedPtr<FConvaiAvatarAssetsClient> Self = WeakOwner.Pin();
		if (!Self) return;
		if (!Error.IsEmpty()) { (*Completion)({}, MoveTemp(Error)); return; }
		TArray<FConvaiAvatarAsset> Assets;
		if (!ParseLibraryResponse(JsonString(Root.ToSharedRef()), Assets, Error)) { (*Completion)({}, MoveTemp(Error)); return; }
		for (FConvaiAvatarAsset& Asset : Assets)
		{
			if (Asset.EntityType == TEXT("avatar") && !Accumulated->ContainsByPredicate([&Asset](const FConvaiAvatarAsset& Existing) { return Existing.AssetId == Asset.AssetId; }))
				Accumulated->Add(MoveTemp(Asset));
		}
		int32 TotalPages = Page;
		Root->TryGetNumberField(TEXT("total_pages"), TotalPages);
		if (Page < TotalPages)
		{
			if (Page >= 1000) { (*Completion)({}, TEXT("The avatar library is too large to retrieve in one request. Try again with a smaller account library.")); return; }
			Self->ListPage(Page + 1, Accumulated, Completion);
		}
		else
		{
			// Deletion can finish after an earlier page has already been accumulated.
			Self->LibraryState.RemoveDeleted(*Accumulated);
			(*Completion)(MoveTemp(*Accumulated), FString());
		}
	});
}

void FConvaiAvatarAssetsClient::GetAsset(const FString& AssetId, FAssetCallback Completion)
{
	if (AssetId.IsEmpty()) { Completion({}, TEXT("Choose an avatar first.")); return; }
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_id"), AssetId);
	JsonRequest(TEXT("get"), Payload, [AssetId, Completion = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FConvaiAvatarAsset Asset;
		if (Error.IsEmpty()) ParsePrivateAssetResponse(JsonString(Root.ToSharedRef()), AssetId, Asset, Error);
		Completion(MoveTemp(Asset), MoveTemp(Error));
	});
}

void FConvaiAvatarAssetsClient::CreateOrUpdate(const FConvaiAvatarAssetWrite& Write, FWriteCallback Completion)
{
	check(IsInGameThread());
	if (Write.AssetId.IsEmpty()) { SendWriteRequest(Write, MoveTemp(Completion)); return; }
	// Do not trust a stale selection or caller-supplied metadata to authorize changing a public asset.
	const TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	GetAsset(Write.AssetId, [WeakOwner, Write, Completion = MoveTemp(Completion)](FConvaiAvatarAsset Current, FString Error) mutable
	{
		const auto Self = WeakOwner.Pin();
		if (!Self) return;
		if (!Error.IsEmpty()) { Completion({}, MoveTemp(Error)); return; }
		if (Write.bArtifactReservationOnly)
		{
			FConvaiAvatarAssetWrite Reservation;
			if (!BuildArtifactReservation(Current, Write.Version, Reservation, Error)) { Completion({}, MoveTemp(Error)); return; }
			Self->SendWriteRequest(Reservation, MoveTemp(Completion));
			return;
		}
		Self->SendWriteRequest(Write, MoveTemp(Completion));
	});
}

bool FConvaiAvatarAssetsClient::BuildArtifactReservation(const FConvaiAvatarAsset& Current, const FString& Version,
	FConvaiAvatarAssetWrite& OutWrite, FString& OutError)
{
	OutWrite = {}; OutError.Reset();
	FString Platform;
	if (Current.AssetId.IsEmpty() || Current.EntityType != TEXT("avatar") || !Current.IsPrivate() || !ArtifactPlatform(Version, Platform))
	{ OutError = TEXT("Refresh the private avatar and choose a supported upload version before reserving its file."); return false; }
	FConvaiAvatarAssetWrite Write;
	Write.AssetId = Current.AssetId; Write.Version = Version; Write.Name = Current.Name; Write.Gender = Current.Gender;
	Write.Tags = MakeArtifactTags(Current.Tags, Platform);
	Write.Metadata = Current.Metadata; Write.bArtifactReservationOnly = true;
	OutWrite = MoveTemp(Write);
	return true;
}

bool FConvaiAvatarAssetsClient::BuildWindowsMetadataCommit(const FConvaiAvatarAsset& Current, const FString& Version, int64 ExpectedBytes,
	const TSharedPtr<FJsonObject>& PreparedMetadata, FConvaiAvatarAssetWrite& OutWrite, FString& OutError)
{
	OutWrite = {}; OutError.Reset();
	FString Platform;
	const auto* Uploaded = Current.VersionInfo.Find(Version);
	if (Current.AssetId.IsEmpty() || Current.EntityType != TEXT("avatar") || !Current.IsPrivate() || !ArtifactPlatform(Version, Platform) || Platform != TEXT("Windows") ||
		ExpectedBytes <= 0 || ExpectedBytes > MaxAvatarBytes || !Uploaded || Uploaded->Presence != EConvaiAvatarArtifactPresence::Available || Uploaded->SizeBytes != ExpectedBytes)
	{ OutError = TEXT("The uploaded Windows file could not be confirmed before saving its avatar setup. Refresh, then retry Upload changes with Windows selected."); return false; }
	TSharedPtr<FJsonObject> Metadata;
	if (!Current.Metadata || !PreparedMetadata || !ReadJson(JsonString(Current.Metadata.ToSharedRef()), Metadata, OutError))
	{ if (OutError.IsEmpty()) OutError = TEXT("The avatar setup is incomplete. Refresh before retrying the Windows upload."); return false; }
	const TCHAR* StringFields[] = {TEXT("project_name"), TEXT("plugin_name"), TEXT("root_path"), TEXT("content_path"), TEXT("blueprint_class_path"), TEXT("blueprint_class")};
	for (const TCHAR* Field : StringFields)
	{
		const auto Value = PreparedMetadata->TryGetField(Field);
		if (!Value || Value->Type != EJson::String || Value->AsString().IsEmpty())
		{ OutError = TEXT("The prepared Windows avatar is missing its project, plugin, or Blueprint setup. Prepare it again before retrying."); return false; }
	}
	const FString Plugin = PreparedMetadata->GetStringField(TEXT("plugin_name")), Project = PreparedMetadata->GetStringField(TEXT("project_name"));
	const FString ClassPath = PreparedMetadata->GetStringField(TEXT("blueprint_class_path"));
	FString ClassObject = PreparedMetadata->GetStringField(TEXT("blueprint_class"));
	FString OldPlugin;
	const auto MetaHuman = PreparedMetadata->TryGetField(TEXT("is_metahuman"));
	if (!SafeMetadataName(Plugin) || !SafeMetadataName(Project) || !MetaHuman || MetaHuman->Type != EJson::Boolean ||
		(Metadata->TryGetStringField(TEXT("plugin_name"), OldPlugin) && !OldPlugin.IsEmpty() && OldPlugin != Plugin) ||
		PreparedMetadata->GetStringField(TEXT("root_path")) != TEXT("/") + Plugin + TEXT("/") ||
		PreparedMetadata->GetStringField(TEXT("content_path")) != TEXT("../../../") + Project + TEXT("/Plugins/ConvaiAvatars/") + Plugin + TEXT("/Content/") ||
		ClassPath.Len() >= NAME_SIZE || !FPackageName::IsValidTextForLongPackageName(ClassPath) || !ClassPath.StartsWith(TEXT("/") + Plugin + TEXT("/"), ESearchCase::CaseSensitive) ||
		!ClassObject.RemoveFromStart(TEXT("/Script/Engine.BlueprintGeneratedClass'"), ESearchCase::CaseSensitive) || !ClassObject.RemoveFromEnd(TEXT("'")) ||
		ClassObject.Len() >= 2 * NAME_SIZE || ClassObject.Contains(TEXT(":")) || !FPackageName::IsValidObjectPath(ClassObject) ||
		FSoftObjectPath(ClassObject).GetLongPackageName() != ClassPath || !FSoftObjectPath(ClassObject).GetAssetName().EndsWith(TEXT("_C")))
	{ OutError = TEXT("The prepared Windows setup does not match this avatar's permanent plugin and Blueprint. Prepare the avatar again before retrying."); return false; }
	const TSharedPtr<FJsonObject>* Entity = nullptr;
	if (!Metadata->TryGetObjectField(TEXT("entity_data"), Entity))
	{ OutError = TEXT("Refresh this avatar before saving its Windows setup; its existing entity settings must be preserved."); return false; }
	for (const TCHAR* Field : StringFields) Metadata->SetStringField(Field, PreparedMetadata->GetStringField(Field));
	Metadata->SetBoolField(TEXT("is_metahuman"), MetaHuman->AsBool());
	Metadata->SetNumberField(TEXT("Windows_PakSize"), static_cast<double>(ExpectedBytes));
	// Match the established uploader's class-leaf convention without replacing avatar_config.
	(*Entity)->SetStringField(TEXT("avatar_name"), FPackageName::GetShortName(ClassPath));
	FConvaiAvatarAssetWrite Write;
	Write.AssetId = Current.AssetId; Write.Name = Current.Name; Write.Gender = Current.Gender; Write.Tags = Current.Tags;
	Write.Metadata = Metadata; Write.bMetadataOnly = true;
	if (!BuildWriteMetadata(Write, Write.Metadata, OutError)) return false;
	OutWrite = MoveTemp(Write);
	return true;
}

void FConvaiAvatarAssetsClient::CommitWindowsMetadata(const FString& AssetId, const FString& Version, int64 ExpectedBytes,
	TSharedPtr<FJsonObject> PreparedMetadata, FWriteCallback Completion)
{
	check(IsInGameThread());
	const TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	GetAsset(AssetId, [WeakOwner, Version, ExpectedBytes, PreparedMetadata = MoveTemp(PreparedMetadata), Completion = MoveTemp(Completion)](FConvaiAvatarAsset Current, FString Error) mutable
	{
		const auto Self = WeakOwner.Pin(); if (!Self) return;
		FConvaiAvatarAssetWrite Write;
		if (!Error.IsEmpty() || !BuildWindowsMetadataCommit(Current, Version, ExpectedBytes, PreparedMetadata, Write, Error)) { Completion({}, MoveTemp(Error)); return; }
		Self->SendWriteRequest(Write, MoveTemp(Completion));
	});
}

bool FConvaiAvatarAssetsClient::BuildMetadataEdit(const FConvaiAvatarAsset& Current, const FConvaiAvatarMetadataEdit& Edit,
	FConvaiAvatarAssetWrite& OutWrite, FString& OutError)
{
	OutWrite = {};
	OutError.Reset();
	if (Current.AssetId.IsEmpty() || Current.EntityType != TEXT("avatar") || !Current.IsPrivate())
	{
		OutError = TEXT("Refresh your library and select one of your private avatars before editing its details.");
		return false;
	}
	FConvaiAvatarAssetWrite Write;
	Write.AssetId = Current.AssetId;
	Write.Name = Edit.Name.TrimStartAndEnd();
	Write.Gender = Edit.Gender;
	Write.ThumbnailPath = Edit.ThumbnailPath;
	Write.bMetadataOnly = true;
	Write.Tags = Current.Tags;
	Write.Metadata = Current.Metadata;
	// An omitted version is the Assets API metadata-only update contract. Never infer one from the record.
	if (!BuildWriteMetadata(Write, Write.Metadata, OutError)) return false;
	OutWrite = MoveTemp(Write);
	return true;
}

void FConvaiAvatarAssetsClient::UpdateMetadata(const FString& AssetId, const FConvaiAvatarMetadataEdit& Edit, FWriteCallback Completion)
{
	check(IsInGameThread());
	const TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	GetAsset(AssetId, [WeakOwner, Edit, Completion = MoveTemp(Completion)](FConvaiAvatarAsset Current, FString Error) mutable
	{
		const auto Self = WeakOwner.Pin();
		if (!Self) return;
		FConvaiAvatarAssetWrite Write;
		if (!Error.IsEmpty() || !BuildMetadataEdit(Current, Edit, Write, Error)) { Completion({}, MoveTemp(Error)); return; }
		Self->SendWriteRequest(Write, MoveTemp(Completion));
	});
}

void FConvaiAvatarAssetsClient::SendWriteRequest(const FConvaiAvatarAssetWrite& Write, FWriteCallback Completion)
{
	if (ApiKey.TrimStartAndEnd().IsEmpty() || AuthHeader.IsEmpty()) { Completion({}, TEXT("Sign in to Convai before publishing an avatar.")); return; }
	const FString Boundary = TEXT("ConvaiAvatarStudio") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<uint8> Body;
	FString Error;
	TSharedPtr<FJsonObject> Metadata;
	if (!BuildWriteMetadata(Write, Metadata, Error) || !BuildMultipart(Write, Boundary, Body, Error)) { Completion({}, MoveTemp(Error)); return; }
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(BaseUrl / (Write.AssetId.IsEmpty() ? TEXT("upload") : TEXT("update")), TEXT("POST"), true);
	Request->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=") + Boundary);
	Request->SetContent(MoveTemp(Body));
	Send(Request, [Write, Metadata, Completion = MoveTemp(Completion)](FHttpResponsePtr Response, FString RequestError)
	{
		FConvaiAvatarAssetWriteResult Result;
		TSharedPtr<FJsonObject> Root;
		if (RequestError.IsEmpty()) ReadJson(Response->GetContentAsString(), Root, RequestError);
		if (RequestError.IsEmpty())
		{
			const TSharedPtr<FJsonObject>* UploadUrls = nullptr;
			if (Write.AssetId.IsEmpty())
			{
				const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
				const TSharedPtr<FJsonObject>* Created = nullptr;
				if (!Root->TryGetArrayField(TEXT("assets"), Assets) || Assets->Num() != 1 || !(*Assets)[0]->TryGetObject(Created) || !ParseAsset(*Created, Result.Asset, RequestError))
				{
					if (RequestError.IsEmpty()) RequestError = TEXT("The avatar may have been created, but its response was incomplete. Refresh the library before retrying.");
				}
				else (*Created)->TryGetObjectField(TEXT("upload_urls"), UploadUrls);
			}
			else
			{
				Result.Asset.AssetId = Write.AssetId;
				Result.Asset.Name = Write.Name;
				Result.Asset.Gender = Write.Gender;
				Result.Asset.EntityType = TEXT("avatar");
				Result.Asset.Metadata = Metadata;
				Metadata->TryGetStringField(TEXT("avatar_id"), Result.Asset.AvatarId);
				Root->TryGetObjectField(TEXT("upload_urls"), UploadUrls);
			}
			if (UploadUrls) (*UploadUrls)->TryGetStringField(TEXT("avatar_asset"), Result.UploadUrl);
			if (RequestError.IsEmpty() && !Write.Version.IsEmpty() && !IsStorageUrl(Result.UploadUrl)) RequestError = TEXT("The avatar metadata was saved, but no valid upload link was returned. Refresh before retrying.");
		}
		Completion(MoveTemp(Result), MoveTemp(RequestError));
	});
}

void FConvaiAvatarAssetsClient::DeleteAsset(const FString& AssetId, FCompletion Completion)
{
	if (AssetId.IsEmpty()) { Completion(TEXT("Choose an avatar first.")); return; }
	const TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	GetAsset(AssetId, [WeakOwner, AssetId, Completion = MoveTemp(Completion)](FConvaiAvatarAsset, FString Error) mutable
	{
		const auto Self = WeakOwner.Pin();
		if (!Self) return;
		if (!Error.IsEmpty()) { Completion(MoveTemp(Error)); return; }
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_id"), AssetId);
		// Deliberately omit version: this removes the avatar and all artifact versions.
		Self->JsonRequest(TEXT("delete"), Payload, [WeakOwner, AssetId, Completion = MoveTemp(Completion)](TSharedPtr<FJsonObject>, FString DeleteError)
		{
			const auto Owner = WeakOwner.Pin();
			if (!Owner) return;
			Owner->LibraryState.RecordDeleteResult(AssetId, DeleteError);
			Completion(MoveTemp(DeleteError));
		});
	});
}

void FConvaiAvatarAssetsClient::UploadFile(const FString& SignedUrl, const FString& FilePath, FCompletion Completion, FProgress Progress, FString DependencyResolutionFile, FProgressDetails ProgressDetails)
{
	if (!IsStorageUrl(SignedUrl)) { Completion(TEXT("The upload link is not a supported Assets storage link. Refresh and try again.")); return; }
	const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
	if (FileSize <= 0 || FileSize > MaxAvatarBytes) { Completion(TEXT("Choose an existing, nonempty artifact no larger than 10,000 MiB.")); return; }
	StartFileTransfer(true, SignedUrl, FilePath, MoveTemp(Completion), MoveTemp(Progress), MoveTemp(DependencyResolutionFile), MoveTemp(ProgressDetails));
}

void FConvaiAvatarAssetsClient::DownloadFile(const FString& SignedUrl, const FString& FilePath, FCompletion Completion, FProgress Progress, FProgressDetails ProgressDetails)
{
	if (!IsStorageUrl(SignedUrl)) { Completion(TEXT("The download link is not a supported Assets storage link. Refresh and try again.")); return; }
	if (IFileManager::Get().FileExists(*FilePath)) { Completion(TEXT("A file already exists at the download destination. Choose a new destination.")); return; }
	StartFileTransfer(false, SignedUrl, FilePath, MoveTemp(Completion), MoveTemp(Progress), {}, MoveTemp(ProgressDetails));
}

void FConvaiAvatarAssetsClient::StartFileTransfer(bool bUpload, const FString& Url, const FString& Path, FCompletion Completion, FProgress Progress, FString DependencyResolutionFile, FProgressDetails ProgressDetails)
{
	check(IsInGameThread());
	if (!FileTransfer) FileTransfer = MakeShared<FConvaiAvatarFileTransfer, ESPMode::ThreadSafe>();
	const TWeakPtr<FConvaiAvatarAssetsClient> WeakOwner = AsShared();
	const uint64 Epoch = RequestEpoch;
	FileTransfer->Start(bUpload, Url, FPaths::ConvertRelativePathToFull(Path),
		[WeakOwner, Epoch, Completion = MoveTemp(Completion)](FString Error)
	{
		const TSharedPtr<FConvaiAvatarAssetsClient> Self = WeakOwner.Pin();
		if (Self && Self->RequestEpoch == Epoch) Completion(MoveTemp(Error));
	}, [WeakOwner, Epoch, Progress = MoveTemp(Progress)](float Fraction)
	{
		const TSharedPtr<FConvaiAvatarAssetsClient> Self = WeakOwner.Pin();
		if (Self && Self->RequestEpoch == Epoch && Progress) Progress(Fraction);
	}, MoveTemp(DependencyResolutionFile), [WeakOwner, Epoch, ProgressDetails = MoveTemp(ProgressDetails)](const FConvaiAvatarTransferProgress& Details)
	{
		const TSharedPtr<FConvaiAvatarAssetsClient> Self = WeakOwner.Pin();
		if (Self && Self->RequestEpoch == Epoch && ProgressDetails) ProgressDetails(Details);
	});
}

void FConvaiAvatarAssetsClient::CancelAll()
{
	++RequestEpoch;
	if (FileTransfer) FileTransfer->Cancel();
	const auto Pending = MoveTemp(Requests);
	for (const auto& Request : Pending)
	{
		ConvaiEditorHttpCompat::UnbindProgress(*Request);
		Request->CancelRequest();
	}
}
