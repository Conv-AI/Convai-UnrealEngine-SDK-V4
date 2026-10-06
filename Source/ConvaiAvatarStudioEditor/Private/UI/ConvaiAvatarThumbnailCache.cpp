// Copyright Convai Inc. All Rights Reserved.
#include "UI/ConvaiAvatarThumbnailCache.h"
#include "Utility/ConvaiEditorHttpCompat.h"
#include "Services/ConvaiAvatarAssetsClient.h"

#include "Async/Async.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Slate/DeferredCleanupSlateBrush.h"

// FreeImage.h only supplies TRUE/FALSE when <windows.h> has not been seen. A unity build
// puts an earlier file's HideWindowsPlatformTypes.h ahead of this include, which leaves
// _WINDOWS_ defined with TRUE/FALSE undefined, so restore them around the include.
#include "Windows/AllowWindowsPlatformTypes.h"
#include "UEFreeImage.h"
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
constexpr int32 MaxImageBytes = 10 * 1024 * 1024;
constexpr int32 MaxImageDimension = 4096;
constexpr int32 MaxConcurrentRequests = 4;

bool DecodeThumbnail(const TArray<uint8>& Compressed, int32& Width, int32& Height, TArray<uint8>& Pixels)
{
	IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = Images.DetectImageFormat(Compressed.GetData(), Compressed.Num());
	if (Format != EImageFormat::Invalid)
	{
		TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(Format);
		if (Wrapper.IsValid() && Wrapper->SetCompressed(Compressed.GetData(), Compressed.Num()))
		{
			// Validate dimensions before allocating the uncompressed image.
			if (Wrapper->GetWidth() <= 0 || Wrapper->GetHeight() <= 0 || Wrapper->GetWidth() > MaxImageDimension || Wrapper->GetHeight() > MaxImageDimension) { return false; }
			Width = static_cast<int32>(Wrapper->GetWidth());
			Height = static_cast<int32>(Wrapper->GetHeight());
			if (Wrapper->GetRaw(ERGBFormat::BGRA, 8, Pixels)) { return true; }
		}
	}
#if WITH_FREEIMAGE_LIB
	if (Compressed.Num() >= 12 && FMemory::Memcmp(Compressed.GetData(), "RIFF", 4) == 0 && FMemory::Memcmp(Compressed.GetData() + 8, "WEBP", 4) == 0)
	{
		FUEFreeImageWrapper::FreeImage_Initialise();
		FIMEMORY* Memory = FreeImage_OpenMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(Compressed.GetData())), Compressed.Num());
		FIBITMAP* Source = Memory ? FreeImage_LoadFromMemory(FIF_WEBP, Memory) : nullptr;
		FIBITMAP* Bitmap = Source ? FreeImage_ConvertTo32Bits(Source) : nullptr;
		bool bDecoded = false;
		if (Bitmap)
		{
			Width = static_cast<int32>(FreeImage_GetWidth(Bitmap));
			Height = static_cast<int32>(FreeImage_GetHeight(Bitmap));
			if (Width > 0 && Height > 0 && Width <= MaxImageDimension && Height <= MaxImageDimension)
			{
				Pixels.SetNumUninitialized(Width * Height * 4);
				for (int32 Y = 0; Y < Height; ++Y)
				{
					FMemory::Memcpy(Pixels.GetData() + Y * Width * 4, FreeImage_GetScanLine(Bitmap, Height - Y - 1), Width * 4);
				}
				bDecoded = true;
			}
			FreeImage_Unload(Bitmap);
		}
		if (Source) { FreeImage_Unload(Source); }
		if (Memory) { FreeImage_CloseMemory(Memory); }
		return bDecoded;
	}
#endif
	return false;
}
}

FConvaiAvatarThumbnailCache::~FConvaiAvatarThumbnailCache()
{
	for (auto& Pair : Pending)
	{
		Pair.Value->OnProcessRequestComplete().Unbind();
		Pair.Value->CancelRequest();
	}
	for (auto& Pair : Entries) RetireResources(Pair.Value);
}

