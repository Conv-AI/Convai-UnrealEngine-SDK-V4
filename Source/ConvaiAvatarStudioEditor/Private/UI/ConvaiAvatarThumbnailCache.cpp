// Copyright Convai Inc. All Rights Reserved.
#include "UI/ConvaiAvatarThumbnailCache.h"
#include "Utility/ConvaiEditorHttpCompat.h"
#include "Services/ConvaiAvatarAssetsClient.h"

#include "Async/Async.h"
#include "Engine/Texture2D.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
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
	IImageWrapperModule& Images = FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
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
	// The cache's constructor loaded FreeImage on the game thread; its loader asserts on any other thread.
	if (FUEFreeImageWrapper::IsValid() && Compressed.Num() >= 12 && FMemory::Memcmp(Compressed.GetData(), "RIFF", 4) == 0 && FMemory::Memcmp(Compressed.GetData() + 8, "WEBP", 4) == 0)
	{
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

/** Alpha-weighted area filter down to MaxTextureDimension on the longer side; smaller images pass through. */
TArray<uint8> FitThumbnail(TArray<uint8> Pixels, int32& Width, int32& Height, int32 MaxTextureDimension)
{
	if (Width <= MaxTextureDimension && Height <= MaxTextureDimension) return Pixels;
	const float Scale = static_cast<float>(MaxTextureDimension) / FMath::Max(Width, Height);
	const int32 SmallWidth = FMath::Max(1, FMath::RoundToInt(Width * Scale));
	const int32 SmallHeight = FMath::Max(1, FMath::RoundToInt(Height * Scale));
	TArray<uint8> SmallPixels;
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
					const uint8* Source = Pixels.GetData() + (SourceY * Width + SourceX) * 4;
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
	Width = SmallWidth;
	Height = SmallHeight;
	return SmallPixels;
}

bool MakeThumbnailTexture(const TArray<uint8>& Pixels, int32 Width, int32 Height, TStrongObjectPtr<UTexture2D>& OutTexture, TSharedPtr<FSlateBrush>& OutBrush)
{
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.IsEmpty()) return false;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num());
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

FConvaiAvatarThumbnailCache::FConvaiAvatarThumbnailCache(int32 InMaxTextureDimension)
	: MaxTextureDimension(FMath::Clamp(InMaxTextureDimension, 1, 2048))
{
	// Both loaders must first run on the game thread; decodes then run on workers.
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
#if WITH_FREEIMAGE_LIB
	FUEFreeImageWrapper::FreeImage_Initialise();
#endif
}

