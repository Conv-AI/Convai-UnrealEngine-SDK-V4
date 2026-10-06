

#include "ConvaiVisionBaseUtils.h"
#include "Engine/Texture2D.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "ImageUtils.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderUtils.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"
#include "Logging/MessageLog.h"
#include "RenderingThread.h"
#include "RenderTargetPool.h"
#include "Engine/Texture2DDynamic.h"
#include "EngineLogs.h"

DEFINE_LOG_CATEGORY(ConvaiVisionBaseUtilsLog);

bool UConvaiVisionBaseUtils::ConvertCompressedDataToTexture2D(const TArray<uint8>& CompressedData, UTexture2D*& Texture)
{
    // Cache the ImageWrapperModule to avoid loading it every time
    static IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    EImageFormat DetectedFormat = ImageWrapperModule.DetectImageFormat(CompressedData.GetData(), CompressedData.Num());
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(DetectedFormat);

    if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
    {
        TArray<uint8> UncompressedBGRA;

        if (ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, UncompressedBGRA))
        {
            const int32 Width = ImageWrapper->GetWidth();
            const int32 Height = ImageWrapper->GetHeight();

            if (!Texture)
            {
                Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
                if (!Texture)
                {
                    return false; 
                }
            }
            else if (Texture->GetSizeX() != Width || Texture->GetSizeY() != Height)
            {
                Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
                if (!Texture)
                {
                    return false; 
                }
            }

            void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            const int32 TextureDataSize = UncompressedBGRA.Num();

            if (TextureDataSize == Texture->GetPlatformData()->Mips[0].BulkData.GetBulkDataSize())
            {
                FMemory::Memcpy(TextureData, UncompressedBGRA.GetData(), TextureDataSize);
            }
            else
            {
                Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
                return false;
            }

            Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
            Texture->UpdateResource();

            return true; 
        }
        return false;
    }
    return false; 
}

bool UConvaiVisionBaseUtils::ConvertRawDataToTexture2D(const TArray<uint8>& RawData, int32 Width, int32 Height, UTexture2D*& Texture)
{
    if (RawData.Num() != Width * Height * 4)
    {
        return false;
    }

    if (!Texture || Texture->GetSizeX() != Width || Texture->GetSizeY() != Height)
    {
        Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (!Texture)
        {
            return false;
        }
    }

    void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);

    FMemory::Memcpy(TextureData, RawData.GetData(), RawData.Num());

    Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
    Texture->UpdateResource();

    return true;
}

bool UConvaiVisionBaseUtils::GetRawImageData(UTexture2D* CapturedImage, TArray<uint8>& OutData, int& width, int& height)
{
    OutData.Reset();
    width = height = 0;
    
    if (!CapturedImage|| !CapturedImage->GetPlatformData())
    {
        UE_LOG(LogTemp, Error, TEXT("No image captured"));
        return false;
    }

    FTexturePlatformData* PlatformData = CapturedImage->GetPlatformData();
    if (PlatformData->Mips.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("No mips in texture"));
        return false;
    }
    
    // Get the first mip level (highest resolution) from the captured texture
    FTexture2DMipMap& Mip = PlatformData->Mips[0];
    width = Mip.SizeX;
    height = Mip.SizeY;

    const int32 NumPixels = width * height;
    if (NumPixels <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid image size"));
        return false;
    }

    // Lock the bulk data to access the raw pixel data
    const void* RawData = Mip.BulkData.Lock(LOCK_READ_ONLY);
    if (!RawData)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to lock bulk data"));
        return false;
    }
    
    const EPixelFormat PixelFormat = PlatformData->PixelFormat;
    const uint8* SrcPtr = static_cast<const uint8*>(RawData);

    bool bOK = true;
    switch (PixelFormat)
    {
    case PF_R8G8B8A8:
        {
            OutData.SetNumUninitialized(NumPixels * 4);
            FMemory::Memcpy(OutData.GetData(), SrcPtr, OutData.Num());
            break;
        }

    case PF_B8G8R8A8:
        {
            OutData.SetNumUninitialized(NumPixels * 4);
            uint8* DstPtr = OutData.GetData();

            // Swizzle BGRA -> RGBA
            for (int32 i = 0; i < NumPixels; ++i)
            {
                const int32 Idx = i * 4;
                const uint8 B = SrcPtr[Idx + 0];
                const uint8 G = SrcPtr[Idx + 1];
                const uint8 R = SrcPtr[Idx + 2];
                const uint8 A = SrcPtr[Idx + 3];

                DstPtr[Idx + 0] = R;
                DstPtr[Idx + 1] = G;
                DstPtr[Idx + 2] = B;
                DstPtr[Idx + 3] = A;
            }
            break;
        }

    default:
        {
            UE_LOG(LogTemp, Error, TEXT("Unsupported pixel format: %d"), static_cast<int32>(PixelFormat));
            bOK = false;
            break;
        }
    }

    Mip.BulkData.Unlock();
    return bOK;
}

