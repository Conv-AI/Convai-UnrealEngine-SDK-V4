// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HttpFwd.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;

/** Small view-owned thumbnail cache; no account credentials are sent with image requests. Game thread only. */
class FConvaiAvatarThumbnailCache : public TSharedFromThis<FConvaiAvatarThumbnailCache>
{
public:
	/** A separate preview cache can retain 2048px portraits without enlarging library textures. */
	explicit FConvaiAvatarThumbnailCache(int32 InMaxTextureDimension = 512);
	~FConvaiAvatarThumbnailCache();
	const FSlateBrush* GetOrRequest(const FString& Url, bool bSquarePortrait = false);
	/** Native picker previews and the just-created card before its cloud URL is refreshed. Null until the file is decoded. */
	const FSlateBrush* GetLocal(const FString& Path, bool bSquarePortrait = false);
	/** Decodes on the spot, for a window that needs its brush before it opens. */
	const FSlateBrush* GetLocalNow(const FString& Path, bool bSquarePortrait = false);
	/** The next GetLocal of Path reads its size and time again, as after a capture overwrote it. */
	void RecheckLocal(const FString& Path) { LocalFiles.Remove(Path); }
	void Invalidate(const FString& Url);
	void RetryFailed() { Failed.Reset(); }
	bool IsLoading(const FString& Url) const { return Pending.Contains(Url); }
	bool HasRemoteImage(const FString& Url) const;
	bool RemoteImageMatches(const FString& Url, const FString& ExpectedMD5) const;
	/** Runs once on a later frame after images change. Brushes change in place, so the widgets holding them cannot see it. */
	FSimpleDelegate OnImagesChanged;
#if WITH_DEV_AUTOMATION_TESTS
	/** Starts a download attempt under Key without HTTP; returns the token its completion carries. */
	uint64 AutomationBeginDownload(const FString& Key);
	/** Feeds downloaded bytes through the remote completion path, as attempt Token's download finishing. */
	void AutomationComplete(const FString& Key, uint64 Token, TArray<uint8> Compressed);
	/** Starts an attempt under Key and feeds it the downloaded bytes at once. */
	void AutomationComplete(const FString& Key, TArray<uint8> Compressed);
	/** Waits for in-flight decodes and applies their results on the calling game thread. False on timeout. */
	bool AutomationWaitForDecodes(double TimeoutSeconds = 10.0);
	/** Runs the change notification due on the next frame now. False when none is due. */
	bool AutomationFlushChanges();
	/** Pixel steps whose result reached the cache, by the thread they ran on. */
	int32 AutomationGameThreadDecodes = 0;
	int32 AutomationWorkerDecodes = 0;
	/** Local file size and time reads. */
	int32 AutomationFileQueries = 0;
#endif

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
	/** A download or decode in flight. It stays pending until its result is applied, so the request cap covers decodes. */
	struct FPendingImage
	{
		/** Null once the bytes are decoding. */
		FHttpRequestPtr Request;
		/** A result whose token no longer matches this attempt is dropped. */
		uint64 Token = 0;
		FString LocalRevision;
	};
	/** BGRA pixels ready for a texture, produced off the game thread. */
	struct FDecodedImage
	{
		TArray<uint8> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		/** Set only for a tall transparent portrait's upper-body square. */
		TArray<uint8> SquarePixels;
		int32 SquareSize = 0;
		FString EncodedMD5;
#if WITH_DEV_AUTOMATION_TESTS
		bool bDecodedOnGameThread = false;
#endif
	};
	/** A local file as GetLocal last read it, by the path its caller passed. */
	struct FLocalFile
	{
		FString Key;
		FString Revision;
		int64 Size = 0;
		double ReadAt = 0.0;
	};
	/** The pixel step: decode, downscale, square crop and hash. Touches no cache state, so any thread may run it. */
	static FDecodedImage DecodeImage(const TArray<uint8>& Compressed, int32 MaxTextureDimension);
	/** Keep brush addresses stable while old render resources finish their queued work. */
	static void RetireResources(FEntry& Entry);
	/** Null when the file is missing, empty or too large. Paint-time getters ask several times a frame, so the file is read at most once a second. */
	const FLocalFile* StatLocal(const FString& Path);
	void Complete(const FString& Url, uint64 Token, TArray<uint8> Compressed);
	void Launch(const FString& Key, uint64 Token, TUniqueFunction<FDecodedImage()> PixelStep);
	void Apply(const FString& Key, uint64 Token, FDecodedImage Image);
	bool StoreImage(const FString& Key, const FDecodedImage& Image);
	void Fail(const FString& Key, const FString& LocalRevision);
	void NotifyChanged();
	TMap<FString, FEntry> Entries;
	TMap<FString, FPendingImage> Pending;
	TSet<FString> Failed;
	TMap<FString, FLocalFile> LocalFiles;
	uint64 NextToken = 0;
	FTSTicker::FDelegateHandle ChangeTicker;
#if WITH_DEV_AUTOMATION_TESTS
	int32 AutomationDecodesInFlight = 0;
#endif
};