FConvaiAvatarThumbnailCache::~FConvaiAvatarThumbnailCache()
{
	if (ChangeTicker.IsValid()) FTSTicker::RemoveTicker(ChangeTicker);
	for (auto& Pair : Pending)
	{
		if (!Pair.Value.Request) continue;
		Pair.Value.Request->OnProcessRequestComplete().Unbind();
		Pair.Value.Request->CancelRequest();
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
	const uint64 Token = ++NextToken;
	Request->OnProcessRequestComplete().BindLambda([WeakSelf, Url, Token](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
	{
		TArray<uint8> Compressed;
		if (bSuccess && Response.IsValid() && Response->GetResponseCode() == 200) Compressed = Response->GetContent();
		auto Finish = [WeakSelf, Url, Token, Compressed = MoveTemp(Compressed)]() mutable
		{
			if (const auto Self = WeakSelf.Pin()) { Self->Complete(Url, Token, MoveTemp(Compressed)); }
		};
		if (IsInGameThread()) { Finish(); } else { AsyncTask(ENamedThreads::GameThread, MoveTemp(Finish)); }
	});
	Pending.Add(Url, FPendingImage{ Request, Token });
	if (!Request->ProcessRequest()) { Pending.Remove(Url); Failed.Add(Url); }
	return nullptr;
}

const FConvaiAvatarThumbnailCache::FLocalFile* FConvaiAvatarThumbnailCache::StatLocal(const FString& Path)
{
	if (Path.IsEmpty()) return nullptr;
	FLocalFile& File = LocalFiles.FindOrAdd(Path);
	const double Now = FPlatformTime::Seconds();
	if (File.Key.IsEmpty() || Now - File.ReadAt >= 1.0)
	{
		if (File.Key.IsEmpty()) File.Key = TEXT("local:") + FPaths::ConvertRelativePathToFull(Path) + FString::Printf(TEXT(":edge%d"), MaxTextureDimension);
		File.ReadAt = Now;
#if WITH_DEV_AUTOMATION_TESTS
		++AutomationFileQueries;
#endif
		File.Size = IFileManager::Get().FileSize(*Path);
		File.Revision = File.Size > 0 ? FString::Printf(TEXT("%lld:%lld"), File.Size, IFileManager::Get().GetTimeStamp(*Path).GetTicks()) : FString();
	}
	return File.Size > 0 && File.Size <= MaxImageBytes ? &File : nullptr;
}

const FSlateBrush* FConvaiAvatarThumbnailCache::GetLocal(const FString& Path, bool bSquarePortrait)
{
	const FLocalFile* File = StatLocal(Path);
	if (!File) return nullptr;
	const FEntry* Entry = Entries.Find(File->Key);
	// A replaced file keeps showing its previous image until the new one is decoded.
	const FSlateBrush* Current = Entry && !Entry->EncodedMD5.IsEmpty() ? Entry->GetBrush(bSquarePortrait) : nullptr;
	if (Entry && Entry->LocalRevision == File->Revision && (Current || Failed.Contains(File->Key))) return Current;
	const FPendingImage* Job = Pending.Find(File->Key);
	if (Job ? Job->LocalRevision == File->Revision : Pending.Num() >= MaxConcurrentRequests) return Current;
	Failed.Remove(File->Key);
	const uint64 Token = ++NextToken;
	Pending.Add(File->Key, FPendingImage{ nullptr, Token, File->Revision });
	Launch(File->Key, Token, [Path, Size = File->Size, Edge = MaxTextureDimension]
	{
		TArray<uint8> Compressed;
		return FFileHelper::LoadFileToArray(Compressed, *Path) && Compressed.Num() == Size ? DecodeImage(Compressed, Edge) : FDecodedImage();
	});
	return Current;
}

const FSlateBrush* FConvaiAvatarThumbnailCache::GetLocalNow(const FString& Path, bool bSquarePortrait)
{
	const FLocalFile* File = StatLocal(Path);
	if (!File) return nullptr;
	if (const FEntry* Entry = Entries.Find(File->Key); Entry && Entry->LocalRevision == File->Revision && !Entry->EncodedMD5.IsEmpty()) return Entry->GetBrush(bSquarePortrait);
	Pending.Remove(File->Key); // This result supersedes a decode still in flight.
	TArray<uint8> Compressed;
	if (!FFileHelper::LoadFileToArray(Compressed, *Path) || Compressed.Num() != File->Size || !StoreImage(File->Key, DecodeImage(Compressed, MaxTextureDimension)))
	{
		Fail(File->Key, File->Revision);
		return nullptr;
	}
	Failed.Remove(File->Key);
	FEntry& Stored = Entries.FindChecked(File->Key);
	Stored.LocalRevision = File->Revision;
	return Stored.GetBrush(bSquarePortrait);
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
	if (const FPendingImage* Job = Pending.Find(Url))
	{
		if (Job->Request) { Job->Request->OnProcessRequestComplete().Unbind(); Job->Request->CancelRequest(); }
		Pending.Remove(Url); // A decode still in flight now holds a stale token.
	}
	Failed.Remove(Url);
	NotifyChanged();
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

void FConvaiAvatarThumbnailCache::Complete(const FString& Url, uint64 Token, TArray<uint8> Compressed)
{
	FPendingImage* Job = Pending.Find(Url);
	if (!Job || Job->Token != Token) return; // An invalidated request must not replace a newer image.
	if (Compressed.IsEmpty() || Compressed.Num() > MaxImageBytes)
	{
		Pending.Remove(Url);
		Fail(Url, FString());
		return;
	}
	Job->Request.Reset();
	Launch(Url, Token, [Compressed = MoveTemp(Compressed), Edge = MaxTextureDimension] { return DecodeImage(Compressed, Edge); });
}

void FConvaiAvatarThumbnailCache::Launch(const FString& Key, uint64 Token, TUniqueFunction<FDecodedImage()> PixelStep)
{
#if WITH_DEV_AUTOMATION_TESTS
	++AutomationDecodesInFlight;
#endif
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSelf = TWeakPtr<FConvaiAvatarThumbnailCache>(AsShared()), Key, Token, PixelStep = MoveTemp(PixelStep)]() mutable
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Key, Token, Image = PixelStep()]() mutable
		{
			if (const auto Self = WeakSelf.Pin()) { Self->Apply(Key, Token, MoveTemp(Image)); }
		});
	});
}

void FConvaiAvatarThumbnailCache::Apply(const FString& Key, uint64 Token, FDecodedImage Image)
{
#if WITH_DEV_AUTOMATION_TESTS
	--AutomationDecodesInFlight;
	++(Image.bDecodedOnGameThread ? AutomationGameThreadDecodes : AutomationWorkerDecodes);
#endif
	const FPendingImage* Job = Pending.Find(Key);
	if (!Job || Job->Token != Token) return; // An invalidated request must not replace a newer image.
	const FString LocalRevision = Job->LocalRevision;
	Pending.Remove(Key);
	if (!StoreImage(Key, Image)) { Fail(Key, LocalRevision); return; }
	Entries.FindChecked(Key).LocalRevision = LocalRevision;
}