bool UConvaiVisionBaseUtils::GetRawImageDataFromRenderTarget(UTextureRenderTarget2D* RenderTarget,
    TArray<uint8>& OutData, int32& OutWidth, int32& OutHeight, bool bApplyGammaCorrection)
{
    if (!RenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("RenderTarget is null"));
        return false;
    }

    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to get RenderTarget resource"));
        return false;
    }

    OutWidth = RenderTarget->SizeX;
    OutHeight = RenderTarget->SizeY;

    TArray<FColor> Bitmap;   // Will store BGRA8 colors
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false); // Keep raw values
    if (!RTResource->ReadPixels(Bitmap, ReadFlags))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to read pixels from render target"));
        return false;
    }

    if (Bitmap.Num() != OutWidth * OutHeight)
    {
        UE_LOG(LogTemp, Error, TEXT("Unexpected pixel count from render target"));
        return false;
    }

    // If gamma correction is requested, manually brighten by applying power curve
    if (bApplyGammaCorrection)
    {
        for (FColor& Pixel : Bitmap)
        {
            // Convert each channel to 0-1 range, apply gamma, convert back
            float R = Pixel.R / 255.0f;
            float G = Pixel.G / 255.0f;
            float B = Pixel.B / 255.0f;

            // Apply gamma 2.2 curve to brighten (linear to sRGB)
            R = FMath::Pow(R, 1.0f / 2.2f);
            G = FMath::Pow(G, 1.0f / 2.2f);
            B = FMath::Pow(B, 1.0f / 2.2f);

            // Convert back to 0-255 range
            Pixel.R = FMath::Clamp(FMath::RoundToInt(R * 255.0f), 0, 255);
            Pixel.G = FMath::Clamp(FMath::RoundToInt(G * 255.0f), 0, 255);
            Pixel.B = FMath::Clamp(FMath::RoundToInt(B * 255.0f), 0, 255);
        }
    }

    // Convert FColor(BGRA) to RGBA byte array
    OutData.SetNumUninitialized(OutWidth * OutHeight * 4);
    uint8* Dest = OutData.GetData();
    for (int32 i = 0; i < Bitmap.Num(); i++)
    {
        const FColor& C = Bitmap[i];
        *Dest++ = C.R;
        *Dest++ = C.G;
        *Dest++ = C.B;
        *Dest++ = C.A;
    }

    return true;
}

