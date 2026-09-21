#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "IImageWrapper.h"

#include "ConvaiVisionBaseUtils.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiVisionBaseUtilsLog, Log, All);

UCLASS()
class CONVAIVISIONBASE_API UConvaiVisionBaseUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	static bool ConvertCompressedDataToTexture2D(const TArray<uint8>& CompressedData, UTexture2D*& Texture);

	static bool ConvertRawDataToTexture2D(const TArray<uint8>& RawData, int32 Width, int32 Height, UTexture2D*& Texture);

	static bool GetRawImageData(UTexture2D* CapturedImage, TArray<uint8>& OutData, int& width, int& height);

	static bool GetRawImageDataFromRenderTarget(UTextureRenderTarget2D* RenderTarget, TArray<uint8>& OutData, int32& OutWidth, int32& OutHeight, bool bApplyGammaCorrection = true);

	static bool TextureRenderTarget2DToBytes(UTextureRenderTarget2D* TextureRenderTarget2D, const EImageFormat ImageFormat, TArray<uint8>& ByteArray, const int32 CompressionQuality = 0, bool bApplyGammaCorrection = true);

	static bool PixelsToBytes(const int32 Width, const int32 Height, const TArray<FColor>& Pixels, const EImageFormat ImageFormat, TArray<uint8>& ByteArray, const int32 CompressionQuality = 0);
};