FConvaiAvatarThumbnailCache::FDecodedImage FConvaiAvatarThumbnailCache::DecodeImage(const TArray<uint8>& Compressed, int32 MaxTextureDimension)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FConvaiAvatarThumbnailCache::DecodeImage);
	FDecodedImage Image;
#if WITH_DEV_AUTOMATION_TESTS
	Image.bDecodedOnGameThread = IsInGameThread();
#endif
	int32 Width = 0, Height = 0;
	TArray<uint8> Pixels;
	if (!DecodeThumbnail(Compressed, Width, Height, Pixels) || Pixels.Num() != static_cast<int64>(Width) * Height * 4) return Image;
	// Decode is bounded at 4096 per side. Library textures remain <=512; only the
	// separately owned full-portrait window requests the native 2048-pixel edge.
	if (Height > Width && MaxTextureDimension <= 512)
	{
		int32 FirstAlphaRow = INDEX_NONE;
		bool bHasTransparency = false;
		for (int32 Pixel = 0; Pixel < Width * Height && !(bHasTransparency && FirstAlphaRow != INDEX_NONE); ++Pixel)
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
			int32 SquareWidth = Width, SquareHeight = Width;
			Image.SquarePixels = FitThumbnail(MoveTemp(SquarePixels), SquareWidth, SquareHeight, MaxTextureDimension);
			Image.SquareSize = SquareWidth;
		}
	}
	Image.Pixels = FitThumbnail(MoveTemp(Pixels), Width, Height, MaxTextureDimension);
	Image.Width = Width;
	Image.Height = Height;
	FMD5 Hash; Hash.Update(Compressed.GetData(), Compressed.Num()); uint8 Digest[16]; Hash.Final(Digest);
	Image.EncodedMD5 = BytesToHex(Digest, 16);
	return Image;
}

bool FConvaiAvatarThumbnailCache::StoreImage(const FString& Key, const FDecodedImage& Image)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FConvaiAvatarThumbnailCache::StoreImage);
	FEntry Entry;
	if (Image.Pixels.IsEmpty() || !MakeThumbnailTexture(Image.Pixels, Image.Width, Image.Height, Entry.Texture, Entry.Brush)) return false;
	if (!Image.SquarePixels.IsEmpty() && !MakeThumbnailTexture(Image.SquarePixels, Image.SquareSize, Image.SquareSize, Entry.SquareTexture, Entry.SquareBrush)) return false;
	Entry.EncodedMD5 = Image.EncodedMD5;
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
	NotifyChanged();
	return true;
}

void FConvaiAvatarThumbnailCache::Fail(const FString& Key, const FString& LocalRevision)
{
	if (!LocalRevision.IsEmpty())
	{
		FEntry& Entry = Entries.FindOrAdd(Key);
		RetireResources(Entry);
		Entry.LocalRevision = LocalRevision;
	}
	Failed.Add(Key);
	NotifyChanged();
}

void FConvaiAvatarThumbnailCache::NotifyChanged()
{
	if (ChangeTicker.IsValid()) return;
	ChangeTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSPLambda(AsShared(), [this](float)
	{
		ChangeTicker.Reset();
		OnImagesChanged.ExecuteIfBound();
		return false;
	}));
}

#if WITH_DEV_AUTOMATION_TESTS
uint64 FConvaiAvatarThumbnailCache::AutomationBeginDownload(const FString& Key)
{
	const uint64 Token = ++NextToken;
	Pending.Add(Key, FPendingImage{ nullptr, Token });
	return Token;
}

void FConvaiAvatarThumbnailCache::AutomationComplete(const FString& Key, uint64 Token, TArray<uint8> Compressed)
{
	Complete(Key, Token, MoveTemp(Compressed));
}

void FConvaiAvatarThumbnailCache::AutomationComplete(const FString& Key, TArray<uint8> Compressed)
{
	const uint64 Token = AutomationBeginDownload(Key);
	Complete(Key, Token, MoveTemp(Compressed));
}

bool FConvaiAvatarThumbnailCache::AutomationWaitForDecodes(double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (AutomationDecodesInFlight > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	return AutomationDecodesInFlight == 0;
}

bool FConvaiAvatarThumbnailCache::AutomationFlushChanges()
{
	if (!ChangeTicker.IsValid()) return false;
	FTSTicker::RemoveTicker(ChangeTicker);
	ChangeTicker.Reset();
	OnImagesChanged.ExecuteIfBound();
	return true;
}
#endif
