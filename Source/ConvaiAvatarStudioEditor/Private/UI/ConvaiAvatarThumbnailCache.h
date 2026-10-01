// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HttpFwd.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;

/** Small view-owned thumbnail cache; no account credentials are sent with image requests. */
class FConvaiAvatarThumbnailCache : public TSharedFromThis<FConvaiAvatarThumbnailCache>
{
public:
	/** A separate preview cache can retain 2048px portraits without enlarging library textures. */
	explicit FConvaiAvatarThumbnailCache(int32 InMaxTextureDimension = 512)
		: MaxTextureDimension(FMath::Clamp(InMaxTextureDimension, 1, 2048)) {}
	~FConvaiAvatarThumbnailCache();
	const FSlateBrush* GetOrRequest(const FString& Url, bool bSquarePortrait = false);
	/** Native picker previews and the just-created card before its cloud URL is refreshed. */
	const FSlateBrush* GetLocal(const FString& Path, bool bSquarePortrait = false);
	void Invalidate(const FString& Url);
	void RetryFailed() { Failed.Reset(); }
	bool IsLoading(const FString& Url) const { return Pending.Contains(Url); }
	bool HasRemoteImage(const FString& Url) const;
	bool RemoteImageMatches(const FString& Url, const FString& ExpectedMD5) const;

private:
	const int32 MaxTextureDimension;
	struct FEntry
	{
		TStrongObjectPtr<UTexture2D> Texture;
		TSharedPtr<FSlateBrush> Brush;
		TStrongObjectPtr<UTexture2D> SquareTexture;
		TSharedPtr<FSlateBrush> SquareBrush;
		FString EncodedMD5;
		FString LocalRevision;
		const FSlateBrush* GetBrush(bool bSquarePortrait) const { return bSquarePortrait && SquareTexture.IsValid() ? SquareBrush.Get() : Brush.Get(); }
	};
	/** Keep brush addresses stable while old render resources finish their queued work. */
	static void RetireResources(FEntry& Entry);
	void Complete(const FString& Url, const FHttpRequestPtr& Request, const FHttpResponsePtr& Response, bool bSuccess);
	bool StoreImage(const FString& Key, const TArray<uint8>& Compressed);
	TMap<FString, FEntry> Entries;
	TMap<FString, FHttpRequestPtr> Pending;
	TSet<FString> Failed;
};