const FSlateBrush* FConvaiAvatarThumbnailCache::GetOrRequest(const FString& Url, bool bSquarePortrait)
{
	if (const FEntry* Entry = Entries.Find(Url); Entry && !Entry->EncodedMD5.IsEmpty()) { return Entry->GetBrush(bSquarePortrait); }
	if (!FConvaiAvatarAssetsClient::IsThumbnailUrl(Url) || Pending.Contains(Url) || Failed.Contains(Url) || Pending.Num() >= MaxConcurrentRequests) { return nullptr; }
	const auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("UnrealEngine/ConvaiAvatarStudio"));
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-cache"));
	Request->SetTimeout(20.f);
	ConvaiEditorHttpCompat::BindProgress(*Request, [](FHttpRequestPtr Active, uint64, uint64 Received)
	{
		if (Received > MaxImageBytes) Active->CancelRequest();
	});
	const TWeakPtr<FConvaiAvatarThumbnailCache> WeakSelf = AsShared();
	Request->OnProcessRequestComplete().BindLambda([WeakSelf, Url](FHttpRequestPtr Active, FHttpResponsePtr Response, bool bSuccess)
	{
		auto Finish = [WeakSelf, Url, Active, Response, bSuccess]
		{
			if (const auto Self = WeakSelf.Pin()) { Self->Complete(Url, Active, Response, bSuccess); }
		};
		if (IsInGameThread()) { Finish(); } else { AsyncTask(ENamedThreads::GameThread, MoveTemp(Finish)); }
	});
	Pending.Add(Url, Request);
	if (!Request->ProcessRequest()) { Pending.Remove(Url); Failed.Add(Url); }
	return nullptr;
}

const FSlateBrush* FConvaiAvatarThumbnailCache::GetLocal(const FString& Path, bool bSquarePortrait)
{
	if (Path.IsEmpty()) return nullptr;
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size <= 0 || Size > MaxImageBytes) return nullptr;
	const FString Key = TEXT("local:") + FPaths::ConvertRelativePathToFull(Path) + FString::Printf(TEXT(":edge%d"), MaxTextureDimension);
	const FString Revision = FString::Printf(TEXT("%lld:%lld"), Size, IFileManager::Get().GetTimeStamp(*Path).GetTicks());
	if (FEntry* Entry = Entries.Find(Key))
	{
		if (Entry->LocalRevision == Revision && !Entry->EncodedMD5.IsEmpty()) return Entry->GetBrush(bSquarePortrait);
		if (Entry->LocalRevision != Revision) { RetireResources(*Entry); Failed.Remove(Key); }
	}
	if (Failed.Contains(Key)) return nullptr;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() != Size || !StoreImage(Key, Bytes))
	{
		Entries.FindOrAdd(Key).LocalRevision = Revision;
		Failed.Add(Key);
		if (FSlateApplication::IsInitialized()) FSlateApplication::Get().InvalidateAllWidgets(false);
		return nullptr;
	}
	Entries.FindChecked(Key).LocalRevision = Revision;
	return Entries.FindChecked(Key).GetBrush(bSquarePortrait);
}

void FConvaiAvatarThumbnailCache::RetireResources(FEntry& Entry)
{
	// SImage attributes can retain raw brush pointers. Reuse those brush objects,
	// and let the engine release only their former texture resources after rendering.
	for (const TSharedPtr<FSlateBrush>& Brush : { Entry.Brush, Entry.SquareBrush })
	{
		if (!Brush) continue;
		if (Brush->GetResourceObject()) { FDeferredCleanupSlateBrush::CreateBrush(*Brush); }
		Brush->SetResourceObject(nullptr);
		Brush->DrawAs = ESlateBrushDrawType::NoDrawType;
	}
	Entry.Texture.Reset(); Entry.SquareTexture.Reset(); Entry.EncodedMD5.Reset();
}