bool UConvaiVisionBaseUtils::TextureRenderTarget2DToBytes(UTextureRenderTarget2D* TextureRenderTarget2D, const EImageFormat ImageFormat, TArray<uint8>& ByteArray, const int32 CompressionQuality, bool bApplyGammaCorrection)
{
    ByteArray = TArray<uint8>();

    if (TextureRenderTarget2D == nullptr)
    {
        return false;
    }
    if (TextureRenderTarget2D->GetFormat() != PF_B8G8R8A8)
    {
        UE_LOG(LogBlueprintUserMessages, Error, TEXT("in ImageToBytes, the TextureRenderTarget2D has a [Render Target Format] that is not supported, use [RTF RGBA8] instead ([PF_B8G8R8A8] in C++)"));
        return false;
    }

    FRenderTarget* RenderTarget = TextureRenderTarget2D->GameThread_GetRenderTargetResource();
    if (RenderTarget == nullptr)
    {
        return false;
    }

    TArray<FColor> Pixels;
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false);  // Don't use built-in conversion

    if (!RenderTarget->ReadPixels(Pixels, ReadFlags))
    {
        return false;
    }

    // If gamma correction is requested, manually brighten by applying power curve
    if (bApplyGammaCorrection)
    {
        for (FColor& Pixel : Pixels)
        {
            // Convert each channel to 0-1 range, apply gamma, convert back
            float R = Pixel.R / 255.0f;
            float G = Pixel.G / 255.0f;
            float B = Pixel.B / 255.0f;

            // Apply gamma 2.2 curve to brighten (linear to sRGB)
            R = FMath::Pow(R, 1.0f / 2.2f);
            G = FMath::Pow(G, 1.0f / 2.2f);
            B = FMath::Pow(B, 1.0f / 2.2f);

            // Convert back to 0-255 range
            Pixel.R = FMath::Clamp(FMath::RoundToInt(R * 255.0f), 0, 255);
            Pixel.G = FMath::Clamp(FMath::RoundToInt(G * 255.0f), 0, 255);
            Pixel.B = FMath::Clamp(FMath::RoundToInt(B * 255.0f), 0, 255);
            Pixel.A = 255;
        }
    }
    else
    {
        for (FColor& Pixel : Pixels)
        {
            Pixel.A = 255;
        }
    }

    return PixelsToBytes(TextureRenderTarget2D->SizeX, TextureRenderTarget2D->SizeY, Pixels, ImageFormat, ByteArray, CompressionQuality);
}

bool UConvaiVisionBaseUtils::PixelsToBytes(const int32 Width, const int32 Height, const TArray<FColor>& Pixels, const EImageFormat ImageFormat, TArray<uint8>& ByteArray, const int32 CompressionQuality)
{
    ByteArray = TArray<uint8>();

    if ((Width <= 0) || (Height <= 0))
    {
        return false;
    }

    if ((CompressionQuality < 0) || (CompressionQuality > 100))
    {
        UE_LOG(LogBlueprintUserMessages, Error, TEXT("in PixelsToBytes, an invalid CompressionQuality (%i) has been given, should be 1-100 or 0 for the default value"), CompressionQuality);
        return false;
    }

    int32 Total = Width * Height;
    if (Total != Pixels.Num())
    {
        UE_LOG(LogBlueprintUserMessages, Error, TEXT("in PixelsToBytes, the number of given Pixels (%i) don't match the given Width (%i) x Height (%i) (Width x Height: %i)"), Pixels.Num(), Width, Height, Total);
        return false;
    }

    if (ImageFormat == EImageFormat::Invalid || ImageFormat == EImageFormat::BMP || ImageFormat == EImageFormat::ICO || ImageFormat == EImageFormat::ICNS)
    {
        UE_LOG(LogBlueprintUserMessages, Error, TEXT("in PixelsToBytes, unsupported or invalid compression format"));
        return false;
    }

    if (ImageFormat == EImageFormat::GrayscaleJPEG)
    {
        TArray<uint8> MutablePixels;
        for (int32 i = 0; i < Total; i++)
        {
            MutablePixels.Add(static_cast<uint8>(FMath::RoundToDouble((0.2125 * Pixels[i].R) + (0.7154 * Pixels[i].G) + (0.0721 * Pixels[i].B))));
        }

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);
        if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(MutablePixels.GetData(), MutablePixels.Num(), Width, Height, ERGBFormat::Gray, 8))
        {
            return false;
        }

        ByteArray = ImageWrapper->GetCompressed(CompressionQuality);
    }
    else
    {
        TArray<FColor> MutablePixels = Pixels;
        for (int32 i = 0; i < Total; i++)
        {
            MutablePixels[i].R = Pixels[i].B;
            MutablePixels[i].B = Pixels[i].R;
        }

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);
        if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(&MutablePixels[0], MutablePixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::RGBA, 8))
        {
            return false;
        }

        ByteArray = ImageWrapper->GetCompressed(CompressionQuality);
    }

    return true;
}
