// Fill out your copyright notice in the Description page of Project Settings.


#include "EnvironmentWebcame.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ConvaiVisionBaseUtils.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"               
#include "Engine/PostProcessVolume.h"   
#include "Engine/World.h" 

UEnvironmentWebcam::UEnvironmentWebcam(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    PrimaryComponentTick.bCanEverTick = true;

    CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("EnvironmentSceneCapture2D"));
    if (CaptureComponent)
    {
        CaptureComponent->SetupAttachment(this);
        CaptureComponent->bCaptureEveryFrame = false; 
        CaptureComponent->bCaptureOnMovement = false;
        CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
    }
}

void UEnvironmentWebcam::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (GetState() == EVisionState::Capturing && OnFrameReady.IsBound())
    {
        OnFrameReady.Broadcast();        
    } 
}

void UEnvironmentWebcam::BeginPlay()
{
    Super::BeginPlay();

    if (bCopyPostProcessProperties) CopyPostProcessPropertiesFromVolume();

    if (bAutoStartVision) Start();
}

void UEnvironmentWebcam::Start()
{
    if (!CanStart())
        return;

    Super::Start();

    if (!CaptureComponent->TextureTarget)
    {
        CaptureComponent->TextureTarget = ConvaiRenderTarget;
    }

    CaptureComponent->bCaptureEveryFrame = true;

    OnFirstFrameCaptured.ExecuteIfBound();
}

void UEnvironmentWebcam::Stop()
{
    if (!CanStop())
        return;

    Super::Stop();
     
    if (CaptureComponent)
    {
        CaptureComponent->bCaptureEveryFrame = false;
        OnFramesStopped.ExecuteIfBound();
    }
}

bool UEnvironmentWebcam::CaptureCompressed(int& width, int& height, TArray<uint8>& data, float ForceCompressionRatio)
{
    width = ConvaiRenderTarget->SizeX;
    height = ConvaiRenderTarget->SizeY;
    // Apply gamma correction to brighten the image for web display
    return UConvaiVisionBaseUtils::TextureRenderTarget2DToBytes(ConvaiRenderTarget, EImageFormat::JPEG, data, ForceCompressionRatio, true);
}

bool UEnvironmentWebcam::CaptureRaw(int& width, int& height, TArray<uint8>& data)
{
    width = ConvaiRenderTarget->SizeX;
    height = ConvaiRenderTarget->SizeY;
    return UConvaiVisionBaseUtils::GetRawImageDataFromRenderTarget(ConvaiRenderTarget, data, width, height);
}

UTexture* UEnvironmentWebcam::GetImageTexture(ETextureSourceType& TextureSourceType)
{
    TextureSourceType = ETextureSourceType::RenderTarget2D;
    return ConvaiRenderTarget;
}

bool UEnvironmentWebcam::CanStart()
{
    if (!Super::CanStart())
        return false;

    if (!CaptureComponent || !ConvaiRenderTarget)
    {
        if (!CaptureComponent)
        {
            SetErrorCodeAndMessage(-1, TEXT("CaptureComponent is null. Cannot start capture."));
        }
        else if (!ConvaiRenderTarget)
        {
            SetErrorCodeAndMessage(-1, TEXT("ConvaiRenderTarget is null. Cannot start capture."));
        }
        
        return false;
    }

    return true;
}

void UEnvironmentWebcam::CopyPostProcessPropertiesFromVolume()
{
    if (!bCopyPostProcessProperties || !CaptureComponent)
    {
        return;
    }

    // Get the world
    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEnvironmentWebcam::CopyPostProcessPropertiesFromVolume - World is null"));
        return;
    }

    // Find the first PostProcessVolume in the world by iterating through all actors
    APostProcessVolume* PostProcessVolume = nullptr;
    for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
    {
        AActor* Actor = *ActorItr;
        if (Actor && Actor->IsA<APostProcessVolume>())
        {
            PostProcessVolume = Cast<APostProcessVolume>(Actor);
            break; // Use the first one found
        }
    }

    if (!PostProcessVolume)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEnvironmentWebcam::CopyPostProcessPropertiesFromVolume - No PostProcessVolume found in the world"));
        return;
    }

    // Copy post-process settings from the volume to the capture component
    CaptureComponent->PostProcessSettings = PostProcessVolume->Settings;

    UE_LOG(LogTemp, Log, TEXT("UEnvironmentWebcam::CopyPostProcessPropertiesFromVolume - Successfully copied post-process properties from PostProcessVolume"));
}