void FConvaiAvatarThumbnailCache::Invalidate(const FString& Url)
{
	if (Url.IsEmpty()) return;
	if (FEntry* Entry = Entries.Find(Url)) RetireResources(*Entry);
	if (FHttpRequestPtr* Request = Pending.Find(Url)) { (*Request)->OnProcessRequestComplete().Unbind(); (*Request)->CancelRequest(); Pending.Remove(Url); }
	Failed.Remove(Url);
	if (FSlateApplication::IsInitialized()) FSlateApplication::Get().InvalidateAllWidgets(false);
}

bool FConvaiAvatarThumbnailCache::HasRemoteImage(const FString& Url) const
{
	const FEntry* Entry = Entries.Find(Url);
	return Url.StartsWith(TEXT("https://")) && Entry && !Entry->EncodedMD5.IsEmpty();
}

bool FConvaiAvatarThumbnailCache::RemoteImageMatches(const FString& Url, const FString& ExpectedMD5) const
{
	const FEntry* Entry = Entries.Find(Url);
	return Url.StartsWith(TEXT("https://")) && Entry && ExpectedMD5.Len() == 32 && Entry->EncodedMD5.Equals(ExpectedMD5, ESearchCase::IgnoreCase);
}

void FConvaiAvatarThumbnailCache::Complete(const FString& Url, const FHttpRequestPtr& Request, const FHttpResponsePtr& Response, bool bSuccess)
{
	const FHttpRequestPtr* Current = Pending.Find(Url);
	if (!Current || *Current != Request) return; // An invalidated request must not replace a newer image.
	Pending.Remove(Url);
	if (!bSuccess || !Response.IsValid() || Response->GetResponseCode() != 200 || Response->GetContent().IsEmpty() || Response->GetContent().Num() > MaxImageBytes)
	{
		Failed.Add(Url);
		if (FSlateApplication::IsInitialized()) FSlateApplication::Get().InvalidateAllWidgets(false);
		return;
	}
	if (!StoreImage(Url, Response->GetContent()))
	{
		Failed.Add(Url);
		if (FSlateApplication::IsInitialized()) FSlateApplication::Get().InvalidateAllWidgets(false);
	}
}

namespace
{
bool MakeThumbnailTexture(const TArray<uint8>& InputPixels, int32 Width, int32 Height, int32 MaxTextureDimension,
	TStrongObjectPtr<UTexture2D>& OutTexture, TSharedPtr<FSlateBrush>& OutBrush)
{
	const TArray<uint8>* TexturePixels = &InputPixels;
	TArray<uint8> SmallPixels;
	if (Width > MaxTextureDimension || Height > MaxTextureDimension)
	{
		const float Scale = static_cast<float>(MaxTextureDimension) / FMath::Max(Width, Height);
		const int32 SmallWidth = FMath::Max(1, FMath::RoundToInt(Width * Scale));
		const int32 SmallHeight = FMath::Max(1, FMath::RoundToInt(Height * Scale));
		SmallPixels.SetNumUninitialized(SmallWidth * SmallHeight * 4);
		for (int32 Y = 0; Y < SmallHeight; ++Y)
		{
			for (int32 X = 0; X < SmallWidth; ++X)
			{
				const double Left = double(X) * Width / SmallWidth, Right = double(X + 1) * Width / SmallWidth;
				const double Top = double(Y) * Height / SmallHeight, Bottom = double(Y + 1) * Height / SmallHeight;
				double Alpha = 0, Channels[3] = { 0, 0, 0 };
				for (int32 SourceY = FMath::FloorToInt(Top); SourceY < FMath::CeilToInt(Bottom); ++SourceY)
				{
					for (int32 SourceX = FMath::FloorToInt(Left); SourceX < FMath::CeilToInt(Right); ++SourceX)
					{
						const double Weight = (FMath::Min(Right, double(SourceX + 1)) - FMath::Max(Left, double(SourceX))) *
							(FMath::Min(Bottom, double(SourceY + 1)) - FMath::Max(Top, double(SourceY)));
						const uint8* Source = InputPixels.GetData() + (SourceY * Width + SourceX) * 4;
						const double WeightedAlpha = Source[3] * Weight;
						Alpha += WeightedAlpha;
						for (int32 Channel = 0; Channel < 3; ++Channel) Channels[Channel] += Source[Channel] * WeightedAlpha;
					}
				}
				uint8* Dest = SmallPixels.GetData() + (Y * SmallWidth + X) * 4;
				for (int32 Channel = 0; Channel < 3; ++Channel) Dest[Channel] = Alpha > 0 ? uint8(FMath::Clamp(FMath::RoundToInt(Channels[Channel] / Alpha), 0, 255)) : 0;
				Dest[3] = uint8(FMath::Clamp(FMath::RoundToInt(Alpha / ((Right - Left) * (Bottom - Top))), 0, 255));
			}
		}
		TexturePixels = &SmallPixels;
		Width = SmallWidth;
		Height = SmallHeight;
	}
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.IsEmpty()) return false;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, TexturePixels->GetData(), TexturePixels->Num());
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
	OutTexture = TStrongObjectPtr<UTexture2D>(Texture);
	OutBrush = MakeShared<FSlateBrush>();
	OutBrush->SetResourceObject(Texture);
	OutBrush->ImageSize = FVector2D(Width, Height);
	OutBrush->DrawAs = ESlateBrushDrawType::Image;
	return true;
}
}

