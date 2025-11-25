// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiWebcamBase.h"

#include "EnvironmentWebcame.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

// Redirect old class name to new class name to prevent breaking existing content
UCLASS(BlueprintType, Blueprintable, ClassGroup = (Convai), meta = (BlueprintSpawnableComponent))
class CONVAIVISIONBASE_API UEnvironmentWebcam : public UConvaiWebcamBase
{
	GENERATED_BODY()
public:
	UEnvironmentWebcam(const FObjectInitializer& ObjectInitializer);
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

public:
	// VisionInterface functions start
	virtual void BeginPlay() override;
	virtual void Start() override;
	virtual void Stop() override;
	virtual bool CaptureCompressed(int& width, int& height, TArray<uint8>& data, float ForceCompressionRatio) override;
	virtual bool CaptureRaw(int& width, int& height, TArray<uint8>& data) override;
	virtual UTexture* GetImageTexture(ETextureSourceType& TextureSourceType) override;
	// VisionInterface functions END

protected:
	// WebcamBase functions start
	virtual bool CanStart() override;
	// WebcamBase functions end

	// Scene capture component for 2D capture functionality
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Vision")
	USceneCaptureComponent2D* CaptureComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Vision")
	UTextureRenderTarget2D* ConvaiRenderTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Vision")
	bool bCopyPostProcessProperties = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Convai|Vision")
	bool bAutoStartVision = false;
	void CopyPostProcessPropertiesFromVolume();
};