bool FConvaiAvatarThumbnailCache::StoreImage(const FString& Key, const TArray<uint8>& Compressed)
{
	int32 Width = 0, Height = 0;
	TArray<uint8> Pixels;
	if (!DecodeThumbnail(Compressed, Width, Height, Pixels) || Pixels.Num() != static_cast<int64>(Width) * Height * 4) return false;
	// Decode is bounded at 4096 per side. Library textures remain <=512; only the
	// separately owned full-portrait window requests the native 2048-pixel edge.
	FEntry Entry;
	if (!MakeThumbnailTexture(Pixels, Width, Height, MaxTextureDimension, Entry.Texture, Entry.Brush)) return false;
	if (Height > Width && MaxTextureDimension <= 512)
	{
		int32 FirstAlphaRow = INDEX_NONE;
		bool bHasTransparency = false;
		for (int32 Pixel = 0; Pixel < Width * Height; ++Pixel)
		{
			if (Pixels[Pixel * 4 + 3] == 0) bHasTransparency = true;
			else if (FirstAlphaRow == INDEX_NONE) FirstAlphaRow = Pixel / Width;
		}
		if (bHasTransparency && FirstAlphaRow != INDEX_NONE)
		{
			// Legacy full-body PNGs use the same headroom proportion as the backend's
			// 100 pixels at 1024px width. Clamp the bottom so a late subject stays whole.
			const int32 Headroom = FMath::Max(1, FMath::RoundToInt(Width * (100.f / 1024.f)));
			const int32 Top = FMath::Clamp(FirstAlphaRow - Headroom, 0, Height - Width);
			TArray<uint8> SquarePixels;
			SquarePixels.Append(Pixels.GetData() + Top * Width * 4, Width * Width * 4);
			if (!MakeThumbnailTexture(SquarePixels, Width, Width, MaxTextureDimension, Entry.SquareTexture, Entry.SquareBrush)) return false;
		}
	}
	FMD5 Hash; Hash.Update(Compressed.GetData(), Compressed.Num()); uint8 Digest[16]; Hash.Final(Digest);
	Entry.EncodedMD5 = BytesToHex(Digest, 16);
	if (FEntry* Previous = Entries.Find(Key))
	{
		RetireResources(*Previous);
		if (Previous->Brush) { *Previous->Brush = *Entry.Brush; Entry.Brush = Previous->Brush; }
		if (Previous->SquareBrush)
		{
			if (Entry.SquareBrush) *Previous->SquareBrush = *Entry.SquareBrush;
			Entry.SquareBrush = Previous->SquareBrush;
		}
	}
	Entries.Add(Key, MoveTemp(Entry));
	if (FSlateApplication::IsInitialized()) { FSlateApplication::Get().InvalidateAllWidgets(false); }
	return true;
}
