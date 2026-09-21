#include "SceneAutoTaggerCapture.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SpotLightComponent.h"
#include "ContentStreaming.h"
#include "Containers/Ticker.h"
#include "Engine/EngineTypes.h"
#include "Interfaces/Interface_PostProcessVolume.h"
#include "Engine/RendererSettings.h"
#include "Engine/Scene.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#if WITH_EDITOR
#include "LevelEditor.h"
#endif
#include "Misc/Paths.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "Math/Float16Color.h"
#include "Modules/ModuleManager.h"
#include "RHITypes.h"
#include "RenderCommandFence.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "FinalPostProcessSettings.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "SLevelViewport.h"
#endif
#include "Serialization/Archive.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StrongObjectPtr.h"

namespace ConvaiSceneAutoTagger
{
DEFINE_LOG_CATEGORY_STATIC(LogConvaiSceneAutoTaggerCapture, Log, All);

constexpr int32 GridLabelDigitWidth = 5;
constexpr int32 GridLabelDigitHeight = 7;

bool IsCaptureBackgroundPixel(
	const FColor& Pixel,
	const FColor& EncodedBackdrop,
	const int32 ColorEpsilon)
{
	const int32 SafeEpsilon = FMath::Clamp(ColorEpsilon, 0, 255);
	const bool bMatchesEncodedBackdrop =
		FMath::Abs(static_cast<int32>(Pixel.R) - static_cast<int32>(EncodedBackdrop.R)) <= SafeEpsilon
		&& FMath::Abs(static_cast<int32>(Pixel.G) - static_cast<int32>(EncodedBackdrop.G)) <= SafeEpsilon
		&& FMath::Abs(static_cast<int32>(Pixel.B) - static_cast<int32>(EncodedBackdrop.B)) <= SafeEpsilon;
	if (bMatchesEncodedBackdrop)
	{
		return true;
	}

	const auto IsNeutralEncodedBlack = [](const FColor& Color)
	{
		const int32 Maximum = FMath::Max3(Color.R, Color.G, Color.B);
		const int32 Minimum = FMath::Min3(Color.R, Color.G, Color.B);
		return Maximum <= 16 && Maximum - Minimum <= 4;
	};
	return IsNeutralEncodedBlack(Pixel) && IsNeutralEncodedBlack(EncodedBackdrop);
}

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
bool GNeutralCaptureLightingForTests = true;
TWeakObjectPtr<UWorld> GNeutralCaptureLightingTestWorld;
#endif

bool UseNeutralCaptureLighting(const UWorld* World)
{
#if WITH_DEV_AUTOMATION_TESTS
	return !World || GNeutralCaptureLightingTestWorld.Get() != World
		|| GNeutralCaptureLightingForTests;
#else
	return true;
#endif
}

UTextureCube* CreateNeutralStudioCubemap()
{
	// A small analytic, achromatic environment supplies diffuse and specular
	// ambient light. It contains no scene imagery and never captures the level.
	constexpr int32 Size = 32;
	constexpr int32 MipCount = 6;
	UTextureCube* Texture = UTextureCube::CreateTransient(Size, Size, PF_FloatRGBA);
	if (!Texture)
	{
		return nullptr;
	}
	Texture->SRGB = false;
	Texture->NeverStream = true;
	Texture->Filter = TF_Trilinear;
#if WITH_EDITORONLY_DATA
	Texture->MipGenSettings = TMGS_LeaveExistingMips;
#endif
	FTexturePlatformData* Data = Texture->GetPlatformData();
	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		const int32 MipSize = Size >> MipIndex;
		if (MipIndex > 0)
		{
			Data->Mips.Add(new FTexture2DMipMap(MipSize, MipSize, 1));
		}
		FTexture2DMipMap& Mip = Data->Mips[MipIndex];
		Mip.BulkData.Lock(LOCK_READ_WRITE);
		FFloat16Color* Pixels = reinterpret_cast<FFloat16Color*>(Mip.BulkData.Realloc(
			static_cast<int64>(6) * MipSize * MipSize * sizeof(FFloat16Color)));
		// Broad lobes fade toward their neutral mean as roughness chooses coarser
		// mips. The final mip is identical on every face, avoiding cube seams.
		const float DirectionalScale = FMath::Square(1.0f - static_cast<float>(MipIndex) / (MipCount - 1));
		for (int32 Face = 0; Face < 6; ++Face)
		{
			for (int32 Y = 0; Y < MipSize; ++Y)
			{
				for (int32 X = 0; X < MipSize; ++X)
				{
					const float U = 2.0f * (X + 0.5f) / MipSize - 1.0f;
					const float V = 2.0f * (Y + 0.5f) / MipSize - 1.0f;
					FVector Direction;
					switch (Face)
					{
					case 0: Direction = FVector(1, -V, -U); break;
					case 1: Direction = FVector(-1, -V, U); break;
					case 2: Direction = FVector(U, 1, V); break;
					case 3: Direction = FVector(U, -1, -V); break;
					case 4: Direction = FVector(U, -V, 1); break;
					default: Direction = FVector(-U, -V, -1); break;
					}
					Direction.Normalize();
					const float Radiance = 0.30f + DirectionalScale
						* (0.22f * static_cast<float>(Direction.Z) + 0.10f * static_cast<float>(Direction.X));
					Pixels[(Face * MipSize + Y) * MipSize + X] =
						FFloat16Color(FLinearColor(Radiance, Radiance, Radiance, 1.0f));
				}
			}
		}
		Mip.BulkData.Unlock();
	}
	Texture->UpdateResource();
	return Texture;
}

// Component-local only: never register this with FSceneViewExtensions. SetupView
// runs after the source world's post-process volumes have been blended.
class FNeutralCaptureViewExtension final : public ISceneViewExtension
{
public:
	explicit FNeutralCaptureViewExtension(USceneCaptureComponent2D* InComponent)
		: Component(InComponent)
		, StudioCubemap(CreateNeutralStudioCubemap())
	{}
	virtual void SetupViewFamily(FSceneViewFamily& ViewFamily) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& ViewFamily) override {}

	virtual void SetupView(FSceneViewFamily& ViewFamily, FSceneView& View) override
	{
		const USceneCaptureComponent2D* Capture = Component.Get();
		if (!Capture || !UseNeutralCaptureLighting(Capture->GetWorld()))
		{
			return;
		}
		// Scene captures do not apply the viewport's material override for this
		// show flag. Keep planar captures diffuse-only in both shading paths.
		if (!ViewFamily.EngineShowFlags.Specular)
		{
			View.SpecularOverrideParameter = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		}
		// Reset the final settings, including accumulated LUTs, cubemaps and
		// blendable data. Resetting only the component settings does not remove
		// source-world post-processing.
		View.FinalPostProcessSettings = FFinalPostProcessSettings();
		FFinalPostProcessSettings& Settings = View.FinalPostProcessSettings;
		Settings.SetBaseValues();
		Settings.AutoExposureMethod = AEM_Manual;
		Settings.AutoExposureApplyPhysicalCameraExposure = false;
		Settings.AutoExposureBias = Capture->PostProcessSettings.AutoExposureBias;
		Settings.AutoExposureBiasCurve = nullptr;
		Settings.AutoExposureMeterMask = nullptr;
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		Settings.ReflectionMethod = EReflectionMethod::None;
		// SetupView is after EndFinalPostprocessSettings: keep its no-GI result
		// explicit so baked lightmaps/indirect caches cannot return after reset.
		Settings.IndirectLightingColor = FLinearColor::Black;
		Settings.IndirectLightingIntensity = 0.0f;
		Settings.AmbientOcclusionIntensity = 0.0f;
		Settings.BloomIntensity = 0.0f;
		Settings.VignetteIntensity = 0.0f;
		Settings.MotionBlurAmount = 0.0f;
		Settings.SceneFringeIntensity = 0.0f;
		// Add only our generated environment after removing all source cubemaps.
		// The deferred ambient pass evaluates specular independently of the
		// disabled world GI/reflection paths. Planar diffuse captures stay free of
		// environmental highlights, matching their existing material policy.
		if (StudioCubemap.IsValid() && ViewFamily.EngineShowFlags.Specular)
		{
			FFinalPostProcessSettings::FCubemapEntry Entry;
			Entry.AmbientCubemap = StudioCubemap.Get();
			// Keep the broad ambient floor low so glossy dark surfaces retain
			// their base color and texture contrast.
			Entry.AmbientCubemapTintMulScaleValue = FLinearColor(0.10f, 0.10f, 0.10f, 1.0f);
			Settings.ContributingCubemaps.Add(Entry);
		}
	}

private:
	TWeakObjectPtr<USceneCaptureComponent2D> Component;
	TStrongObjectPtr<UTextureCube> StudioCubemap;
};

constexpr float CaptureFovDegrees = 35.0f;
constexpr float CaptureFrameFill = 0.80f;
constexpr int32 MinCaptureResolution = 128;
constexpr int32 MaxCaptureResolution = 2048;
constexpr int32 BackgroundColorEpsilon = 8;
constexpr float FallbackFillReferenceIntensity = 2800.0f;
constexpr float FallbackFillReferenceRadius = 100.0f;
constexpr float FallbackKeyIntensityScale = 0.65f;
constexpr float FallbackKeySpecularScale = 0.45f;
constexpr float OpposingDiffuseIntensityScale = 0.30f;
constexpr float ShadowBalanceSpecularScale = 0.12f;
constexpr float FallbackKeyInitialExposureDelta = 0.50f;
constexpr float FallbackKeyBrightExposureDelta = 1.50f;
constexpr float FallbackKeyFinalExposureDelta = 2.50f;
constexpr float NeutralReadabilityExposureDelta = 3.50f;
constexpr float MaxReadabilityRecoveryShoulderFraction = 0.30f;
constexpr float MaxReadabilityRecoveryClippedFraction = 0.04f;
constexpr float MaxKeyRecoveryShoulderFraction = 0.18f;
constexpr float MaxKeyRecoveryClippedFraction = 0.015f;
constexpr int32 MaxHighlightMetricSamples = 65536;
constexpr int32 CaptureProfileSettleFrameCount = 2;
const FLinearColor CaptureBackground(0.16f, 0.16f, 0.17f, 1.0f);

TWeakObjectPtr<UWorld> GCaptureWorld;
TWeakObjectPtr<ASceneCapture2D> GCaptureActor;
TWeakObjectPtr<USpotLightComponent> GKeyLight;
TWeakObjectPtr<USpotLightComponent> GFillLight;
TWeakObjectPtr<UTextureRenderTarget2D> GCaptureRenderTarget;
int32 GCaptureRenderTargetResolution = 0;
TWeakObjectPtr<UTextureRenderTarget2D> GGeometryMaskRenderTarget;
int32 GGeometryMaskRenderTargetResolution = 0;
TSharedPtr<FNeutralCaptureViewExtension, ESPMode::ThreadSafe> GNeutralCaptureViewExtension;

void FillRect(
	TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	const int32 X,
	const int32 Y,
	const int32 RectWidth,
	const int32 RectHeight,
	const FColor& Color)
{
	const int32 MinX = FMath::Clamp(X, 0, Width);
	const int32 MinY = FMath::Clamp(Y, 0, Height);
	const int32 MaxX = FMath::Clamp(X + RectWidth, 0, Width);
	const int32 MaxY = FMath::Clamp(Y + RectHeight, 0, Height);
	for (int32 PixelY = MinY; PixelY < MaxY; ++PixelY)
	{
		for (int32 PixelX = MinX; PixelX < MaxX; ++PixelX)
		{
			Pixels[PixelY * Width + PixelX] = Color;
		}
	}
}

bool DigitBit(const int32 Digit, const int32 X, const int32 Y)
{
	static const uint8 Digits[10][GridLabelDigitHeight] =
	{
		{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
		{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
		{ 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
		{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
		{ 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
		{ 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E },
		{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
		{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
		{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E },
	};
	return Digit >= 0 && Digit <= 9
		&& X >= 0 && X < GridLabelDigitWidth
		&& Y >= 0 && Y < GridLabelDigitHeight
		&& (Digits[Digit][Y] & (1 << (GridLabelDigitWidth - 1 - X))) != 0;
}

void DrawNumber(
	TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	const int32 X,
	const int32 Y,
	const int32 Value)
{
	const FString Label = FString::FromInt(Value);
	constexpr int32 Scale = 4;
	constexpr int32 Gap = 2;
	const int32 TextWidth = Label.Len() * GridLabelDigitWidth * Scale
		+ FMath::Max(0, Label.Len() - 1) * Gap;
	const int32 TextHeight = GridLabelDigitHeight * Scale;
	FillRect(Pixels, Width, Height, X - 5, Y - 5, TextWidth + 10, TextHeight + 10, FColor(18, 18, 18, 255));

	int32 CursorX = X;
	for (const TCHAR Character : Label)
	{
		const int32 Digit = static_cast<int32>(Character - TEXT('0'));
		for (int32 DigitY = 0; DigitY < GridLabelDigitHeight; ++DigitY)
		{
			for (int32 DigitX = 0; DigitX < GridLabelDigitWidth; ++DigitX)
			{
				if (DigitBit(Digit, DigitX, DigitY))
				{
					FillRect(Pixels, Width, Height, CursorX + DigitX * Scale,
						Y + DigitY * Scale, Scale, Scale, FColor(255, 232, 80, 255));
				}
			}
		}
		CursorX += GridLabelDigitWidth * Scale + Gap;
	}
}

void SetFallbackLightViewChannel(FViewLightingChannels& Channels)
{
	Channels.bViewChannel0 = false;
	Channels.bViewChannel1 = false;
	Channels.bViewChannel2 = false;
	Channels.bViewChannel3 = false;
	Channels.bViewChannel4 = true;
}

void SetSceneAndFallbackViewLightingChannels(FViewLightingChannels& Channels)
{
	// Normal authored level lights use channel 0. Channel 4 is reserved for the
	// bounded emergency fill, which the ordinary Level viewport never sees.
	Channels.bViewChannel0 = true;
	Channels.bViewChannel1 = false;
	Channels.bViewChannel2 = false;
	Channels.bViewChannel3 = false;
	Channels.bViewChannel4 = true;
}

void ConfigureCaptureLightViewIsolation(USpotLightComponent* Light)
{
	if (!Light)
	{
		return;
	}
	const uint8 PreviousMask = Light->ViewLightingChannels.GetMaskForStruct();
	SetFallbackLightViewChannel(Light->ViewLightingChannels);
	if (Light->IsRegistered()
		&& PreviousMask != Light->ViewLightingChannels.GetMaskForStruct())
	{
		Light->MarkRenderStateDirty();
	}
}

void SetViewLightingMask(FViewLightingChannels& Channels, const uint8 Mask)
{
	Channels.bViewChannel0 = (Mask & 1) != 0;
	Channels.bViewChannel1 = (Mask & 2) != 0;
	Channels.bViewChannel2 = (Mask & 4) != 0;
	Channels.bViewChannel3 = (Mask & 8) != 0;
	Channels.bViewChannel4 = (Mask & 16) != 0;
}

bool ConfigureNeutralLightIsolation(
	USceneCaptureComponent2D* Capture,
	USpotLightComponent* KeyLight,
	USpotLightComponent* FillLight,
	FString& OutError)
{
	if (!UseNeutralCaptureLighting(Capture ? Capture->GetWorld() : nullptr))
	{
		// Preserve the pre-fix path exactly for the real-render comparison seam,
		// including when it reuses a capture that previously ran in neutral mode.
		if (KeyLight)
		{
			KeyLight->SetLightingChannels(true, false, false);
		}
		if (FillLight)
		{
			FillLight->SetLightingChannels(true, false, false);
		}
		return true;
	}
	if (!Capture || !KeyLight || !FillLight)
	{
		OutError = TEXT("capture failed: could not create neutral capture lights");
		return false;
	}
	uint8 OccupiedChannels = 1; // Never illuminate the ordinary Level viewport.
	for (TObjectIterator<ULightComponent> It; It; ++It)
	{
		const ULightComponent* Light = *It;
		if (Light != KeyLight && Light != FillLight && Light->IsRegistered()
			&& Light->GetWorld() == Capture->GetWorld())
		{
			OccupiedChannels |= Light->ViewLightingChannels.GetMaskForStruct();
		}
	}
	uint8 CaptureChannel = 0;
	for (int32 Channel = 4; Channel >= 1; --Channel)
	{
		if ((OccupiedChannels & (1 << Channel)) == 0)
		{
			CaptureChannel = static_cast<uint8>(1 << Channel);
			break;
		}
	}
	if (CaptureChannel == 0)
	{
		OutError = TEXT("capture failed: neutral lighting needs one unused custom view lighting channel");
		return false;
	}
	SetViewLightingMask(Capture->ViewLightingChannels, CaptureChannel);
	for (USpotLightComponent* Light : {KeyLight, FillLight})
	{
		const uint8 PreviousMask = Light->ViewLightingChannels.GetMaskForStruct();
		SetViewLightingMask(Light->ViewLightingChannels, CaptureChannel);
		// Primitive channels and view channels are independent. The rig must
		// illuminate authored custom-channel primitives without changing them.
		Light->SetLightingChannels(true, true, true);
		if (PreviousMask != CaptureChannel)
		{
			Light->MarkRenderStateDirty();
		}
	}
	return true;
}

void AttachNeutralCaptureExtension(
	USceneCaptureComponent2D* Component,
	TSharedPtr<FNeutralCaptureViewExtension, ESPMode::ThreadSafe>& Extension)
{
	if (!Extension)
	{
		Extension = MakeShared<FNeutralCaptureViewExtension, ESPMode::ThreadSafe>(Component);
		Component->SceneViewExtensions.Add(Extension);
	}
}

void ReleaseRootedRenderTargetAfterFence(UTextureRenderTarget2D* RenderTarget)
{
	if (!RenderTarget || !RenderTarget->IsRooted())
	{
		return;
	}
	const TWeakObjectPtr<UTextureRenderTarget2D> WeakRenderTarget = RenderTarget;
	const TSharedRef<FRenderCommandFence> Fence = MakeShared<FRenderCommandFence>();
	Fence->BeginFence();
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakRenderTarget, Fence](float)
		{
			if (!Fence->IsFenceComplete())
			{
				return true;
			}
			if (UTextureRenderTarget2D* Target = WeakRenderTarget.Get();
				Target && Target->IsRooted())
			{
				Target->RemoveFromRoot();
			}
			return false;
		}),
		0.0f);
}

void ReleaseCaptureRenderTarget()
{
	if (UTextureRenderTarget2D* RenderTarget = GCaptureRenderTarget.Get())
	{
		if (RenderTarget->IsRooted())
		{
			RenderTarget->RemoveFromRoot();
		}
	}
	GCaptureRenderTarget.Reset();
	GCaptureRenderTargetResolution = 0;
}

void ReleaseGeometryMaskRenderTarget()
{
	if (UTextureRenderTarget2D* RenderTarget = GGeometryMaskRenderTarget.Get())
	{
		if (RenderTarget->IsRooted())
		{
			RenderTarget->RemoveFromRoot();
		}
	}
	GGeometryMaskRenderTarget.Reset();
	GGeometryMaskRenderTargetResolution = 0;
}

UTextureRenderTarget2D* GetReusableCaptureRenderTarget(const int32 Resolution)
{
	if (UTextureRenderTarget2D* Existing = GCaptureRenderTarget.Get())
	{
		if (GCaptureRenderTargetResolution == Resolution
			&& Existing->SizeX == Resolution
			&& Existing->SizeY == Resolution)
		{
			return Existing;
		}
	}

	ReleaseCaptureRenderTarget();
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	if (!RenderTarget)
	{
		return nullptr;
	}
	// FinalColorLDR needs a display-gamma render target. UTextureRenderTarget2D
	// otherwise defaults bForceLinearGamma to true, which makes the tonemapper
	// write gamma-1.0 bytes that look much darker when retained as ordinary sRGB
	// image data. This is the render-target equivalent of Convai Vision's explicit
	// linear-to-sRGB conversion.
	RenderTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
	RenderTarget->bForceLinearGamma = false;
	RenderTarget->SRGB = true;
	RenderTarget->ClearColor = CaptureBackground;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(Resolution, Resolution);
	RenderTarget->UpdateResourceImmediate(true);
	RenderTarget->AddToRoot();
	GCaptureRenderTarget = RenderTarget;
	GCaptureRenderTargetResolution = Resolution;
	return RenderTarget;
}

UTextureRenderTarget2D* GetReusableGeometryMaskRenderTarget(const int32 Resolution)
{
	if (UTextureRenderTarget2D* Existing = GGeometryMaskRenderTarget.Get())
	{
		if (GGeometryMaskRenderTargetResolution == Resolution
			&& Existing->SizeX == Resolution
			&& Existing->SizeY == Resolution)
		{
			return Existing;
		}
	}

	ReleaseGeometryMaskRenderTarget();
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	if (!RenderTarget)
	{
		return nullptr;
	}
	// Device depth is encoded into RGB. A linear 8-bit target is sufficient for
	// separating the exact show-only geometry from the untouched background and
	// avoids making foreground detection depend on the object's albedo or lighting.
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->bForceLinearGamma = true;
	RenderTarget->SRGB = false;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(Resolution, Resolution);
	RenderTarget->UpdateResourceImmediate(true);
	RenderTarget->AddToRoot();
	GGeometryMaskRenderTarget = RenderTarget;
	GGeometryMaskRenderTargetResolution = Resolution;
	return RenderTarget;
}

ASceneCapture2D* GetReusableCaptureActor(UWorld* World)
{
	if (GCaptureWorld.Get() == World)
	{
		if (ASceneCapture2D* Existing = GCaptureActor.Get())
		{
			return Existing;
		}
	}
	if (ASceneCapture2D* PreviousActor = GCaptureActor.Get())
	{
		PreviousActor->Destroy();
	}
	GCaptureWorld.Reset();
	GNeutralCaptureViewExtension.Reset();
	GCaptureActor.Reset();
	GKeyLight.Reset();
	GFillLight.Reset();

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		ASceneCapture2D::StaticClass(),
		TEXT("ConvaiSceneAutoTaggerCapture"));
	SpawnParameters.ObjectFlags = RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(SpawnParameters);
	if (CaptureActor)
	{
		CaptureActor->SetActorEnableCollision(false);
		GCaptureWorld = World;
		GCaptureActor = CaptureActor;
		GKeyLight.Reset();
		GFillLight.Reset();
	}
	return CaptureActor;
}

USpotLightComponent* CreateCaptureLight(ASceneCapture2D* CaptureActor, const FName BaseName)
{
	const FName UniqueName = MakeUniqueObjectName(CaptureActor, USpotLightComponent::StaticClass(), BaseName);
	USpotLightComponent* Light = NewObject<USpotLightComponent>(CaptureActor, UniqueName, RF_Transient);
	if (!Light)
	{
		return nullptr;
	}

	Light->SetMobility(EComponentMobility::Movable);
	Light->SetVisibility(false);
	Light->SetHiddenInGame(false);
	Light->SetCastShadows(false);
	// Recovery lights are deterministic direct illumination only. Feeding a
	// camera-relative transient light into Lumen/reflection history makes a
	// one-shot probe differ from the persistent Edit View and can leave ghosts.
	Light->SetIndirectLightingIntensity(0.0f);
	Light->SetAffectGlobalIllumination(false);
	Light->SetAffectReflection(false);
	Light->SetVolumetricScatteringIntensity(0.0f);
	Light->SetInnerConeAngle(32.0f);
	Light->SetOuterConeAngle(58.0f);
	Light->SetSourceRadius(20.0f);
	// View-light channels are independent of primitive lighting channels. The
	// capture can opt into this fallback channel without exposing the transient
	// light to the ordinary Level viewport.
	ConfigureCaptureLightViewIsolation(Light);
	CaptureActor->AddInstanceComponent(Light);
	Light->RegisterComponent();
	return Light;
}

void EnsureCaptureLights(ASceneCapture2D* CaptureActor)
{
	if (!GKeyLight.IsValid() || GKeyLight->GetOwner() != CaptureActor)
	{
		GKeyLight = CreateCaptureLight(CaptureActor, TEXT("ConvaiSceneAutoTaggerKeyLight"));
	}
	if (!GFillLight.IsValid() || GFillLight->GetOwner() != CaptureActor)
	{
		GFillLight = CreateCaptureLight(CaptureActor, TEXT("ConvaiSceneAutoTaggerFillLight"));
	}
	ConfigureCaptureLightViewIsolation(GKeyLight.Get());
	ConfigureCaptureLightViewIsolation(GFillLight.Get());
}

void SetCaptureLightsVisible(const bool bVisible)
{
	if (USpotLightComponent* KeyLight = GKeyLight.Get())
	{
		KeyLight->SetVisibility(bVisible);
	}
	if (USpotLightComponent* FillLight = GFillLight.Get())
	{
		FillLight->SetVisibility(bVisible);
	}
}

float ComputeFallbackFillIntensity(const float Radius)
{
	const float RadiusScale = FMath::Max(Radius, 25.0f) / FallbackFillReferenceRadius;
	return FallbackFillReferenceIntensity * RadiusScale * RadiusScale;
}

void SetFallbackFillVisible(
	USpotLightComponent* KeyLight,
	USpotLightComponent* FillLight,
	const bool bVisible)
{
	if (UseNeutralCaptureLighting(KeyLight ? KeyLight->GetWorld() : (FillLight ? FillLight->GetWorld() : nullptr)))
	{
		// The neutral rig is the baseline, not an emergency scene-light supplement.
		if (KeyLight)
		{
			KeyLight->SetVisibility(true);
		}
		if (FillLight)
		{
			FillLight->SetVisibility(true);
		}
		return;
	}
	if (KeyLight)
	{
		KeyLight->SetVisibility(false);
	}
	if (FillLight)
	{
		FillLight->SetVisibility(bVisible);
	}
}

void SetFallbackFillVisible(const bool bVisible)
{
	SetFallbackFillVisible(GKeyLight.Get(), GFillLight.Get(), bVisible);
}

void SetCaptureRecoveryLightsVisible(
	USpotLightComponent* KeyLight,
	USpotLightComponent* FillLight,
	const bool bFillVisible,
	const bool bKeyVisible,
	const bool bOpposingDiffuseVisible = false)
{
	if (UseNeutralCaptureLighting(KeyLight ? KeyLight->GetWorld() : (FillLight ? FillLight->GetWorld() : nullptr)))
	{
		// Recovery may adjust exposure, but it never restores world lighting or
		// switches off the only illumination source. Specular is controlled by
		// the capture's planar/material policy, not by source actor mutation.
		if (KeyLight)
		{
			const ASceneCapture2D* CaptureActor = Cast<ASceneCapture2D>(KeyLight->GetOwner());
			const USceneCaptureComponent2D* Capture = CaptureActor ? CaptureActor->GetCaptureComponent2D() : nullptr;
			KeyLight->SetIntensity((FillLight ? FillLight->Intensity : 0.0f) * FallbackKeyIntensityScale);
			KeyLight->SetSpecularScale(Capture && Capture->ShowFlags.Specular ? FallbackKeySpecularScale : 0.0f);
		}
		SetFallbackFillVisible(KeyLight, FillLight, true);
		return;
	}
	if (KeyLight)
	{
		const float FillIntensity = FillLight ? FillLight->Intensity : 0.0f;
		if (bKeyVisible && bOpposingDiffuseVisible)
		{
			// The combined state is a deliberately gentle shadow-balance profile:
			// keep the opposing light at diffuse-fill power and add only a small
			// specular contribution so rough metals do not remain black.
			KeyLight->SetIntensity(FillIntensity * OpposingDiffuseIntensityScale);
			KeyLight->SetSpecularScale(ShadowBalanceSpecularScale);
		}
		else if (bKeyVisible)
		{
			KeyLight->SetIntensity(FillIntensity * FallbackKeyIntensityScale);
			KeyLight->SetSpecularScale(FallbackKeySpecularScale);
		}
		else if (bOpposingDiffuseVisible)
		{
			KeyLight->SetIntensity(FillIntensity * OpposingDiffuseIntensityScale);
			KeyLight->SetSpecularScale(0.0f);
		}
		KeyLight->SetVisibility(bKeyVisible || bOpposingDiffuseVisible);
	}
	if (FillLight)
	{
		FillLight->SetVisibility(bFillVisible || bKeyVisible || bOpposingDiffuseVisible);
	}
}

void SetCaptureRecoveryLightsVisible(
	const bool bFillVisible,
	const bool bKeyVisible,
	const bool bOpposingDiffuseVisible = false)
{
	SetCaptureRecoveryLightsVisible(
		GKeyLight.Get(),
		GFillLight.Get(),
		bFillVisible,
		bKeyVisible,
		bOpposingDiffuseVisible);
}

void SetCaptureLightsVisible(
	USpotLightComponent* KeyLight,
	USpotLightComponent* FillLight,
	const bool bVisible)
{
	if (KeyLight)
	{
		KeyLight->SetVisibility(bVisible);
	}
	if (FillLight)
	{
		FillLight->SetVisibility(bVisible);
	}
}

void PositionCaptureLights(
	USpotLightComponent* KeyLight,
	USpotLightComponent* FillLight,
	const FVector& Center,
	const FVector& CameraLocation,
	const float Radius,
	const bool bSuppressDirectSpecularHighlights,
	const bool bAllowSuppressedSpecularRecovery)
{
	const FVector Forward = (Center - CameraLocation).GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);
	FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();
	if (Right.IsNearlyZero())
	{
		Right = FVector::RightVector;
	}
	const FVector Up = FVector::CrossProduct(Forward, Right).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
	const float SafeRadius = FMath::Max(Radius, 25.0f);

	if (KeyLight)
	{
		const bool bCanUseKey = !bSuppressDirectSpecularHighlights
			|| bAllowSuppressedSpecularRecovery;
		const FVector Location = Center - Forward * (SafeRadius * 1.6f)
			- Right * (SafeRadius * 0.55f) + Up * (SafeRadius * 0.55f);
		KeyLight->SetWorldLocationAndRotation(Location, (Center - Location).Rotation());
		KeyLight->SetAttenuationRadius(SafeRadius * 5.0f);
		KeyLight->SetSourceRadius(FMath::Clamp(SafeRadius * 0.25f, 10.0f, 60.0f));
		KeyLight->SetCastShadows(false);
		KeyLight->SetSpecularScale(
			UseNeutralCaptureLighting(KeyLight->GetWorld()) && !bSuppressDirectSpecularHighlights
				? FallbackKeySpecularScale : 0.0f);
		KeyLight->SetIntensity(
			bCanUseKey
				? ComputeFallbackFillIntensity(SafeRadius) * FallbackKeyIntensityScale
				: 0.0f);
	}

	if (FillLight)
	{
		// A high side fill reveals contours without flattening the front face.
		const FVector Location = UseNeutralCaptureLighting(FillLight->GetWorld())
			? Center + Forward * (SafeRadius * 0.3f)
				+ Right * (SafeRadius * 1.3f) + Up * (SafeRadius * 1.5f)
			: Center - Forward * (SafeRadius * 1.6f)
				+ Right * (SafeRadius * 1.2f) + Up * (SafeRadius * 0.25f);
		FillLight->SetWorldLocationAndRotation(Location, (Center - Location).Rotation());
		FillLight->SetAttenuationRadius(SafeRadius * 5.0f);
		FillLight->SetSourceRadius(FMath::Clamp(SafeRadius * 0.25f, 10.0f, 60.0f));
		FillLight->SetCastShadows(false);
		// Keep the first fallback genuinely neutral and diffuse. True metals that
		// cannot respond to it may receive the separately measured key probe below.
		FillLight->SetSpecularScale(0.0f);
		FillLight->SetIntensity(ComputeFallbackFillIntensity(SafeRadius));
	}

	// Neutral mode uses this rig from the first frame. The automation-only
	// legacy comparison retains its original emergency-fill behavior.
	SetFallbackFillVisible(KeyLight, FillLight, false);
}

void PositionCaptureLights(
	ASceneCapture2D* CaptureActor,
	const FVector& Center,
	const FVector& CameraLocation,
	const float Radius,
	const bool bSuppressDirectSpecularHighlights,
	const bool bAllowSuppressedSpecularRecovery)
{
	EnsureCaptureLights(CaptureActor);
	// Position the owned rig without changing any source-world light or primitive.
	PositionCaptureLights(
		GKeyLight.Get(),
		GFillLight.Get(),
		Center,
		CameraLocation,
		Radius,
		bSuppressDirectSpecularHighlights,
		bAllowSuppressedSpecularRecovery);
}

bool HasCapturableComponent(UWorld* World, const TArray<UPrimitiveComponent*>& Components)
{
	for (const UPrimitiveComponent* Component : Components)
	{
		if (IsValid(Component) && Component->IsRegistered() && Component->GetWorld() == World)
		{
			return true;
		}
	}
	return false;
}

bool IsWithinColorEpsilon(const FColor& Pixel, const FColor& Target)
{
	return IsCaptureBackgroundPixel(Pixel, Target, BackgroundColorEpsilon);
}

FColor EstimateEncodedBackdrop(const TArray<FColor>& Pixels, const int32 Resolution)
{
	if (Resolution <= 0 || Pixels.Num() != Resolution * Resolution)
	{
		return FColor::Black;
	}
	const FColor Corners[] =
	{
		Pixels[0],
		Pixels[Resolution - 1],
		Pixels[(Resolution - 1) * Resolution],
		Pixels.Last()
	};
	const auto MedianChannel = [&Corners](const uint8 FColor::* Channel)
	{
		uint8 Values[] =
		{
			Corners[0].*Channel,
			Corners[1].*Channel,
			Corners[2].*Channel,
			Corners[3].*Channel
		};
		if (Values[0] > Values[1])
		{
			Swap(Values[0], Values[1]);
		}
		if (Values[2] > Values[3])
		{
			Swap(Values[2], Values[3]);
		}
		if (Values[0] > Values[2])
		{
			Swap(Values[0], Values[2]);
		}
		if (Values[1] > Values[3])
		{
			Swap(Values[1], Values[3]);
		}
		if (Values[1] > Values[2])
		{
			Swap(Values[1], Values[2]);
		}
		return static_cast<uint8>((static_cast<int32>(Values[1]) + Values[2]) / 2);
	};
	return FColor(
		MedianChannel(&FColor::R),
		MedianChannel(&FColor::G),
		MedianChannel(&FColor::B),
		255);
}

bool IsDegenerateCapture(const TArray<FColor>& Pixels, const FColor& ClearColor)
{
	if (Pixels.IsEmpty())
	{
		return true;
	}

	for (const FColor& Pixel : Pixels)
	{
		if (!IsWithinColorEpsilon(Pixel, ClearColor))
		{
			return false;
		}
	}
	return true;
}

int32 DisplayLuminance(const FColor& Pixel)
{
	// Integer Rec. 709 coefficients keep the diagnostic cheap for probe captures.
	return (54 * Pixel.R + 183 * Pixel.G + 19 * Pixel.B + 128) >> 8;
}

FCapturePresentationMetrics MeasureCaptureHighlights(
	const TArray<FColor>& Pixels,
	const int32 Resolution,
	const FColor& ClearColor,
	const TArray<uint8>* GeometryMask = nullptr)
{
	FCapturePresentationMetrics Metrics;
	if (Resolution <= 0 || Pixels.Num() != Resolution * Resolution)
	{
		return Metrics;
	}

	const int32 SampleStep = FMath::Max(
		1,
		FMath::CeilToInt(FMath::Sqrt(
			static_cast<float>(Pixels.Num()) / static_cast<float>(MaxHighlightMetricSamples))));
	int32 TotalSamples = 0;
	int32 ShoulderSamples = 0;
	int32 ClippedSamples = 0;
	int32 LowChromaSamples = 0;
	int32 DetailPairs = 0;
	double LuminanceSum = 0.0;
	double DetailSum = 0.0;
	int32 LuminanceHistogram[256] = {};
	const bool bUseGeometryMask = GeometryMask && GeometryMask->Num() == Pixels.Num();

	for (int32 Y = 0; Y < Resolution; Y += SampleStep)
	{
		for (int32 X = 0; X < Resolution; X += SampleStep)
		{
			++TotalSamples;
			const int32 PixelIndex = Y * Resolution + X;
			const FColor& Pixel = Pixels[PixelIndex];
			if (bUseGeometryMask
				? (*GeometryMask)[PixelIndex] == 0
				: IsWithinColorEpsilon(Pixel, ClearColor))
			{
				continue;
			}

			++Metrics.ForegroundSamples;
			const int32 Luminance = DisplayLuminance(Pixel);
			++LuminanceHistogram[Luminance];
			LuminanceSum += Luminance;
			// Pale materials can lose all useful shading in the display-referred
			// shoulder without producing literal 255 values. Track that broad
			// compression separately from hard clipping.
			if (Luminance >= 224)
			{
				++ShoulderSamples;
			}
			if (Luminance >= 248 && FMath::Max3(Pixel.R, Pixel.G, Pixel.B) >= 253)
			{
				++ClippedSamples;
			}
			const int32 MaxChannel = FMath::Max3(Pixel.R, Pixel.G, Pixel.B);
			const int32 MinChannel = FMath::Min3(Pixel.R, Pixel.G, Pixel.B);
			if (MaxChannel - MinChannel <= 18)
			{
				++LowChromaSamples;
			}

			const auto AccumulateDetail = [&](const int32 OtherX, const int32 OtherY)
			{
				if (OtherX >= Resolution || OtherY >= Resolution)
				{
					return;
				}
				const int32 OtherIndex = OtherY * Resolution + OtherX;
				const FColor& Other = Pixels[OtherIndex];
				const bool bOtherForeground = bUseGeometryMask
					? (*GeometryMask)[OtherIndex] != 0
					: !IsWithinColorEpsilon(Other, ClearColor);
				if (bOtherForeground)
				{
					DetailSum += FMath::Abs(Luminance - DisplayLuminance(Other));
					++DetailPairs;
				}
			};
			AccumulateDetail(X + SampleStep, Y);
			AccumulateDetail(X, Y + SampleStep);
		}
	}

	if (Metrics.ForegroundSamples <= 0 || TotalSamples <= 0)
	{
		return Metrics;
	}

	Metrics.ForegroundFraction = static_cast<float>(Metrics.ForegroundSamples)
		/ static_cast<float>(TotalSamples);
	Metrics.ShoulderFraction = static_cast<float>(ShoulderSamples)
		/ static_cast<float>(Metrics.ForegroundSamples);
	Metrics.ClippedFraction = static_cast<float>(ClippedSamples)
		/ static_cast<float>(Metrics.ForegroundSamples);
	Metrics.LowChromaFraction = static_cast<float>(LowChromaSamples)
		/ static_cast<float>(Metrics.ForegroundSamples);
	Metrics.MeanLuminance = static_cast<float>(LuminanceSum / Metrics.ForegroundSamples);
	Metrics.InteriorDetail = DetailPairs > 0
		? static_cast<float>(DetailSum / DetailPairs)
		: 0.0f;

	const auto FindPercentile = [&](const float Fraction)
	{
		const int32 Target = FMath::Clamp(
			FMath::CeilToInt(Metrics.ForegroundSamples * Fraction),
			1,
			Metrics.ForegroundSamples);
		int32 RunningCount = 0;
		for (int32 Luminance = 0; Luminance < 256; ++Luminance)
		{
			RunningCount += LuminanceHistogram[Luminance];
			if (RunningCount >= Target)
			{
				return Luminance;
			}
		}
		return 255;
	};
	Metrics.P10Luminance = static_cast<float>(FindPercentile(0.10f));
	Metrics.P25Luminance = static_cast<float>(FindPercentile(0.25f));
	Metrics.MedianLuminance = static_cast<float>(FindPercentile(0.50f));
	Metrics.P90Luminance = static_cast<float>(FindPercentile(0.90f));
	Metrics.ToneSpan = Metrics.P90Luminance - static_cast<float>(FindPercentile(0.10f));
	return Metrics;
}

bool ShouldAttemptHighlightRecovery(const FCapturePresentationMetrics& Metrics)
{
	// Colorful subjects and isolated specular spots must not be darkened. Planar
	// artwork is excluded by the caller; this gate targets broadly pale subjects
	// whose median and upper percentile have been compressed into the display
	// shoulder, including cases that never reach literal channel clipping.
	const bool bBroadShoulderCompression = Metrics.ShoulderFraction >= 0.38f
		&& Metrics.MedianLuminance >= 205.0f
		&& Metrics.P90Luminance >= 238.0f;
	const bool bLiteralClipping = Metrics.ClippedFraction >= 0.10f
		&& Metrics.P90Luminance >= 248.0f;
	return Metrics.ForegroundSamples >= 64
		&& Metrics.ForegroundFraction >= 0.02f
		&& Metrics.LowChromaFraction >= 0.60f
		&& (bBroadShoulderCompression || bLiteralClipping);
}

float ComputeHighlightRecoveryExposureBias(const FCapturePresentationMetrics& Metrics)
{
	const float ShoulderSeverity = FMath::Clamp(
		(Metrics.ShoulderFraction - 0.35f) / 0.55f, 0.0f, 1.0f);
	const float MedianSeverity = FMath::Clamp(
		(Metrics.MedianLuminance - 205.0f) / 45.0f, 0.0f, 1.0f);
	const float P90Severity = FMath::Clamp(
		(Metrics.P90Luminance - 238.0f) / 17.0f, 0.0f, 1.0f);
	const float ClipSeverity = FMath::Clamp(Metrics.ClippedFraction / 0.60f, 0.0f, 1.0f);
	const float Severity = FMath::Clamp(
		ShoulderSeverity * 0.35f
			+ MedianSeverity * 0.25f
			+ P90Severity * 0.20f
			+ ClipSeverity * 0.20f,
		0.0f,
		1.0f);
	// A bounded negative EV retains the scene's light ratios and material response;
	// unlike scaling local spotlights, it is independent of object size.
	return FMath::Lerp(-0.75f, -2.0f, Severity);
}

bool PreferHighlightRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery)
{
	if (Recovery.ForegroundSamples < FMath::RoundToInt(Original.ForegroundSamples * 0.80f)
		|| Recovery.MeanLuminance < 56.0f
		|| Recovery.MedianLuminance < 72.0f
		|| Recovery.P90Luminance < 150.0f)
	{
		return false;
	}

	const float RequiredShoulderReduction = FMath::Max(
		0.08f,
		Original.ShoulderFraction * 0.18f);
	const bool bRecoveredHeadroom = Original.P90Luminance - Recovery.P90Luminance >= 6.0f
		&& Original.ShoulderFraction - Recovery.ShoulderFraction >= RequiredShoulderReduction;
	const bool bRecoveredTone = Recovery.ToneSpan >= Original.ToneSpan + 4.0f;
	const bool bRecoveredDetail = Recovery.InteriorDetail >= Original.InteriorDetail + 0.50f
		&& Recovery.InteriorDetail >= Original.InteriorDetail * 1.08f;
	return bRecoveredHeadroom && (bRecoveredTone || bRecoveredDetail);
}

bool ShouldAttemptDarkRecovery(const FCapturePresentationMetrics& Metrics)
{
	return Metrics.ForegroundSamples >= 64
		&& Metrics.ForegroundFraction >= 0.02f
		&& Metrics.MedianLuminance < 48.0f
		&& Metrics.P90Luminance < 112.0f;
}

bool PreferDarkRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery)
{
	if (Recovery.ForegroundSamples < FMath::RoundToInt(Original.ForegroundSamples * 0.80f)
		|| Recovery.ShoulderFraction > 0.24f
		|| Recovery.ClippedFraction > 0.04f)
	{
		return false;
	}

	const bool bMeaningfullyBrighter = Recovery.MedianLuminance >= Original.MedianLuminance + 14.0f
		&& Recovery.P90Luminance >= Original.P90Luminance + 18.0f;
	const bool bRetainsStructure = Recovery.ToneSpan >= FMath::Max(18.0f, Original.ToneSpan * 0.85f)
		&& Recovery.InteriorDetail >= Original.InteriorDetail * 0.80f;
	return bMeaningfullyBrighter && bRetainsStructure;
}

struct FCaptureEvaluation
{
	FColor Backdrop = FColor::Black;
	FCapturePresentationMetrics Metrics;
	bool bDegenerate = true;
};

void LogCaptureEvaluation(
	const TCHAR* Scope,
	const FString& Subject,
	const TCHAR* Phase,
	const FCaptureEvaluation& Evaluation)
{
	UE_LOG(
		LogConvaiSceneAutoTaggerCapture,
		Verbose,
		TEXT("%s '%s' %s: degenerate=%s backdrop=(%u,%u,%u) foreground=%d/%0.3f p10=%0.1f p25=%0.1f median=%0.1f p90=%0.1f span=%0.1f detail=%0.2f lowChroma=%0.3f shoulder=%0.3f clipped=%0.3f"),
		Scope,
		*Subject,
		Phase,
		Evaluation.bDegenerate ? TEXT("true") : TEXT("false"),
		Evaluation.Backdrop.R,
		Evaluation.Backdrop.G,
		Evaluation.Backdrop.B,
		Evaluation.Metrics.ForegroundSamples,
		Evaluation.Metrics.ForegroundFraction,
		Evaluation.Metrics.P10Luminance,
		Evaluation.Metrics.P25Luminance,
		Evaluation.Metrics.MedianLuminance,
		Evaluation.Metrics.P90Luminance,
		Evaluation.Metrics.ToneSpan,
		Evaluation.Metrics.InteriorDetail,
		Evaluation.Metrics.LowChromaFraction,
		Evaluation.Metrics.ShoulderFraction,
		Evaluation.Metrics.ClippedFraction);
}

FCaptureEvaluation EvaluateCapture(
	const TArray<FColor>& Pixels,
	const int32 Resolution,
	const TArray<uint8>* GeometryMask = nullptr)
{
	FCaptureEvaluation Evaluation;
	Evaluation.Backdrop = EstimateEncodedBackdrop(Pixels, Resolution);
	const bool bHasGeometryMask = GeometryMask && GeometryMask->Num() == Pixels.Num();
	const bool bHasGeometry = bHasGeometryMask
		&& GeometryMask->ContainsByPredicate([](const uint8 Value) { return Value != 0; });
	Evaluation.bDegenerate = !bHasGeometry && IsDegenerateCapture(Pixels, Evaluation.Backdrop);
	Evaluation.Metrics = MeasureCaptureHighlights(
		Pixels,
		Resolution,
		Evaluation.Backdrop,
		GeometryMask);
	return Evaluation;
}

bool HasReadableForeground(const FCaptureEvaluation& Evaluation)
{
	return !Evaluation.bDegenerate
		&& Evaluation.Metrics.ForegroundSamples >= 64
		&& Evaluation.Metrics.ForegroundFraction >= 0.02f;
}

bool NeedsReadabilityRecovery(const FCaptureEvaluation& Evaluation)
{
	return !HasReadableForeground(Evaluation)
		|| ShouldAttemptDarkRecovery(Evaluation.Metrics);
}

bool HasUsefulCapturedAppearance(const FCaptureEvaluation& Evaluation)
{
	// Geometry-backed coverage prevents a black material from disappearing into
	// the isolation backdrop, but it must not make a pitch-black image acceptable.
	// Require a small, conservative amount of visible upper-tone information; the
	// existing clipping guards still constrain every recovery candidate.
	return HasReadableForeground(Evaluation)
		&& Evaluation.Metrics.MeanLuminance >= 4.0f
		&& Evaluation.Metrics.P90Luminance >= 32.0f;
}

bool PreferReadabilityRecovery(
	const FCaptureEvaluation& Original,
	const FCaptureEvaluation& Recovery)
{
	if (!HasReadableForeground(Recovery))
	{
		return false;
	}
	// A recovery that merely turns an empty silhouette into a white blob is not a
	// useful analysis image. This guard is intentionally looser than the ordinary
	// dark-recovery comparison because a sparse/empty original has no trustworthy
	// histogram to compare against.
	if (Recovery.Metrics.ShoulderFraction > MaxReadabilityRecoveryShoulderFraction
		|| Recovery.Metrics.ClippedFraction > MaxReadabilityRecoveryClippedFraction
		|| Recovery.Metrics.P90Luminance < 32.0f)
	{
		return false;
	}
	if (!HasReadableForeground(Original))
	{
		return true;
	}
	return PreferDarkRecovery(Original.Metrics, Recovery.Metrics);
}

bool PreferDimensionalKeyRecovery(
	const FCaptureEvaluation& Original,
	const FCaptureEvaluation& Recovery)
{
	if (!PreferReadabilityRecovery(Original, Recovery)
		|| Recovery.Metrics.ShoulderFraction > MaxKeyRecoveryShoulderFraction
		|| Recovery.Metrics.ClippedFraction > MaxKeyRecoveryClippedFraction
		|| Recovery.Metrics.P90Luminance > 242.0f)
	{
		return false;
	}

	if (!HasReadableForeground(Original))
	{
		return Recovery.Metrics.P90Luminance >= 48.0f
			&& Recovery.Metrics.ToneSpan >= 18.0f
			&& Recovery.Metrics.InteriorDetail >= 3.0f;
	}

	return Recovery.Metrics.MedianLuminance >= Original.Metrics.MedianLuminance + 12.0f
		&& Recovery.Metrics.P90Luminance >= Original.Metrics.P90Luminance + 16.0f
		&& Recovery.Metrics.ToneSpan >= FMath::Max(
			18.0f,
			Original.Metrics.ToneSpan * 0.90f)
		&& Recovery.Metrics.InteriorDetail >= FMath::Max(
			Original.Metrics.InteriorDetail + 0.50f,
			Original.Metrics.InteriorDetail * 1.05f);
}

float ResolveWorldAutoExposureBias(UWorld* World, const FVector& ViewLocation)
{
	FPostProcessSettings ResolvedSettings;
	ResolvedSettings.SetBaseValues();
	if (!World)
	{
		return ResolvedSettings.AutoExposureBias;
	}

	// World post-process volumes are already sorted in ascending priority. Mirror
	// the engine's volume-weight calculation for the one property that a recovery
	// probe overrides, so a +1 EV probe remains +1 relative to the authored scene
	// instead of accidentally replacing (and possibly lowering) its exposure bias.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	for (IInterface_PostProcessVolume* VolumePointer : World->PostProcessVolumes)
	{
		if (!VolumePointer) continue;
		IInterface_PostProcessVolume& Volume = *VolumePointer;
#else
	for (IInterface_PostProcessVolume& Volume : World->GetPostProcessVolumeIterator())
	{
#endif
		const FPostProcessVolumeProperties Properties = Volume.GetProperties();
		if (!Properties.bIsEnabled || !Properties.Settings
			|| !Properties.Settings->bOverride_AutoExposureBias)
		{
			continue;
		}

		float LocalWeight = FMath::Clamp(Properties.BlendWeight, 0.0f, 1.0f);
		if (!Properties.bIsUnbound)
		{
			float DistanceToPoint = 0.0f;
			Volume.EncompassesPoint(ViewLocation, 0.0f, &DistanceToPoint);
			if (DistanceToPoint < 0.0f || DistanceToPoint > Properties.BlendRadius)
			{
				LocalWeight = 0.0f;
			}
			else if (Properties.BlendRadius >= 1.0f)
			{
				LocalWeight *= 1.0f - DistanceToPoint / Properties.BlendRadius;
			}
		}

		if (LocalWeight > 0.0f)
		{
			ResolvedSettings.AutoExposureBias = FMath::Lerp(
				ResolvedSettings.AutoExposureBias,
				Properties.Settings->AutoExposureBias,
				LocalWeight);
		}
	}
	return ResolvedSettings.AutoExposureBias;
}

void SetCaptureExposureDelta(
	USceneCaptureComponent2D* CaptureComponent,
	UWorld* World,
	const TOptional<float>& ExposureDelta)
{
	if (!CaptureComponent)
	{
		return;
	}
	if (UseNeutralCaptureLighting(World))
	{
		CaptureComponent->PostProcessSettings.bOverride_AutoExposureBias = true;
		CaptureComponent->PostProcessSettings.AutoExposureBias = ExposureDelta.Get(0.0f);
		return;
	}
	CaptureComponent->PostProcessSettings.bOverride_AutoExposureBias = ExposureDelta.IsSet();
	CaptureComponent->PostProcessSettings.AutoExposureBias = ExposureDelta.IsSet()
		? ResolveWorldAutoExposureBias(World, CaptureComponent->GetComponentLocation())
			+ ExposureDelta.GetValue()
		: 0.0f;
}

bool IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X)
		&& FMath::IsFinite(Value.Y)
		&& FMath::IsFinite(Value.Z);
}

bool TryBuildComponentWorldCorners(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& Components,
	TArray<FVector>& OutWorldCorners)
{
	OutWorldCorners.Reset();
	int32 CapturableComponentCount = 0;
	for (const UPrimitiveComponent* Component : Components)
	{
		if (!IsValid(Component) || !Component->IsRegistered() || Component->GetWorld() != World)
		{
			continue;
		}

		++CapturableComponentCount;
		const FBoxSphereBounds LocalBounds = Component->GetLocalBounds();
		if (!IsFiniteVector(LocalBounds.Origin)
			|| !IsFiniteVector(LocalBounds.BoxExtent)
			|| LocalBounds.BoxExtent.X < 0.0
			|| LocalBounds.BoxExtent.Y < 0.0
			|| LocalBounds.BoxExtent.Z < 0.0)
		{
			OutWorldCorners.Reset();
			return false;
		}

		const FVector LocalMin = LocalBounds.Origin - LocalBounds.BoxExtent;
		const FVector LocalMax = LocalBounds.Origin + LocalBounds.BoxExtent;
		const FTransform& ComponentToWorld = Component->GetComponentTransform();
		for (int32 X = 0; X < 2; ++X)
		{
			for (int32 Y = 0; Y < 2; ++Y)
			{
				for (int32 Z = 0; Z < 2; ++Z)
				{
					const FVector LocalCorner(
						X == 0 ? LocalMin.X : LocalMax.X,
						Y == 0 ? LocalMin.Y : LocalMax.Y,
						Z == 0 ? LocalMin.Z : LocalMax.Z);
					const FVector WorldCorner = ComponentToWorld.TransformPosition(LocalCorner);
					if (!IsFiniteVector(WorldCorner))
					{
						OutWorldCorners.Reset();
						return false;
					}
					OutWorldCorners.Add(WorldCorner);
				}
			}
		}
	}

	return CapturableComponentCount > 0
		&& OutWorldCorners.Num() == CapturableComponentCount * 8;
}

float ComputeFitDistanceForComponents(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& Components,
	const FBox& Bounds,
	const FVector& ViewDirection)
{
	const FVector Center = Bounds.GetCenter();
	const float HalfFovTan = FMath::Tan(FMath::DegreesToRadians(CaptureFovDegrees * 0.5f));
	if (HalfFovTan <= KINDA_SMALL_NUMBER)
	{
		return FMath::Max(Bounds.GetExtent().Size() * 3.0f, 35.0f);
	}

	const FRotationMatrix CameraMatrix(ViewDirection.Rotation());
	const FVector Forward = CameraMatrix.GetUnitAxis(EAxis::X);
	const FVector Right = CameraMatrix.GetUnitAxis(EAxis::Y);
	const FVector Up = CameraMatrix.GetUnitAxis(EAxis::Z);

	float RequiredDistance = 35.0f;
	const auto AccumulateCorner = [&](const FVector& Corner)
	{
		const FVector Offset = Corner - Center;
		const float Lateral = FMath::Max(
			FMath::Abs(FVector::DotProduct(Offset, Right)),
			FMath::Abs(FVector::DotProduct(Offset, Up)));
		const float ForwardOffset = FVector::DotProduct(Offset, Forward);
		RequiredDistance = FMath::Max(
			RequiredDistance,
			Lateral / (HalfFovTan * CaptureFrameFill) - ForwardOffset);
		RequiredDistance = FMath::Max(RequiredDistance, -ForwardOffset + 10.0f);
	};

	// A union world-space AABB can become substantially larger than the actual
	// actor after rotated components are combined. Prefer each component's local
	// bounds transformed into world space, but fall back atomically when any
	// capturable component cannot provide trustworthy finite corners.
	TArray<FVector> ComponentWorldCorners;
	if (TryBuildComponentWorldCorners(World, Components, ComponentWorldCorners))
	{
		for (const FVector& Corner : ComponentWorldCorners)
		{
			AccumulateCorner(Corner);
		}
	}
	else
	{
		for (int32 X = 0; X < 2; ++X)
		{
			for (int32 Y = 0; Y < 2; ++Y)
			{
				for (int32 Z = 0; Z < 2; ++Z)
				{
					AccumulateCorner(FVector(
						X == 0 ? Bounds.Min.X : Bounds.Max.X,
						Y == 0 ? Bounds.Min.Y : Bounds.Max.Y,
						Z == 0 ? Bounds.Min.Z : Bounds.Max.Z));
				}
			}
		}
	}

	return FMath::Max(RequiredDistance, Bounds.GetExtent().Size() * 0.5f);
}

void ConfigureEvidenceShowFlags(
	USceneCaptureComponent2D* CaptureComponent,
	const bool bUseLumenGlobalIllumination,
	const bool bUseLumenReflections)
{
	FEngineShowFlags& ShowFlags = CaptureComponent->ShowFlags;
	// Geometry isolation does not isolate illumination. Disable source-world
	// environment paths in neutral mode; the component-local extension removes
	// inherited post-process volumes after Unreal has blended them.
	ShowFlags.SetAtmosphere(false);
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	// Earlier engines suppress this pass through the Atmosphere flag above.
	ShowFlags.SetDeferredAtmospherePass(false);
#endif
	ShowFlags.SetFog(false);
	ShowFlags.SetVolumetricFog(false);
	ShowFlags.SetCloud(false);
	ShowFlags.SetSkyLighting(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	// Neutral mode installs only its generated studio cubemap after volume blend.
	ShowFlags.SetAmbientCubemap(true);
	ShowFlags.SetLightShafts(false);
	ShowFlags.SetLensFlares(false);
	ShowFlags.SetLensDistortion(false);
	ShowFlags.SetBloom(false);
	ShowFlags.SetEyeAdaptation(true);
	ShowFlags.SetLocalExposure(false);
	ShowFlags.SetTemporalAA(false);
	ShowFlags.SetMotionBlur(false);
	ShowFlags.SetVignette(false);
	ShowFlags.SetDepthOfField(false);
	ShowFlags.SetGrain(false);
	ShowFlags.SetCameraImperfections(false);
	ShowFlags.SetSceneColorFringe(false);
	ShowFlags.SetColorGrading(true);
	ShowFlags.SetReflectionEnvironment(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	ShowFlags.SetScreenSpaceReflections(false);
	ShowFlags.SetGlobalIllumination(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	ShowFlags.SetIndirectLightingCache(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	ShowFlags.SetVolumetricLightmap(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	ShowFlags.SetAmbientOcclusion(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	ShowFlags.SetDistanceFieldAO(!UseNeutralCaptureLighting(CaptureComponent->GetWorld()));
	ShowFlags.SetLumenGlobalIllumination(bUseLumenGlobalIllumination);
	ShowFlags.SetLumenReflections(bUseLumenReflections);
	ShowFlags.SetPostProcessMaterial(false);
}

struct FCapturePlacement
{
	FVector ViewDirection = FVector::ZeroVector;
	FVector Center = FVector::ZeroVector;
	FVector CameraLocation = FVector::ZeroVector;
	float Radius = 1.0f;
	float Distance = 1.0f;
};

FBox ResolveCurrentWorldBounds(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const FBox& FallbackBounds)
{
	FBox CurrentBounds(ForceInit);
	int32 CapturableComponentCount = 0;
	for (const UPrimitiveComponent* Component : ObjectComponents)
	{
		if (!IsValid(Component) || !Component->IsRegistered() || Component->GetWorld() != World)
		{
			continue;
		}
		++CapturableComponentCount;
		const FBox ComponentBounds = Component->Bounds.GetBox();
		if (!ComponentBounds.IsValid
			|| !IsFiniteVector(ComponentBounds.Min)
			|| !IsFiniteVector(ComponentBounds.Max))
		{
			// Do not silently crop a multipart actor because one current component
			// returned unusable bounds. The discovery snapshot remains a safe fallback.
			return FallbackBounds;
		}
		CurrentBounds += ComponentBounds;
	}
	return CapturableComponentCount > 0 && CurrentBounds.IsValid
		? CurrentBounds
		: FallbackBounds;
}

FCapturePlacement BuildCapturePlacement(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const FBox& WorldBounds,
	const FVector& ViewDirection,
	const float DistanceScale,
	const FVector2D& TargetOffset)
{
	FCapturePlacement Placement;
	const FBox FramingBounds = ResolveCurrentWorldBounds(
		World,
		ObjectComponents,
		WorldBounds);
	Placement.ViewDirection = ViewDirection.GetSafeNormal();
	if (Placement.ViewDirection.IsNearlyZero())
	{
		Placement.ViewDirection = FVector(-1.0, -1.0, -0.65).GetSafeNormal();
	}

	const FVector BoundsCenter = FramingBounds.GetCenter();
	Placement.Radius = FMath::Max(FramingBounds.GetExtent().Size(), 1.0f);
	FVector CameraRight = FVector::CrossProduct(
		FVector::UpVector,
		Placement.ViewDirection).GetSafeNormal();
	if (CameraRight.IsNearlyZero())
	{
		CameraRight = FVector::RightVector;
	}
	const FVector CameraUp = FVector::CrossProduct(
		Placement.ViewDirection,
		CameraRight).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
	Placement.Center = BoundsCenter
		+ CameraRight * FMath::Clamp(TargetOffset.X, -0.75, 0.75) * Placement.Radius
		+ CameraUp * FMath::Clamp(TargetOffset.Y, -0.75, 0.75) * Placement.Radius;
	Placement.Distance = ComputeFitDistanceForComponents(
		World,
		ObjectComponents,
		FramingBounds,
		Placement.ViewDirection)
		* FMath::Clamp(DistanceScale, 0.55f, 2.5f);
	Placement.CameraLocation = Placement.Center
		- Placement.ViewDirection * Placement.Distance;
	return Placement;
}

void ConfigureCaptureComponent(
	USceneCaptureComponent2D* CaptureComponent,
	UTextureRenderTarget2D* RenderTarget,
	const float MaxViewDistance,
	const bool bSuppressDirectSpecularHighlights)
{
	check(CaptureComponent);
	CaptureComponent->TextureTarget = RenderTarget;
	CaptureComponent->FOVAngle = CaptureFovDegrees;
	CaptureComponent->bCaptureOnMovement = false;
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	// Older engines have no per-capture override; retain their bounded streaming warm-up.
	CaptureComponent->bOverrideVirtualTextureThrottle = true;
#endif
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureComponent->MaxViewDistanceOverride = MaxViewDistance;
	CaptureComponent->bCameraCutThisFrame = true;
	SetSceneAndFallbackViewLightingChannels(CaptureComponent->ViewLightingChannels);
	CaptureComponent->PostProcessSettings.WeightedBlendables.Array.Reset();
	// The legacy comparison inherits world post-processing here. Neutral captures
	// reset the final merged settings in their component-local view extension.
	CaptureComponent->PostProcessSettings.bOverride_AutoExposureMethod = false;
	CaptureComponent->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = false;
	CaptureComponent->PostProcessSettings.bOverride_AutoExposureBias = false;
	CaptureComponent->PostProcessSettings.AutoExposureBias = 0.0f;
	CaptureComponent->PostProcessSettings.bOverride_ColorContrast = false;
	const URendererSettings* RendererSettings = GetDefault<URendererSettings>();
	const bool bUseLumenGlobalIllumination = !UseNeutralCaptureLighting(CaptureComponent->GetWorld()) && RendererSettings
		&& RendererSettings->DynamicGlobalIllumination == EDynamicGlobalIlluminationMethod::Lumen;
	const bool bUseLumenReflections = !UseNeutralCaptureLighting(CaptureComponent->GetWorld()) && !bSuppressDirectSpecularHighlights
		&& RendererSettings
		&& RendererSettings->Reflections == EReflectionMethod::Lumen;
	// UE scene captures otherwise force their final post-process GI/reflection
	// methods to None. Explicitly retain the project's Lumen choice, while keeping
	// screen-space/plugin methods off because an isolated camera cannot reproduce
	// their full-scene inputs reliably.
	CaptureComponent->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
	CaptureComponent->PostProcessSettings.DynamicGlobalIlluminationMethod =
		bUseLumenGlobalIllumination
			? EDynamicGlobalIlluminationMethod::Lumen
			: EDynamicGlobalIlluminationMethod::None;
	CaptureComponent->PostProcessSettings.bOverride_ReflectionMethod = true;
	CaptureComponent->PostProcessSettings.ReflectionMethod = bUseLumenReflections
		? EReflectionMethod::Lumen
		: EReflectionMethod::None;
	CaptureComponent->PostProcessSettings.bOverride_BloomIntensity = true;
	CaptureComponent->PostProcessSettings.BloomIntensity = 0.0f;
	CaptureComponent->PostProcessSettings.bOverride_MotionBlurAmount = true;
	CaptureComponent->PostProcessSettings.MotionBlurAmount = 0.0f;
	CaptureComponent->PostProcessSettings.bOverride_VignetteIntensity = true;
	CaptureComponent->PostProcessSettings.VignetteIntensity = 0.0f;
	ConfigureEvidenceShowFlags(
		CaptureComponent,
		bUseLumenGlobalIllumination,
		bUseLumenReflections);
	CaptureComponent->ShowFlags.SetSpecular(!bSuppressDirectSpecularHighlights);
}

void SetCaptureShowOnlyComponents(
	USceneCaptureComponent2D* CaptureComponent,
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents)
{
	CaptureComponent->ShowOnlyComponents.Reset();
	for (UPrimitiveComponent* Component : ObjectComponents)
	{
		if (IsValid(Component) && Component->IsRegistered() && Component->GetWorld() == World)
		{
			CaptureComponent->ShowOnlyComponent(Component);
		}
	}
}

bool CapturePixelsNow(
	USceneCaptureComponent2D* CaptureComponent,
	UTextureRenderTarget2D* RenderTarget,
	const int32 Resolution,
	TArray<FColor>& OutPixels)
{
	OutPixels.Reset();
	if (!CaptureComponent || !RenderTarget || CaptureComponent->bCaptureEveryFrame)
	{
		return false;
	}

	CaptureComponent->CaptureScene();
	FTextureRenderTargetResource* RenderResource = RenderTarget->GameThread_GetRenderTargetResource();
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
	// The sRGB render target makes FinalColorLDR produce display-ready bytes.
	// Preserve those exact bytes for the retained preview and model PNG.
	ReadFlags.SetLinearToGamma(false);
	if (!RenderResource
		|| !RenderResource->ReadPixels(OutPixels, ReadFlags)
		|| OutPixels.Num() != Resolution * Resolution)
	{
		OutPixels.Reset();
		return false;
	}
	for (FColor& Pixel : OutPixels)
	{
		Pixel.A = 255;
	}
	return true;
}

bool CaptureSettledProfilePixels(
	USceneCaptureComponent2D* CaptureComponent,
	UTextureRenderTarget2D* RenderTarget,
	const int32 Resolution,
	TArray<FColor>& OutPixels)
{
	OutPixels.Reset();
	if (!CaptureComponent || !RenderTarget)
	{
		return false;
	}

	// A scene-capture camera cut forces histogram eye adaptation to its current
	// target. Re-arm it for every view/light/EV generation, discard that first
	// render, and compare only the following identical-profile render. Without
	// this reset the reusable sweeper lets the previous object or recovery profile
	// decide the exposure of the next angle.
	CaptureComponent->bCameraCutThisFrame = true;
	TArray<FColor> DiscardedPixels;
	for (int32 FrameIndex = 0; FrameIndex < CaptureProfileSettleFrameCount; ++FrameIndex)
	{
		TArray<FColor>& Destination = FrameIndex + 1 == CaptureProfileSettleFrameCount
			? OutPixels
			: DiscardedPixels;
		if (!CapturePixelsNow(CaptureComponent, RenderTarget, Resolution, Destination))
		{
			OutPixels.Reset();
			return false;
		}
	}
	return true;
}

bool CaptureGeometryMaskNow(
	USceneCaptureComponent2D* CaptureComponent,
	const int32 Resolution,
	TArray<uint8>& OutMask)
{
	OutMask.Reset();
	if (!CaptureComponent || Resolution <= 0 || CaptureComponent->bCaptureEveryFrame)
	{
		return false;
	}
	UTextureRenderTarget2D* MaskTarget = GetReusableGeometryMaskRenderTarget(Resolution);
	if (!MaskTarget)
	{
		return false;
	}

	UTextureRenderTarget2D* OriginalTarget = CaptureComponent->TextureTarget;
	const ESceneCaptureSource OriginalSource = CaptureComponent->CaptureSource;
	ON_SCOPE_EXIT
	{
		CaptureComponent->TextureTarget = OriginalTarget;
		CaptureComponent->CaptureSource = OriginalSource;
		// The next color render starts a fresh temporal generation after the depth-only pass.
		CaptureComponent->bCameraCutThisFrame = true;
	};

	CaptureComponent->TextureTarget = MaskTarget;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_DeviceDepth;
	CaptureComponent->bCameraCutThisFrame = true;
	TArray<FColor> EncodedDepth;
	if (!CapturePixelsNow(CaptureComponent, MaskTarget, Resolution, EncodedDepth))
	{
		return false;
	}

	const FColor EncodedBackground = EstimateEncodedBackdrop(EncodedDepth, Resolution);
	OutMask.SetNumUninitialized(EncodedDepth.Num());
	int32 GeometryPixels = 0;
	for (int32 Index = 0; Index < EncodedDepth.Num(); ++Index)
	{
		// Device-depth background is constant. Use a tight comparison rather than
		// IsCaptureBackgroundPixel: neutral near-black depth bytes can legitimately
		// belong to geometry close to the camera.
		const FColor& Pixel = EncodedDepth[Index];
		const bool bMatchesDepthBackground =
			FMath::Abs(static_cast<int32>(Pixel.R) - static_cast<int32>(EncodedBackground.R)) <= 1
			&& FMath::Abs(static_cast<int32>(Pixel.G) - static_cast<int32>(EncodedBackground.G)) <= 1
			&& FMath::Abs(static_cast<int32>(Pixel.B) - static_cast<int32>(EncodedBackground.B)) <= 1;
		const bool bGeometry = !bMatchesDepthBackground;
		OutMask[Index] = bGeometry ? 1 : 0;
		GeometryPixels += bGeometry ? 1 : 0;
	}
	if (GeometryPixels <= 0)
	{
		OutMask.Reset();
		return false;
	}
	return true;
}

}

FColor GetCaptureBackgroundColor()
{
	return CaptureBackground.ToFColor(false);
}

bool ShouldContinueAutomaticCaptureWarmup(
	const int32 DistinctFrames,
	const double ElapsedSeconds,
	const int32 PendingConventionalAssets)
{
	constexpr int32 MinimumFrames = 5;
	// The final prestream request remains valid for three seconds. Keep the
	// winning view alive for that same bounded window so large authored textures
	// are not retained from a temporary low mip on slower editor machines.
	constexpr int32 MaximumFrames = 60;
	constexpr double MaximumSeconds = 3.0;
	return DistinctFrames < MinimumFrames
		|| (PendingConventionalAssets > 0
			&& DistinctFrames < MaximumFrames
			&& ElapsedSeconds < MaximumSeconds);
}

bool ShouldContinueAutomaticProbeWarmup(
	const int32 DistinctFrames,
	const double ElapsedSeconds,
	const int32 PendingRenderWork)
{
	// A reported-resident texture can still be installing view-dependent VT/Nanite
	// data after the priming draw. Under a full-level sweep, two editor frames and
	// 50 ms proved insufficient even though the asset counters were already clear.
	constexpr int32 MinimumFrames = 3;
	constexpr int32 MaximumFrames = 12;
	constexpr double MinimumSeconds = 0.10;
	constexpr double MaximumSeconds = 0.75;
	return DistinctFrames < MinimumFrames
		|| ElapsedSeconds < MinimumSeconds
		|| (PendingRenderWork > 0
			&& DistinctFrames < MaximumFrames
			&& ElapsedSeconds < MaximumSeconds);
}

bool AdvanceAutomaticProbePass(
	const int32 ProbeCount,
	int32& InOutProbeCursor,
	bool& bInOutSeedingPass)
{
	if (ProbeCount <= 0)
	{
		InOutProbeCursor = 0;
		bInOutSeedingPass = false;
		return true;
	}

	++InOutProbeCursor;
	if (InOutProbeCursor < ProbeCount)
	{
		return false;
	}

	InOutProbeCursor = 0;
	if (bInOutSeedingPass)
	{
		bInOutSeedingPass = false;
		return false;
	}
	return true;
}

bool IsConventionalCaptureAssetReady(
	const bool bHasPendingUpdate,
	const bool bFullyStreamedIn)
{
	return !bHasPendingUpdate && bFullyStreamedIn;
}

bool HasAutomaticCaptureSettledAfterCalibration(const int32 DistinctFrames)
{
	return DistinctFrames >= 2;
}

bool PreferSevereDarkDimensionalKeyRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery,
	const bool bSuppressDirectSpecularHighlights,
	const bool bAllowSuppressedSpecularRecovery)
{
	// This is deliberately key-only. Loosening the generic dark comparison would
	// also change EV-only and diffuse-fill behavior for authored paintings. A
	// non-metallic broad face therefore remains strictly specular-suppressed.
	if (bSuppressDirectSpecularHighlights && !bAllowSuppressedSpecularRecovery)
	{
		return false;
	}

	const bool bPinnedSevereDark = Original.ForegroundSamples >= 64
		&& Original.ForegroundFraction >= 0.02f
		&& Original.P25Luminance <= 20.0f
		&& Original.MedianLuminance <= 24.0f
		&& Original.P90Luminance < 112.0f;
	const bool bSafeCoverage = Recovery.ForegroundSamples >= 64
		&& Recovery.ForegroundSamples >= FMath::RoundToInt(
			Original.ForegroundSamples * 0.80f)
		&& Recovery.ForegroundFraction >= Original.ForegroundFraction * 0.80f;
	const bool bNoLowerToneRegression = Recovery.P25Luminance
			>= Original.P25Luminance - 2.0f
		&& Recovery.MedianLuminance >= Original.MedianLuminance - 2.0f;
	const bool bSafeHighlights = Recovery.P90Luminance <= 220.0f
		&& Recovery.ShoulderFraction <= 0.12f
		&& Recovery.ClippedFraction <= 0.01f;
	const bool bUpperStructureRevealed = Recovery.P90Luminance >= FMath::Max(
			96.0f,
			Original.P90Luminance + 32.0f)
		&& Recovery.ToneSpan >= FMath::Max(
			64.0f,
			Original.ToneSpan + 24.0f)
		&& Recovery.InteriorDetail >= FMath::Max(
			5.0f,
			FMath::Max(
				Original.InteriorDetail + 1.0f,
				Original.InteriorDetail * 1.10f));
	return bPinnedSevereDark
		&& bSafeCoverage
		&& bNoLowerToneRegression
		&& bSafeHighlights
		&& bUpperStructureRevealed;
}

bool ShouldAttemptOpposingDiffuseRecovery(
	const FCapturePresentationMetrics& Metrics,
	const bool bSingleDiffuseFillRetained,
	const bool bSuppressDirectSpecularHighlights)
{
	// This probe is presentation refinement, not a replacement lighting rig. It
	// only follows an accepted single-fill recovery on dimensional subjects whose
	// lower quarter remains dark while the upper range still has safe headroom.
	return bSingleDiffuseFillRetained
		&& !bSuppressDirectSpecularHighlights
		&& Metrics.ForegroundSamples >= 64
		&& Metrics.ForegroundFraction >= 0.02f
		&& Metrics.MedianLuminance >= 80.0f
		&& Metrics.MedianLuminance <= 190.0f
		&& Metrics.P90Luminance >= 130.0f
		&& Metrics.P90Luminance <= 220.0f
		&& Metrics.P25Luminance <= 90.0f
		&& Metrics.MedianLuminance - Metrics.P25Luminance >= 40.0f
		&& Metrics.ShoulderFraction <= 0.18f
		&& Metrics.ClippedFraction <= 0.015f;
}

bool PreferOpposingDiffuseRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery)
{
	if (Recovery.ForegroundSamples < FMath::RoundToInt(Original.ForegroundSamples * 0.90f)
		|| Recovery.ForegroundFraction < Original.ForegroundFraction * 0.90f)
	{
		return false;
	}

	const float OriginalShadowGap = Original.MedianLuminance - Original.P25Luminance;
	const float RecoveryShadowGap = Recovery.MedianLuminance - Recovery.P25Luminance;
	const bool bShadowLifted = Recovery.P25Luminance >= Original.P25Luminance + 12.0f
		&& RecoveryShadowGap <= OriginalShadowGap * 0.90f;
	const bool bHighlightsBounded = Recovery.MedianLuminance >= Original.MedianLuminance - 4.0f
		&& Recovery.MedianLuminance <= Original.MedianLuminance + 32.0f
		&& Recovery.P90Luminance <= FMath::Min(224.0f, Original.P90Luminance + 20.0f)
		&& Recovery.ShoulderFraction <= 0.18f
		&& Recovery.ClippedFraction <= 0.015f;
	const bool bStructureRetained = Recovery.ToneSpan >= FMath::Max(
		48.0f,
		Original.ToneSpan * 0.72f)
		&& Recovery.InteriorDetail >= FMath::Max(
			3.0f,
			Original.InteriorDetail * 0.82f);
	return bShadowLifted && bHighlightsBounded && bStructureRetained;
}

bool ShouldAttemptDimensionalShadowBalanceRecovery(
	const FCapturePresentationMetrics& Metrics,
	const bool bSuppressDirectSpecularHighlights)
{
	// This is intentionally separate from severe-dark recovery. The subject is
	// already visible in its highlights; the missing information is confined to
	// a broad shadow range. Planar captures remain on the specular-free path.
	const bool bExistingHighlightRange = Metrics.P90Luminance >= 112.0f;
	const bool bDarkBoundaryRange = Metrics.P90Luminance >= 96.0f
		&& Metrics.P25Luminance <= 32.0f
		&& Metrics.MedianLuminance < 52.0f;
	return !bSuppressDirectSpecularHighlights
		&& Metrics.ForegroundSamples >= 64
		&& Metrics.ForegroundFraction >= 0.02f
		&& Metrics.P25Luminance <= 64.0f
		&& Metrics.MedianLuminance <= 100.0f
		// Straddle the ordinary severe-dark boundary only below the absolute
		// readability floor. Existing readable captures retain the prior >=112 gate.
		&& (bExistingHighlightRange || bDarkBoundaryRange)
		&& Metrics.P90Luminance <= 220.0f
		&& Metrics.P90Luminance - Metrics.P25Luminance >= 64.0f
		&& (Metrics.MedianLuminance - Metrics.P25Luminance >= 24.0f
			|| Metrics.P90Luminance - Metrics.MedianLuminance >= 48.0f)
		&& Metrics.ShoulderFraction <= 0.12f
		&& Metrics.ClippedFraction <= 0.01f;
}

bool PreferDimensionalShadowBalanceRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery)
{
	const bool bSafeCoverage = Recovery.ForegroundSamples >= 64
		&& Recovery.ForegroundSamples >= FMath::RoundToInt(
			Original.ForegroundSamples * 0.90f)
		&& Recovery.ForegroundFraction >= Original.ForegroundFraction * 0.90f;
	const float P25Lift = Recovery.P25Luminance - Original.P25Luminance;
	const float MedianLift = Recovery.MedianLuminance - Original.MedianLuminance;
	const float HighlightLift = Recovery.P90Luminance - Original.P90Luminance;
	const bool bBalancedShadowLift = P25Lift >= 8.0f
		&& MedianLift >= 4.0f
		&& P25Lift >= FMath::Max(4.0f, HighlightLift * 0.35f);
	// Rough dark metals sometimes keep the bottom histogram bucket pinned to the
	// isolation backdrop. In that case a bounded midtone lift still reveals real
	// surface information, provided the lower quarter does not regress.
	const bool bDarkMetalMidtoneLift = Original.P25Luminance <= 24.0f
		&& Original.MedianLuminance <= 60.0f
		&& P25Lift >= -2.0f
		&& MedianLift >= 10.0f
		&& HighlightLift >= 6.0f
		&& HighlightLift <= 32.0f;
	const bool bSafeHighlights = MedianLift <= 24.0f
		&& Recovery.P90Luminance <= FMath::Min(
			220.0f,
			Original.P90Luminance + 32.0f)
		&& Recovery.ShoulderFraction <= 0.10f
		&& Recovery.ClippedFraction <= 0.01f;
	const bool bStructureRetained = Recovery.ToneSpan >= FMath::Max(
		48.0f,
		Original.ToneSpan * 0.75f)
		&& Recovery.InteriorDetail >= FMath::Max(
			3.0f,
			Original.InteriorDetail * 0.88f);
	return bSafeCoverage
		&& (bBalancedShadowLift || bDarkMetalMidtoneLift)
		&& bSafeHighlights
		&& bStructureRetained;
}

bool ShouldAttemptDimensionalReadabilityFloorRecovery(
	const FCapturePresentationMetrics& Metrics,
	const bool bSuppressDirectSpecularHighlights)
{
	// This is an absolute floor, independent of the older P90-based dark split.
	// A few highlights cannot make a subject readable when its lower quarter and
	// median remain pinned near black. Planar/specular-suppressed evidence never
	// enters this dimensional lighting path.
	return !bSuppressDirectSpecularHighlights
		&& Metrics.ForegroundSamples >= 64
		&& Metrics.ForegroundFraction >= 0.02f
		&& Metrics.P25Luminance <= 32.0f
		&& Metrics.MedianLuminance < 52.0f
		&& Metrics.P90Luminance <= 220.0f
		&& Metrics.ShoulderFraction <= 0.10f
		&& Metrics.ClippedFraction <= 0.01f;
}

bool PreferDimensionalReadabilityFloorRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery)
{
	const bool bOriginalBelowFloor = Original.ForegroundSamples >= 64
		&& Original.ForegroundFraction >= 0.02f
		&& Original.P25Luminance <= 32.0f
		&& Original.MedianLuminance < 52.0f;
	const bool bSafeCoverage = Recovery.ForegroundSamples >= 64
		&& Recovery.ForegroundSamples >= FMath::RoundToInt(
			Original.ForegroundSamples * 0.90f)
		&& Recovery.ForegroundFraction >= Original.ForegroundFraction * 0.90f;
	const bool bMeaningfulLift = Recovery.P25Luminance >= Original.P25Luminance - 2.0f
		&& Recovery.MedianLuminance >= Original.MedianLuminance + 8.0f;
	const bool bSafeHighlights = Recovery.P90Luminance <= 220.0f
		&& Recovery.ShoulderFraction <= 0.10f
		&& Recovery.ClippedFraction <= 0.01f;
	const bool bStructureRetained = Recovery.ToneSpan >= FMath::Max(
		36.0f,
		Original.ToneSpan * 0.85f)
		&& Recovery.InteriorDetail >= FMath::Max(
			3.0f,
			Original.InteriorDetail * 0.85f);
	return bOriginalBelowFloor
		&& bSafeCoverage
		&& bMeaningfulLift
		&& bSafeHighlights
		&& bStructureRetained;
}

bool ShouldAttemptPlanarDiffuseExposureRecovery(
	const FCapturePresentationMetrics& Metrics,
	const bool bSuppressDirectSpecularHighlights,
	const bool bAllowSuppressedSpecularRecovery)
{
	// A front-facing painting has no useful "shadow side" for an opposing light to
	// reveal. Instead, use the same zero-specular diffuse fill and add only as much
	// exposure as the measured highlight range can safely tolerate. Explicitly
	// metallic panels stay on their material-gated recovery path.
	return bSuppressDirectSpecularHighlights
		&& !bAllowSuppressedSpecularRecovery
		&& Metrics.ForegroundSamples >= 64
		&& Metrics.ForegroundFraction >= 0.02f
		&& Metrics.P25Luminance < 52.0f
		&& Metrics.MedianLuminance < 88.0f
		&& Metrics.P90Luminance < 214.0f
		&& Metrics.ShoulderFraction <= 0.16f
		&& Metrics.ClippedFraction <= 0.01f;
}

float ComputePlanarDiffuseExposureBias(const FCapturePresentationMetrics& Metrics)
{
	// Leave approximately 35 code values of highlight headroom. A minimum half-stop
	// makes the probe meaningful for dark authored panels; the acceptance gate below
	// still rejects it if the real render response clips or flattens the image.
	const float SafeP90 = FMath::Max(64.0f, Metrics.P90Luminance);
	const float HeadroomStops = FMath::Log2(220.0f / SafeP90);
	return FMath::Clamp(HeadroomStops, 0.50f, 1.0f);
}

bool PreferPlanarDiffuseExposureRecovery(
	const FCapturePresentationMetrics& Original,
	const FCapturePresentationMetrics& Recovery)
{
	if (Recovery.ForegroundSamples < FMath::RoundToInt(Original.ForegroundSamples * 0.80f)
		|| Recovery.ForegroundFraction < Original.ForegroundFraction * 0.80f
		|| Recovery.P90Luminance > 224.0f
		|| Recovery.MedianLuminance > 170.0f
		|| Recovery.ShoulderFraction > 0.18f
		|| Recovery.ClippedFraction > 0.015f)
	{
		return false;
	}

	const bool bMedianLifted = Recovery.MedianLuminance >= Original.MedianLuminance + 10.0f;
	const bool bUpperTonesPreserved = Recovery.P90Luminance >= Original.P90Luminance - 4.0f;
	const bool bLowerQuartileLifted = Recovery.P25Luminance >= Original.P25Luminance + 8.0f;
	// Very dark authored pixels can sit close enough to the black isolation
	// backdrop that the foreground mask under-counts them. Accept a smaller P25
	// rise only when both the median and upper authored tones make a decisive
	// measured gain; the clipping and structure gates below still apply.
	const bool bStrongDistributionLift = Recovery.P25Luminance >= Original.P25Luminance + 4.0f
		&& Recovery.MedianLuminance >= Original.MedianLuminance + 12.0f
		&& Recovery.P90Luminance >= Original.P90Luminance + 20.0f;
	const bool bLowerTonesLifted = bMedianLifted
		&& bUpperTonesPreserved
		&& (bLowerQuartileLifted || bStrongDistributionLift);
	const bool bStructureRetained = Recovery.ToneSpan >= FMath::Max(
		24.0f,
		Original.ToneSpan * 0.70f)
		&& Recovery.InteriorDetail >= FMath::Max(
			1.5f,
			Original.InteriorDetail * 0.75f);
	return bLowerTonesLifted && bStructureRetained;
}

bool PrepareViewportContextPng(
	const TArray<FColor>& SourceBGRA,
	const int32 SourceWidth,
	const int32 SourceHeight,
	const int32 MaximumLongEdge,
	TArray64<uint8>& OutPngBytes,
	FString& OutError)
{
	OutPngBytes.Reset();
	OutError.Reset();
	const int64 ExpectedPixelCount = static_cast<int64>(SourceWidth) * SourceHeight;
	if (SourceWidth < 64 || SourceHeight < 64
		|| SourceWidth > 16384 || SourceHeight > 16384
		|| MaximumLongEdge < 256 || MaximumLongEdge > 2048
		|| ExpectedPixelCount <= 0 || ExpectedPixelCount > 64ll * 1024ll * 1024ll
		|| SourceBGRA.Num() != ExpectedPixelCount)
	{
		OutError = TEXT("The active Level Editor viewport has an unsupported capture size.");
		return false;
	}

	FColor Minimum(255, 255, 255, 255);
	FColor Maximum(0, 0, 0, 255);
	const int32 SampleStride = FMath::Max(1, SourceBGRA.Num() / 16384);
	for (int32 Index = 0; Index < SourceBGRA.Num(); Index += SampleStride)
	{
		const FColor Pixel = SourceBGRA[Index];
		Minimum.R = FMath::Min(Minimum.R, Pixel.R);
		Minimum.G = FMath::Min(Minimum.G, Pixel.G);
		Minimum.B = FMath::Min(Minimum.B, Pixel.B);
		Maximum.R = FMath::Max(Maximum.R, Pixel.R);
		Maximum.G = FMath::Max(Maximum.G, Pixel.G);
		Maximum.B = FMath::Max(Maximum.B, Pixel.B);
	}
	if (Maximum.R - Minimum.R <= 2
		&& Maximum.G - Minimum.G <= 2
		&& Maximum.B - Minimum.B <= 2)
	{
		OutError = TEXT("The active Level Editor viewport appears blank or flat. Frame the scene and try again.");
		return false;
	}

	const float Scale = FMath::Min(
		1.0f,
		static_cast<float>(MaximumLongEdge) / static_cast<float>(FMath::Max(SourceWidth, SourceHeight)));
	const int32 OutputWidth = FMath::Max(1, FMath::RoundToInt(SourceWidth * Scale));
	const int32 OutputHeight = FMath::Max(1, FMath::RoundToInt(SourceHeight * Scale));
	TArray<FColor> PreparedPixels;
	if (OutputWidth == SourceWidth && OutputHeight == SourceHeight)
	{
		PreparedPixels = SourceBGRA;
	}
	else
	{
		FImage SourceImage(SourceWidth, SourceHeight, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		if (SourceImage.RawData.Num() != ExpectedPixelCount * static_cast<int64>(sizeof(FColor)))
		{
			OutError = TEXT("Could not allocate the viewport image resize buffer.");
			return false;
		}
		FMemory::Memcpy(SourceImage.RawData.GetData(), SourceBGRA.GetData(), SourceImage.RawData.Num());
		FImage OutputImage(OutputWidth, OutputHeight, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FImageCore::ResizeImage(
			SourceImage,
			OutputImage,
			FImageCore::EResizeImageFilter::AdaptiveSmooth);
		PreparedPixels.SetNumUninitialized(OutputWidth * OutputHeight);
		if (OutputImage.RawData.Num() != static_cast<int64>(PreparedPixels.Num()) * sizeof(FColor))
		{
			OutError = TEXT("Viewport image resizing returned an invalid pixel buffer.");
			return false;
		}
		FMemory::Memcpy(PreparedPixels.GetData(), OutputImage.RawData.GetData(), OutputImage.RawData.Num());
	}
	for (FColor& Pixel : PreparedPixels)
	{
		Pixel.A = 255;
	}
	return EncodePng(PreparedPixels, OutputWidth, OutputHeight, OutPngBytes, OutError);
}

bool CaptureActiveLevelViewportPng(
	TArray64<uint8>& OutPngBytes,
	FString& OutError,
	const int32 MaximumLongEdge)
{
#if WITH_EDITOR
	OutPngBytes.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("The Level Editor viewport can only be captured from the editor thread.");
		return false;
	}

	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	const TSharedPtr<SLevelViewport> LevelViewport = LevelEditorModule
		? LevelEditorModule->GetFirstActiveLevelViewport()
		: nullptr;
	if (!LevelViewport.IsValid() || !LevelViewport->GetWorld())
	{
		OutError = TEXT("Open and activate a Level Editor viewport first.");
		return false;
	}
	if (LevelViewport->HasPlayInEditorViewport())
	{
		OutError = TEXT("Stop the embedded Play session before drafting scene context from the editor viewport.");
		return false;
	}

	FViewport* Viewport = LevelViewport->GetActiveViewport();
	if (!Viewport)
	{
		OutError = TEXT("The active Level Editor viewport is not ready for capture.");
		return false;
	}
	const FIntPoint ViewportSize = Viewport->GetSizeXY();
	TArray<FColor> Pixels;
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0 || !Viewport->ReadPixels(Pixels))
	{
		OutError = TEXT("Could not read the active Level Editor viewport image.");
		return false;
	}
	return PrepareViewportContextPng(
		Pixels,
		ViewportSize.X,
		ViewportSize.Y,
		MaximumLongEdge,
		OutPngBytes,
		OutError);
#else
	OutPngBytes.Reset();
	OutError = TEXT("The Level Editor viewport is unavailable outside the editor.");
	return false;
#endif
}

void CleanupCaptureResources()
{
	if (!IsInGameThread())
	{
		return;
	}
	SetCaptureLightsVisible(false);
	if (ASceneCapture2D* CaptureActor = GCaptureActor.Get())
	{
		if (USceneCaptureComponent2D* CaptureComponent = CaptureActor->GetCaptureComponent2D())
		{
			CaptureComponent->TextureTarget = nullptr;
		}
		CaptureActor->Destroy();
	}
	ReleaseCaptureRenderTarget();
	ReleaseGeometryMaskRenderTarget();
	GCaptureWorld.Reset();
	GCaptureActor.Reset();
	GKeyLight.Reset();
	GFillLight.Reset();
}

namespace
{
void PublishCaptureStreamingView(
	const USceneCaptureComponent2D* CaptureComponent,
	const int32 Resolution,
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const float DurationSeconds)
{
	if (!CaptureComponent || !World || Resolution <= 0)
	{
		return;
	}

	TWeakObjectPtr<AActor> ActorToBoost;
	for (const UPrimitiveComponent* ObjectComponent : ObjectComponents)
	{
		if (ObjectComponent)
		{
			ActorToBoost = ObjectComponent->GetOwner();
			if (ActorToBoost.IsValid())
			{
				break;
			}
		}
	}

	const float ScreenSize = static_cast<float>(Resolution);
	const float FovScreenSize = ScreenSize / FMath::Tan(
		FMath::DegreesToRadians(CaptureComponent->FOVAngle * 0.5f));
	IStreamingManager::Get().AddViewInformation(
		CaptureComponent->GetComponentLocation(),
		ScreenSize,
		FovScreenSize,
		1.0f,
		false,
		FMath::Max(0.0f, DurationSeconds),
		ActorToBoost,
		World);
}
}

#if WITH_DEV_AUTOMATION_TESTS
void SetNeutralCaptureLightingForTests(UWorld* World, const bool bEnabled)
{
	check(IsInGameThread());
	GNeutralCaptureLightingTestWorld = World;
	GNeutralCaptureLightingForTests = bEnabled;
}
#endif

struct FLiveObjectCaptureSession::FImpl
{
	TSharedPtr<FNeutralCaptureViewExtension, ESPMode::ThreadSafe> NeutralViewExtension;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ASceneCapture2D> CaptureActor;
	TWeakObjectPtr<USceneCaptureComponent2D> CaptureComponent;
	TWeakObjectPtr<USpotLightComponent> KeyLight;
	TWeakObjectPtr<USpotLightComponent> FillLight;
	TWeakObjectPtr<UTextureRenderTarget2D> RenderTarget;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> ObjectComponents;
	mutable TArray<UPrimitiveComponent*> ResolvedComponentScratch;
	FBox WorldBounds = FBox(ForceInit);
	FVector ViewDirection = FVector(-1.0, -1.0, -0.65).GetSafeNormal();
	FVector2D TargetOffset = FVector2D::ZeroVector;
	float DistanceScale = 1.0f;
	TOptional<float> ExposureDelta;
	int32 Resolution = 0;
	bool bSuppressDirectSpecularHighlights = false;
	bool bAllowSuppressedSpecularRecovery = false;
	bool bUseFallbackFill = false;
	bool bUseFallbackKey = false;
	bool bUseOpposingDiffuse = false;
	bool bRenderFromCallerTick = false;
	bool bRenderingEnabled = false;
	uint64 LastCallerDrivenRenderFrame = MAX_uint64;
	bool bEnded = false;

	~FImpl()
	{
		End();
	}

	const TArray<UPrimitiveComponent*>& ResolveComponents() const
	{
		ResolvedComponentScratch.Reset();
		for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent : ObjectComponents)
		{
			if (UPrimitiveComponent* Component = WeakComponent.Get())
			{
				ResolvedComponentScratch.Add(Component);
			}
		}
		return ResolvedComponentScratch;
	}

	bool IsUsable() const
	{
		UWorld* BoundWorld = World.Get();
		USceneCaptureComponent2D* Component = CaptureComponent.Get();
		UTextureRenderTarget2D* Target = RenderTarget.Get();
		if (bEnded || !::IsValid(BoundWorld) || !::IsValid(CaptureActor.Get())
			|| !::IsValid(Component) || !::IsValid(Target)
			|| Component->GetWorld() != BoundWorld
			|| Component->TextureTarget != Target
			|| Target->SizeX != Resolution || Target->SizeY != Resolution)
		{
			return false;
		}
		return HasCapturableComponent(BoundWorld, ResolveComponents());
	}

	void PublishStreamingView() const
	{
		PublishCaptureStreamingView(
			CaptureComponent.Get(),
			Resolution,
			World.Get(),
			ResolveComponents(),
			0.0f);
	}

	void ApplyRecoveryProfile()
	{
		SetCaptureExposureDelta(
			CaptureComponent.Get(),
			World.Get(),
			ExposureDelta);
		if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
		{
			Component->ShowFlags.SetSpecular(
				!bSuppressDirectSpecularHighlights
				|| (bAllowSuppressedSpecularRecovery && bUseFallbackKey));
		}
		SetCaptureRecoveryLightsVisible(
			KeyLight.Get(),
			FillLight.Get(),
			bUseFallbackFill,
			bUseFallbackKey,
			bUseOpposingDiffuse);
	}

	void SetRecoveryProfile(
		const TOptional<float>& InExposureDelta,
		const bool bInUseFallbackFill,
		const bool bInUseFallbackKey = false,
		const bool bInUseOpposingDiffuse = false)
	{
		ExposureDelta = InExposureDelta;
		bUseFallbackFill = bInUseFallbackFill;
		bUseFallbackKey = bInUseFallbackKey;
		bUseOpposingDiffuse = bInUseOpposingDiffuse;
		ApplyRecoveryProfile();
	}

	bool ApplyView(
		const FVector& InViewDirection,
		const float InDistanceScale,
		const FVector2D& InTargetOffset,
		FString& OutError)
	{
		OutError.Reset();
		if (!IsUsable())
		{
			OutError = TEXT("The live capture session is no longer valid in this editor world.");
			return false;
		}

		UWorld* BoundWorld = World.Get();
		ASceneCapture2D* Actor = CaptureActor.Get();
		USceneCaptureComponent2D* Component = CaptureComponent.Get();
		const TArray<UPrimitiveComponent*>& Components = ResolveComponents();
		const FCapturePlacement Placement = BuildCapturePlacement(
			BoundWorld,
			Components,
			WorldBounds,
			InViewDirection,
			InDistanceScale,
			InTargetOffset);
		Actor->SetActorLocationAndRotation(
			Placement.CameraLocation,
			(Placement.Center - Placement.CameraLocation).Rotation());
		PositionCaptureLights(
			KeyLight.Get(),
			FillLight.Get(),
			Placement.Center,
			Placement.CameraLocation,
			Placement.Radius,
			bSuppressDirectSpecularHighlights,
			bAllowSuppressedSpecularRecovery);
		// Positioning deliberately turns the emergency light off. Reapply the one
		// profile selected at session startup; orbit/pan/zoom then remain pure
		// transform updates with no readback or per-drag capture-policy changes.
		ApplyRecoveryProfile();
		Component->MaxViewDistanceOverride = Placement.Distance + Placement.Radius * 6.0f;
		PublishStreamingView();

		ViewDirection = Placement.ViewDirection;
		DistanceScale = FMath::Clamp(InDistanceScale, 0.55f, 2.5f);
		TargetOffset.X = FMath::Clamp(InTargetOffset.X, -0.75, 0.75);
		TargetOffset.Y = FMath::Clamp(InTargetOffset.Y, -0.75, 0.75);
		return true;
	}

	void CalibrateReadability()
	{
		USceneCaptureComponent2D* Component = CaptureComponent.Get();
		UTextureRenderTarget2D* Target = RenderTarget.Get();
		if (!Component || !Target || Resolution <= 0)
		{
			return;
		}

		const FString Subject = ObjectComponents.IsEmpty()
			? TEXT("unknown")
			: GetNameSafe(ObjectComponents[0].IsValid()
				? ObjectComponents[0]->GetOwner()
				: nullptr);
		UE_LOG(
			LogConvaiSceneAutoTaggerCapture,
			Verbose,
			TEXT("live '%s' lighting: baselineSpecular=%s measuredMetalRecovery=%s"),
			*Subject,
			bSuppressDirectSpecularHighlights ? TEXT("off") : TEXT("on"),
			bAllowSuppressedSpecularRecovery ? TEXT("allowed") : TEXT("not-allowed"));
		SetRecoveryProfile(TOptional<float>(), false);
		TArray<FColor> BaselinePixels;
		if (!CapturePixelsNow(Component, Target, Resolution, BaselinePixels))
		{
			return;
		}
		// The controller has already kept this exact camera/profile alive across
		// several real Editor frames. Preserve that accumulated texture, GI, and
		// exposure state instead of declaring a camera cut here. A second identical
		// readback filters the last transient frame without throwing the warm state
		// away. Only the changed recovery profiles below start a new generation.
		TArray<FColor> ConfirmedBaselinePixels;
		if (CapturePixelsNow(Component, Target, Resolution, ConfirmedBaselinePixels))
		{
			BaselinePixels = MoveTemp(ConfirmedBaselinePixels);
		}
		TArray<uint8> GeometryMask;
		CaptureGeometryMaskNow(Component, Resolution, GeometryMask);
		const TArray<uint8>* GeometryMaskPtr = GeometryMask.IsEmpty()
			? nullptr
			: &GeometryMask;

		FCaptureEvaluation BestEvaluation = EvaluateCapture(
			BaselinePixels,
			Resolution,
			GeometryMaskPtr);
		LogCaptureEvaluation(TEXT("live"), Subject, TEXT("baseline"), BestEvaluation);
		TOptional<float> BestExposureDelta;
		bool bBestUsesFallbackFill = false;
		bool bBestUsesFallbackKey = false;
		bool bBestUsesOpposingDiffuse = false;
		const auto RestoreBestProfile = [&]()
		{
			SetRecoveryProfile(
				BestExposureDelta,
				bBestUsesFallbackFill,
				bBestUsesFallbackKey,
				bBestUsesOpposingDiffuse);
			// The automatic path renders two real frames after calibration. Mark the
			// restored winner as a new generation so its first continuous frame cannot
			// inherit the last rejected probe's eye-adaptation state.
			Component->bCameraCutThisFrame = true;
		};
		const auto CaptureProfile = [&](const TOptional<float>& Delta,
			const bool bUseFill,
			const bool bUseKey,
			const bool bUseOpposingDiffuse,
			FCaptureEvaluation& OutEvaluation)
		{
			SetRecoveryProfile(Delta, bUseFill, bUseKey, bUseOpposingDiffuse);
			TArray<FColor> ProbePixels;
			if (!CaptureSettledProfilePixels(Component, Target, Resolution, ProbePixels))
			{
				return false;
			}
			OutEvaluation = EvaluateCapture(ProbePixels, Resolution, GeometryMaskPtr);
			return true;
		};

		if (!BestEvaluation.bDegenerate
			&& !bSuppressDirectSpecularHighlights
			&& ShouldAttemptHighlightRecovery(BestEvaluation.Metrics))
		{
			const TOptional<float> HighlightDelta(
				ComputeHighlightRecoveryExposureBias(BestEvaluation.Metrics));
			FCaptureEvaluation HighlightEvaluation;
			if (CaptureProfile(HighlightDelta, false, false, false, HighlightEvaluation)
				&& !HighlightEvaluation.bDegenerate
				&& PreferHighlightRecovery(
					BestEvaluation.Metrics,
					HighlightEvaluation.Metrics))
			{
				BestEvaluation = HighlightEvaluation;
				BestExposureDelta = HighlightDelta;
			}
			LogCaptureEvaluation(TEXT("live"), Subject, TEXT("highlight EV"), HighlightEvaluation);
			RestoreBestProfile();
		}

		if (NeedsReadabilityRecovery(BestEvaluation))
		{
			const TOptional<float> BrightenDelta(1.0f);
			FCaptureEvaluation BrightEvaluation;
			if (CaptureProfile(BrightenDelta, false, false, false, BrightEvaluation)
				&& PreferReadabilityRecovery(BestEvaluation, BrightEvaluation))
			{
				BestEvaluation = BrightEvaluation;
				BestExposureDelta = BrightenDelta;
				bBestUsesFallbackFill = false;
				bBestUsesFallbackKey = false;
				bBestUsesOpposingDiffuse = false;
			}
			LogCaptureEvaluation(TEXT("live"), Subject, TEXT("positive EV"), BrightEvaluation);
			RestoreBestProfile();
		}

		if (NeedsReadabilityRecovery(BestEvaluation))
		{
			FCaptureEvaluation FillEvaluation;
			if (CaptureProfile(TOptional<float>(), true, false, false, FillEvaluation)
				&& PreferReadabilityRecovery(BestEvaluation, FillEvaluation))
			{
				BestEvaluation = FillEvaluation;
				BestExposureDelta.Reset();
				bBestUsesFallbackFill = true;
				bBestUsesFallbackKey = false;
				bBestUsesOpposingDiffuse = false;
			}
			LogCaptureEvaluation(TEXT("live"), Subject, TEXT("fallback fill"), FillEvaluation);
			RestoreBestProfile();
		}

		// Flat artwork does not benefit from a second directional light. If its
		// lower tones remain unreadable, combine the existing specular-free fill
		// with one measured exposure offset and retain it only when the rendered
		// histogram improves without clipping or losing detail.
		if (ShouldAttemptPlanarDiffuseExposureRecovery(
			BestEvaluation.Metrics,
			bSuppressDirectSpecularHighlights,
			bAllowSuppressedSpecularRecovery))
		{
			const TOptional<float> PanelExposureDelta(
				ComputePlanarDiffuseExposureBias(BestEvaluation.Metrics));
			FCaptureEvaluation PanelEvaluation;
			if (CaptureProfile(
				PanelExposureDelta,
				true,
				false,
				false,
				PanelEvaluation)
				&& PreferPlanarDiffuseExposureRecovery(
					BestEvaluation.Metrics,
					PanelEvaluation.Metrics))
			{
				BestEvaluation = PanelEvaluation;
				BestExposureDelta = PanelExposureDelta;
				bBestUsesFallbackFill = true;
				bBestUsesFallbackKey = false;
				bBestUsesOpposingDiffuse = false;
			}
			LogCaptureEvaluation(
				TEXT("live"),
				Subject,
				TEXT("planar diffuse EV"),
				PanelEvaluation);
			RestoreBestProfile();
		}

		// A neutral sculpture can become readable under the first diffuse fill while
		// retaining an unnecessarily black shadow side. Probe the opposite existing
		// light at low power, still with zero specular, and keep it only when the
		// measured lower tail improves without flattening the object.
		if (ShouldAttemptOpposingDiffuseRecovery(
			BestEvaluation.Metrics,
			bBestUsesFallbackFill && !bBestUsesFallbackKey,
			bSuppressDirectSpecularHighlights))
		{
			FCaptureEvaluation OpposingDiffuseEvaluation;
			if (CaptureProfile(
				TOptional<float>(),
				true,
				false,
				true,
				OpposingDiffuseEvaluation)
				&& PreferOpposingDiffuseRecovery(
					BestEvaluation.Metrics,
					OpposingDiffuseEvaluation.Metrics))
			{
				BestEvaluation = OpposingDiffuseEvaluation;
				BestExposureDelta.Reset();
				bBestUsesFallbackFill = true;
				bBestUsesFallbackKey = false;
				bBestUsesOpposingDiffuse = true;
			}
			LogCaptureEvaluation(
				TEXT("live"),
				Subject,
				TEXT("opposing diffuse"),
				OpposingDiffuseEvaluation);
			RestoreBestProfile();
		}

		// Some dimensional subjects (notably rough bronze and stone) already have
		// bright readable highlights, so they never enter the severe-dark path even
		// though most of the object is still underexposed. Probe one deterministic,
		// gentle two-sided balance and retain it only when measured lower-tone
		// information improves without moving the highlight shoulder into clipping.
		if (ShouldAttemptDimensionalShadowBalanceRecovery(
			BestEvaluation.Metrics,
			bSuppressDirectSpecularHighlights))
		{
			FCaptureEvaluation ShadowBalanceEvaluation;
			if (CaptureProfile(
				TOptional<float>(),
				true,
				true,
				true,
				ShadowBalanceEvaluation)
				&& PreferDimensionalShadowBalanceRecovery(
					BestEvaluation.Metrics,
					ShadowBalanceEvaluation.Metrics))
			{
				BestEvaluation = ShadowBalanceEvaluation;
				BestExposureDelta.Reset();
				bBestUsesFallbackFill = true;
				bBestUsesFallbackKey = true;
				bBestUsesOpposingDiffuse = true;
			}
			LogCaptureEvaluation(
				TEXT("live"),
				Subject,
				TEXT("dimensional shadow balance"),
				ShadowBalanceEvaluation);
			RestoreBestProfile();
		}

		// If the gentle balance still leaves the dimensional subject below the
		// absolute readability floor, try exactly one stronger version of that same
		// two-sided profile. The bounded +0.5 EV probe runs before the emergency key
		// ladder and is retained only when coverage, highlights, tone, and detail stay
		// safe.
		if (ShouldAttemptDimensionalReadabilityFloorRecovery(
			BestEvaluation.Metrics,
			bSuppressDirectSpecularHighlights))
		{
			const TOptional<float> FloorExposureDelta(0.50f);
			FCaptureEvaluation FloorEvaluation;
			if (CaptureProfile(
				FloorExposureDelta,
				true,
				true,
				true,
				FloorEvaluation)
				&& PreferDimensionalReadabilityFloorRecovery(
					BestEvaluation.Metrics,
					FloorEvaluation.Metrics))
			{
				BestEvaluation = FloorEvaluation;
				BestExposureDelta = FloorExposureDelta;
				bBestUsesFallbackFill = true;
				bBestUsesFallbackKey = true;
				bBestUsesOpposingDiffuse = true;
			}
			LogCaptureEvaluation(
				TEXT("live"),
				Subject,
				TEXT("dimensional shadow balance +0.5 EV"),
				FloorEvaluation);
			RestoreBestProfile();
		}

		// Diffuse fill is intentionally the first fallback. Only a dimensional
		// subject that is still unreadable receives a second camera-side key; this
		// lets true metals return a bounded highlight without changing already-good
		// diffuse subjects or reintroducing planar reflection discs.
		if ((!bSuppressDirectSpecularHighlights
				|| bAllowSuppressedSpecularRecovery)
			&& NeedsReadabilityRecovery(BestEvaluation))
		{
			const float KeyExposureDeltas[] = {
				FallbackKeyInitialExposureDelta,
				FallbackKeyBrightExposureDelta,
				FallbackKeyFinalExposureDelta};
			for (const float KeyExposureValue : KeyExposureDeltas)
			{
				if (!NeedsReadabilityRecovery(BestEvaluation))
				{
					break;
				}

				const TOptional<float> KeyExposureDelta(KeyExposureValue);
				FCaptureEvaluation KeyEvaluation;
				if (CaptureProfile(KeyExposureDelta, true, true, false, KeyEvaluation)
					&& (PreferSevereDarkDimensionalKeyRecovery(
							BestEvaluation.Metrics,
							KeyEvaluation.Metrics,
							bSuppressDirectSpecularHighlights,
							bAllowSuppressedSpecularRecovery)
						|| PreferDimensionalKeyRecovery(BestEvaluation, KeyEvaluation)))
				{
					BestEvaluation = KeyEvaluation;
					BestExposureDelta = KeyExposureDelta;
					bBestUsesFallbackFill = true;
					bBestUsesFallbackKey = true;
					bBestUsesOpposingDiffuse = false;
				}
				LogCaptureEvaluation(
					TEXT("live"),
					Subject,
					KeyExposureValue == FallbackKeyInitialExposureDelta
						? TEXT("dimensional key")
						: KeyExposureValue == FallbackKeyBrightExposureDelta
							? TEXT("dimensional key bright")
							: TEXT("dimensional key final"),
					KeyEvaluation);
				RestoreBestProfile();
			}
		}

		// Neutral mode already uses the same controlled rig for every recovery.
		// A small earlier floor improvement can leave the last key candidate short
		// of its required incremental gain. Try one final exposure-only candidate
		// against the existing strict floor/highlight/detail guards, without
		// changing the lighting or relaxing acceptance for already-readable objects.
		if (UseNeutralCaptureLighting(World.Get())
			&& ShouldAttemptDimensionalReadabilityFloorRecovery(
				BestEvaluation.Metrics, bSuppressDirectSpecularHighlights))
		{
			const TOptional<float> NeutralExposureDelta(NeutralReadabilityExposureDelta);
			FCaptureEvaluation NeutralEvaluation;
			const bool bAccepted = CaptureProfile(
				NeutralExposureDelta,
				bBestUsesFallbackFill,
				bBestUsesFallbackKey,
				bBestUsesOpposingDiffuse,
				NeutralEvaluation)
				&& PreferDimensionalReadabilityFloorRecovery(
					BestEvaluation.Metrics, NeutralEvaluation.Metrics);
			if (bAccepted)
			{
				BestEvaluation = NeutralEvaluation;
				BestExposureDelta = NeutralExposureDelta;
			}
			LogCaptureEvaluation(TEXT("live"), Subject, TEXT("neutral readability +3.5 EV"), NeutralEvaluation);
			UE_LOG(LogConvaiSceneAutoTaggerCapture, Verbose,
				TEXT("live '%s' neutral readability +3.5 EV accepted=%s"),
				*Subject, bAccepted ? TEXT("true") : TEXT("false"));
			RestoreBestProfile();
		}

		// A truly absent/back-face-only starting view should still open so the user
		// can orbit to a valid face. Do not leave a failed recovery profile active.
		if (!HasReadableForeground(BestEvaluation))
		{
			SetRecoveryProfile(TOptional<float>(), false);
			Component->bCameraCutThisFrame = true;
		}
	}

	void SetContinuousCaptureEnabled(
		const bool bEnabled,
		const bool bPreserveViewStateWhilePaused = false)
	{
		bRenderingEnabled = bEnabled;
		if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
		{
			Component->bTickInEditor = true;
			Component->bCaptureOnMovement = false;
			Component->bAlwaysPersistRenderingState = bEnabled || bPreserveViewStateWhilePaused;
			Component->bCaptureEveryFrame = bEnabled && !bRenderFromCallerTick;
			// bCaptureEveryFrame queues CaptureSceneDeferred from TickComponent. Do
			// not throttle that tick: a 30 Hz interval turns fast orbit input into a
			// visibly stepped sequence even though the render target is persistent.
			Component->PrimaryComponentTick.TickInterval = 0.0f;
			Component->SetComponentTickEnabled(bEnabled && !bRenderFromCallerTick);
			if (bEnabled)
			{
				Component->Activate(true);
				if (bRenderFromCallerTick)
				{
					// Edit View owns this persistent capture's cadence. CaptureScene only
					// enqueues render work; it does not perform the commit-time readback.
					Component->CaptureScene();
					LastCallerDrivenRenderFrame = GFrameCounter;
				}
				else
				{
					// Queue the first asynchronous frame immediately instead of waiting for
					// the next editor-world component tick. AddUnique makes a same-frame
					// TickComponent request harmless.
					Component->CaptureSceneDeferred();
				}
			}
		}
	}

	bool RenderCallerDrivenFrame(FString& OutError)
	{
		OutError.Reset();
		if (!IsUsable())
		{
			OutError = TEXT("The live capture session is no longer valid in this editor world.");
			return false;
		}
		if (!bRenderFromCallerTick || !bRenderingEnabled
			|| LastCallerDrivenRenderFrame == GFrameCounter)
		{
			return true;
		}

		PublishStreamingView();
		CaptureComponent->CaptureScene();
		LastCallerDrivenRenderFrame = GFrameCounter;
		return true;
	}

	void End()
	{
		if (bEnded)
		{
			return;
		}
		if (!ensureMsgf(IsInGameThread(), TEXT("Live object capture must end on the game thread.")))
		{
			return;
		}
		bEnded = true;
		SetContinuousCaptureEnabled(false);
		if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
		{
			Component->ShowOnlyComponents.Reset();
			Component->TextureTarget = nullptr;
		}
		SetCaptureLightsVisible(KeyLight.Get(), FillLight.Get(), false);
		if (ASceneCapture2D* Actor = CaptureActor.Get())
		{
			Actor->Destroy();
		}
		// A continuous capture may already have queued work for this target. Keep
		// the UObject rooted until all earlier render commands have crossed a fence;
		// the UI's deferred Slate brush independently covers any Slate draw buffers.
		ReleaseRootedRenderTargetAfterFence(RenderTarget.Get());
		ObjectComponents.Reset();
		ResolvedComponentScratch.Reset();
		World.Reset();
		CaptureActor.Reset();
		CaptureComponent.Reset();
		KeyLight.Reset();
		FillLight.Reset();
		RenderTarget.Reset();
		Resolution = 0;
	}
};

FLiveObjectCaptureSession::FLiveObjectCaptureSession(TUniquePtr<FImpl>&& InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FLiveObjectCaptureSession::~FLiveObjectCaptureSession()
{
	End();
}

UTextureRenderTarget2D* FLiveObjectCaptureSession::GetRenderTarget() const
{
	return Impl ? Impl->RenderTarget.Get() : nullptr;
}

int32 FLiveObjectCaptureSession::GetResolution() const
{
	return Impl ? Impl->Resolution : 0;
}

UWorld* FLiveObjectCaptureSession::GetWorld() const
{
	return Impl ? Impl->World.Get() : nullptr;
}

bool FLiveObjectCaptureSession::IsValid() const
{
	return Impl && Impl->IsUsable();
}

bool FLiveObjectCaptureSession::UpdateView(
	const FVector& ViewDirection,
	const float DistanceScale,
	const FVector2D& TargetOffset,
	FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("The live capture view can only be updated on the game thread.");
		return false;
	}
	if (!Impl)
	{
		OutError = TEXT("The live capture session has ended.");
		return false;
	}
	return Impl->ApplyView(ViewDirection, DistanceScale, TargetOffset, OutError);
}

bool FLiveObjectCaptureSession::RenderFrame(FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("The live capture can only render on the game thread.");
		return false;
	}
	if (!Impl)
	{
		OutError = TEXT("The live capture session has ended.");
		return false;
	}
	return Impl->RenderCallerDrivenFrame(OutError);
}

bool FLiveObjectCaptureSession::CalibrateReadability(FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("The live capture can only be calibrated on the game thread.");
		return false;
	}
	if (!Impl || !Impl->IsUsable())
	{
		OutError = TEXT("The live capture session is no longer valid in this editor world.");
		return false;
	}

	Impl->SetContinuousCaptureEnabled(false, true);
	Impl->CalibrateReadability();
	Impl->SetContinuousCaptureEnabled(true);
	return true;
}

bool FLiveObjectCaptureSession::ReadCurrentPixels(
	TArray<FColor>& OutBGRA,
	FString& OutError)
{
	OutBGRA.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("The live capture can only be read on the game thread.");
		return false;
	}
	if (!Impl || !Impl->IsUsable())
	{
		OutError = TEXT("The live capture session is no longer valid in this editor world.");
		return false;
	}

	// Pause the every-frame tick but retain the exact temporal/exposure view state
	// used by the displayed frame through the synchronous commit readback.
	Impl->SetContinuousCaptureEnabled(false, true);
	ON_SCOPE_EXIT
	{
		if (Impl && Impl->IsUsable())
		{
			Impl->SetContinuousCaptureEnabled(true);
		}
	};
	if (!CapturePixelsNow(
		Impl->CaptureComponent.Get(),
		Impl->RenderTarget.Get(),
		Impl->Resolution,
		OutBGRA))
	{
		OutError = TEXT("empty_capture");
		return false;
	}
	const FColor Backdrop = EstimateEncodedBackdrop(OutBGRA, Impl->Resolution);
	TArray<uint8> GeometryMask;
	const bool bHasGeometryMask = CaptureGeometryMaskNow(
		Impl->CaptureComponent.Get(),
		Impl->Resolution,
		GeometryMask);
	const FCaptureEvaluation Evaluation = EvaluateCapture(
		OutBGRA,
		Impl->Resolution,
		bHasGeometryMask ? &GeometryMask : nullptr);
	if (IsDegenerateCapture(OutBGRA, Backdrop) && !bHasGeometryMask)
	{
		OutBGRA.Reset();
		OutError = TEXT("empty_capture");
		return false;
	}
	if (!HasUsefulCapturedAppearance(Evaluation))
	{
		OutBGRA.Reset();
		OutError = bHasGeometryMask
			? TEXT("unreadable_dark_capture")
			: TEXT("empty_capture");
		return false;
	}
	return true;
}

void FLiveObjectCaptureSession::End()
{
	if (Impl)
	{
		Impl->End();
		Impl.Reset();
	}
}

TUniquePtr<FLiveObjectCaptureSession> BeginLiveObjectCapture(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const FBox& WorldBounds,
	const int32 RequestedResolution,
	FString& OutError,
	const FVector& ViewDirection,
	const float DistanceScale,
	const FVector2D& TargetOffset,
	const bool bSuppressDirectSpecularHighlights,
	const bool bAllowSuppressedSpecularRecovery,
	const bool bCalibrateOnStart,
	const bool bRenderFromCallerTick)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("The live capture can only begin on the game thread.");
		return nullptr;
	}
	if (!World || !WorldBounds.IsValid || WorldBounds.GetCenter().ContainsNaN()
		|| WorldBounds.GetExtent().ContainsNaN()
		|| !HasCapturableComponent(World, ObjectComponents))
	{
		OutError = TEXT("empty_capture");
		return nullptr;
	}

	TUniquePtr<FLiveObjectCaptureSession::FImpl> NewImpl =
		MakeUnique<FLiveObjectCaptureSession::FImpl>();
	NewImpl->World = World;
	NewImpl->WorldBounds = WorldBounds;
	NewImpl->Resolution = FMath::Clamp(RequestedResolution, MinCaptureResolution, MaxCaptureResolution);
	NewImpl->bSuppressDirectSpecularHighlights = bSuppressDirectSpecularHighlights;
	NewImpl->bAllowSuppressedSpecularRecovery = bAllowSuppressedSpecularRecovery;
	NewImpl->bRenderFromCallerTick = bRenderFromCallerTick;
	for (UPrimitiveComponent* Component : ObjectComponents)
	{
		if (IsValid(Component) && Component->IsRegistered() && Component->GetWorld() == World)
		{
			NewImpl->ObjectComponents.Add(Component);
		}
	}
	NewImpl->ResolvedComponentScratch.Reserve(NewImpl->ObjectComponents.Num());

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	if (!Target)
	{
		OutError = TEXT("capture failed: could not allocate live render target");
		return nullptr;
	}
	// Match one-shot evidence: FinalColorLDR renders into an explicitly sRGB
	// target, while raw readback preserves the same display-ready bytes later
	// retained for the model.
	Target->RenderTargetFormat = RTF_RGBA8_SRGB;
	Target->bForceLinearGamma = false;
	Target->SRGB = true;
	Target->ClearColor = CaptureBackground;
	Target->bAutoGenerateMips = false;
	Target->InitAutoFormat(NewImpl->Resolution, NewImpl->Resolution);
	Target->UpdateResourceImmediate(true);
	Target->AddToRoot();
	NewImpl->RenderTarget = Target;

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		ASceneCapture2D::StaticClass(),
		TEXT("ConvaiSceneAutoTaggerLiveCapture"));
	SpawnParameters.ObjectFlags = RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASceneCapture2D* Actor = World->SpawnActor<ASceneCapture2D>(SpawnParameters);
	if (!Actor)
	{
		OutError = TEXT("capture failed: could not spawn live scene capture");
		return nullptr;
	}
	Actor->SetActorEnableCollision(false);
	NewImpl->CaptureActor = Actor;

	USceneCaptureComponent2D* Component = Actor->GetCaptureComponent2D();
	if (!Component)
	{
		OutError = TEXT("capture failed: live scene capture has no component");
		return nullptr;
	}
	NewImpl->CaptureComponent = Component;
	AttachNeutralCaptureExtension(Component, NewImpl->NeutralViewExtension);
	NewImpl->SetContinuousCaptureEnabled(false);
	USpotLightComponent* KeyLight = CreateCaptureLight(
		Actor,
		TEXT("ConvaiSceneAutoTaggerLiveKeyLight"));
	USpotLightComponent* FillLight = CreateCaptureLight(
		Actor,
		TEXT("ConvaiSceneAutoTaggerLiveFillLight"));
	// Neutral capture requires both owned lights; validation below fails cleanly
	// if allocation or an exclusive custom view channel is unavailable.
	NewImpl->KeyLight = KeyLight;
	NewImpl->FillLight = FillLight;
	ConfigureCaptureComponent(
		Component,
		Target,
		WorldBounds.GetExtent().Size() * 8.0f,
		bSuppressDirectSpecularHighlights);
	if (!ConfigureNeutralLightIsolation(Component, KeyLight, FillLight, OutError))
	{
		return nullptr;
	}
	SetCaptureShowOnlyComponents(Component, World, ObjectComponents);
	if (!NewImpl->ApplyView(ViewDirection, DistanceScale, TargetOffset, OutError))
	{
		return nullptr;
	}

	// Edit View chooses a profile immediately. Automatic capture deliberately
	// starts uncalibrated, lets the same high-resolution view warm streamed
	// textures/mesh detail across real editor frames, and calibrates once afterward.
	if (bCalibrateOnStart)
	{
		NewImpl->SetContinuousCaptureEnabled(false, true);
		NewImpl->CalibrateReadability();
	}
	NewImpl->SetContinuousCaptureEnabled(true);
	return TUniquePtr<FLiveObjectCaptureSession>(
		new FLiveObjectCaptureSession(MoveTemp(NewImpl)));
}

bool CaptureObjectCell(
	UWorld* World,
	const TArray<UPrimitiveComponent*>& ObjectComponents,
	const FBox& WorldBounds,
	const int32 CellPixels,
	TArray<FColor>& OutBGRA,
	FString& OutError,
	const FVector& ViewDirection,
	const float DistanceScale,
	const FVector2D& TargetOffset,
	const bool bSuppressDirectSpecularHighlights,
	const bool bAllowSuppressedSpecularRecovery,
	const bool bReadinessPrimeOnly,
	TArray<uint8>* OutGeometryMask)
{
	// Adapted from ConvaiAssemblyStudio's
	// ConvaiObjectCapture.cpp::CapturePrimitiveComponentsPixels/ComputeFitDistanceForBounds.
	OutBGRA.Reset();
	if (OutGeometryMask)
	{
		OutGeometryMask->Reset();
	}
	OutError.Reset();

	if (!IsInGameThread())
	{
		OutError = TEXT("capture failed: must run on the game thread");
		return false;
	}
	if (!World)
	{
		OutError = TEXT("capture failed: no world");
		return false;
	}
	if (!WorldBounds.IsValid || WorldBounds.GetCenter().ContainsNaN() || WorldBounds.GetExtent().ContainsNaN())
	{
		OutError = TEXT("empty_capture");
		return false;
	}
	if (!HasCapturableComponent(World, ObjectComponents))
	{
		OutError = TEXT("empty_capture");
		return false;
	}

	const int32 Resolution = FMath::Clamp(CellPixels, MinCaptureResolution, MaxCaptureResolution);
	UTextureRenderTarget2D* RenderTarget = GetReusableCaptureRenderTarget(Resolution);
	if (!RenderTarget)
	{
		OutError = TEXT("capture failed: could not allocate render target");
		return false;
	}

	ASceneCapture2D* CaptureActor = GetReusableCaptureActor(World);
	if (!CaptureActor)
	{
		OutError = TEXT("capture failed: could not spawn scene capture");
		return false;
	}

	USceneCaptureComponent2D* CaptureComponent = CaptureActor->GetCaptureComponent2D();
	if (!CaptureComponent)
	{
		OutError = TEXT("capture failed: scene capture has no component");
		return false;
	}

	ON_SCOPE_EXIT
	{
		SetCaptureLightsVisible(false);
		CaptureComponent->ShowOnlyComponents.Reset();
		CaptureComponent->TextureTarget = nullptr;
	};

	const FCapturePlacement Placement = BuildCapturePlacement(
		World,
		ObjectComponents,
		WorldBounds,
		ViewDirection,
		DistanceScale,
		TargetOffset);

	CaptureActor->SetActorLocationAndRotation(
		Placement.CameraLocation,
		(Placement.Center - Placement.CameraLocation).Rotation());
	PositionCaptureLights(
		CaptureActor,
		Placement.Center,
		Placement.CameraLocation,
		Placement.Radius,
		bSuppressDirectSpecularHighlights,
		bAllowSuppressedSpecularRecovery);

	CaptureComponent->bCaptureEveryFrame = false;
	// Preserve the scene-capture view state between sequential cells so authored
	// exposure/skylight data is available without turning the sweeper into a set of
	// unrelated temporary cameras. It still renders only on explicit CaptureScene.
	CaptureComponent->bAlwaysPersistRenderingState = true;
	// Scene captures are intentionally one-shot, but virtual-texture feedback
	// throttling can otherwise leave their isolated evidence visibly blurry.
	// This override does not retain temporal history or enable continuous capture.
	ConfigureCaptureComponent(
		CaptureComponent,
		RenderTarget,
		Placement.Distance + Placement.Radius * 6.0f,
		bSuppressDirectSpecularHighlights);
	AttachNeutralCaptureExtension(CaptureComponent, GNeutralCaptureViewExtension);
	if (!ConfigureNeutralLightIsolation(CaptureComponent, GKeyLight.Get(), GFillLight.Get(), OutError))
	{
		return false;
	}
	SetCaptureShowOnlyComponents(CaptureComponent, World, ObjectComponents);
	CaptureComponent->ShowFlags.SetSpecular(!bSuppressDirectSpecularHighlights);
	SetCaptureRecoveryLightsVisible(false, false, false);
	SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());
	// Scene captures are not streaming viewpoints by default. Keep this synthetic
	// camera registered through the caller's bounded probe warm-up so every angle
	// is compared with texture/mesh demand matching its actual on-screen size.
	PublishCaptureStreamingView(
		CaptureComponent,
		Resolution,
		World,
		ObjectComponents,
		1.5f);
	if (bReadinessPrimeOnly)
	{
		// Render this exact direction once, then let the Editor advance a real
		// frame before scoring it. The readback completes shader/PSO work and emits
		// view-specific virtual-texture feedback that an asset-only residency wait
		// cannot trigger. The caller discards these pixels.
		CaptureComponent->bCameraCutThisFrame = true;
		return CapturePixelsNow(CaptureComponent, RenderTarget, Resolution, OutBGRA);
	}

	const auto CaptureIntoPixels = [&](TArray<FColor>& OutPixels)
	{
		return CaptureSettledProfilePixels(
			CaptureComponent,
			RenderTarget,
			Resolution,
			OutPixels);
	};
	const FString Subject = ObjectComponents.IsEmpty()
		? TEXT("unknown")
		: GetNameSafe(ObjectComponents[0] ? ObjectComponents[0]->GetOwner() : nullptr);
	UE_LOG(
		LogConvaiSceneAutoTaggerCapture,
		Verbose,
		TEXT("one-shot '%s' lighting: baselineSpecular=%s measuredMetalRecovery=%s"),
		*Subject,
		bSuppressDirectSpecularHighlights ? TEXT("off") : TEXT("on"),
		bAllowSuppressedSpecularRecovery ? TEXT("allowed") : TEXT("not-allowed"));

	if (!CaptureIntoPixels(OutBGRA))
	{
		OutError = TEXT("empty_capture");
		return false;
	}

	// Do not reject an all-dark baseline before recovery. A valid unlit plane or
	// true metal can encode almost exactly like the backdrop even though the
	// primitive is present. Only declare the capture empty after both bounded
	// exposure and fallback-light probes also fail to reveal useful foreground.
	TArray<uint8> GeometryMask;
	const bool bHasGeometryMask = CaptureGeometryMaskNow(
		CaptureComponent,
		Resolution,
		GeometryMask);
	if (bHasGeometryMask && OutGeometryMask)
	{
		*OutGeometryMask = GeometryMask;
	}
	const TArray<uint8>* GeometryMaskPtr = bHasGeometryMask ? &GeometryMask : nullptr;
	FCaptureEvaluation RetainedEvaluation = EvaluateCapture(
		OutBGRA,
		Resolution,
		GeometryMaskPtr);
	bool bSingleDiffuseFillRetained = false;
	LogCaptureEvaluation(TEXT("one-shot"), Subject, TEXT("baseline"), RetainedEvaluation);

	// Preserve the configured capture rig first. A bounded EV-only retry handles
	// broadly clipped captures without inventing new highlights.
	if (!RetainedEvaluation.bDegenerate
		&& !bSuppressDirectSpecularHighlights
		&& ShouldAttemptHighlightRecovery(RetainedEvaluation.Metrics))
	{
		SetCaptureExposureDelta(
			CaptureComponent,
			World,
			ComputeHighlightRecoveryExposureBias(RetainedEvaluation.Metrics));
		TArray<FColor> RecoveryBGRA;
		if (CaptureIntoPixels(RecoveryBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				RecoveryBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(TEXT("one-shot"), Subject, TEXT("highlight EV"), RecoveryEvaluation);
			if (!RecoveryEvaluation.bDegenerate
				&& PreferHighlightRecovery(
					RetainedEvaluation.Metrics,
					RecoveryEvaluation.Metrics))
			{
				OutBGRA = MoveTemp(RecoveryBGRA);
				RetainedEvaluation = RecoveryEvaluation;
			}
		}
	}

	if (NeedsReadabilityRecovery(RetainedEvaluation))
	{
		SetCaptureExposureDelta(CaptureComponent, World, 1.0f);
		TArray<FColor> ExposureRecoveryBGRA;
		if (CaptureIntoPixels(ExposureRecoveryBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				ExposureRecoveryBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(TEXT("one-shot"), Subject, TEXT("positive EV"), RecoveryEvaluation);
			if (PreferReadabilityRecovery(RetainedEvaluation, RecoveryEvaluation))
			{
				OutBGRA = MoveTemp(ExposureRecoveryBGRA);
				RetainedEvaluation = RecoveryEvaluation;
			}
		}
	}

	// Only an object that remains unreadably dark after the EV probe receives a
	// bounded fallback fill. This first light is diffuse for every subject; the
	// intensity scales with radius^2, so small busts are not blasted more strongly
	// than full-size statues.
	if (NeedsReadabilityRecovery(RetainedEvaluation))
	{
		// Test the bounded neutral fill against the initial exposure baseline. Do
		// not stack it with the +1 EV probe, which can flatten pale materials.
		SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());
		SetFallbackFillVisible(true);
		TArray<FColor> FillRecoveryBGRA;
		if (CaptureIntoPixels(FillRecoveryBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				FillRecoveryBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(TEXT("one-shot"), Subject, TEXT("fallback fill"), RecoveryEvaluation);
			if (PreferReadabilityRecovery(RetainedEvaluation, RecoveryEvaluation))
			{
				OutBGRA = MoveTemp(FillRecoveryBGRA);
				RetainedEvaluation = RecoveryEvaluation;
				bSingleDiffuseFillRetained = true;
			}
		}
		SetFallbackFillVisible(false);
	}

	if (ShouldAttemptPlanarDiffuseExposureRecovery(
		RetainedEvaluation.Metrics,
		bSuppressDirectSpecularHighlights,
		bAllowSuppressedSpecularRecovery))
	{
		const float PanelExposureDelta = ComputePlanarDiffuseExposureBias(
			RetainedEvaluation.Metrics);
		SetCaptureExposureDelta(CaptureComponent, World, PanelExposureDelta);
		SetFallbackFillVisible(true);
		TArray<FColor> PanelRecoveryBGRA;
		if (CaptureIntoPixels(PanelRecoveryBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				PanelRecoveryBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(
				TEXT("one-shot"),
				Subject,
				TEXT("planar diffuse EV"),
				RecoveryEvaluation);
			if (PreferPlanarDiffuseExposureRecovery(
				RetainedEvaluation.Metrics,
				RecoveryEvaluation.Metrics))
			{
				OutBGRA = MoveTemp(PanelRecoveryBGRA);
				RetainedEvaluation = RecoveryEvaluation;
			}
		}
		SetFallbackFillVisible(false);
		SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());
	}

	if (ShouldAttemptOpposingDiffuseRecovery(
		RetainedEvaluation.Metrics,
		bSingleDiffuseFillRetained,
		bSuppressDirectSpecularHighlights))
	{
		SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());
		SetCaptureRecoveryLightsVisible(true, false, true);
		TArray<FColor> OpposingDiffuseBGRA;
		if (CaptureIntoPixels(OpposingDiffuseBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				OpposingDiffuseBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(
				TEXT("one-shot"),
				Subject,
				TEXT("opposing diffuse"),
				RecoveryEvaluation);
			if (PreferOpposingDiffuseRecovery(
				RetainedEvaluation.Metrics,
				RecoveryEvaluation.Metrics))
			{
				OutBGRA = MoveTemp(OpposingDiffuseBGRA);
				RetainedEvaluation = RecoveryEvaluation;
			}
		}
		SetCaptureRecoveryLightsVisible(false, false, false);
	}

	if (ShouldAttemptDimensionalShadowBalanceRecovery(
		RetainedEvaluation.Metrics,
		bSuppressDirectSpecularHighlights))
	{
		SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());
		SetCaptureRecoveryLightsVisible(true, true, true);
		TArray<FColor> ShadowBalanceBGRA;
		if (CaptureIntoPixels(ShadowBalanceBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				ShadowBalanceBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(
				TEXT("one-shot"),
				Subject,
				TEXT("dimensional shadow balance"),
				RecoveryEvaluation);
			if (PreferDimensionalShadowBalanceRecovery(
				RetainedEvaluation.Metrics,
				RecoveryEvaluation.Metrics))
			{
				OutBGRA = MoveTemp(ShadowBalanceBGRA);
				RetainedEvaluation = RecoveryEvaluation;
			}
		}
		SetCaptureRecoveryLightsVisible(false, false, false);
	}

	if (ShouldAttemptDimensionalReadabilityFloorRecovery(
		RetainedEvaluation.Metrics,
		bSuppressDirectSpecularHighlights))
	{
		SetCaptureExposureDelta(CaptureComponent, World, 0.50f);
		SetCaptureRecoveryLightsVisible(true, true, true);
		TArray<FColor> FloorRecoveryBGRA;
		if (CaptureIntoPixels(FloorRecoveryBGRA))
		{
			const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
				FloorRecoveryBGRA,
				Resolution,
				GeometryMaskPtr);
			LogCaptureEvaluation(
				TEXT("one-shot"),
				Subject,
				TEXT("dimensional shadow balance +0.5 EV"),
				RecoveryEvaluation);
			if (PreferDimensionalReadabilityFloorRecovery(
				RetainedEvaluation.Metrics,
				RecoveryEvaluation.Metrics))
			{
				OutBGRA = MoveTemp(FloorRecoveryBGRA);
				RetainedEvaluation = RecoveryEvaluation;
			}
		}
		SetCaptureRecoveryLightsVisible(false, false, false);
		SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());
	}

	// A true metal can remain nearly black under the neutral diffuse-first fill.
	// Only dimensional subjects that are still unreadable receive this second,
	// off-axis key probe. Planar artwork never reaches this path, preserving the
	// specular-free capture that prevents reflection discs over paintings.
	if ((!bSuppressDirectSpecularHighlights
			|| bAllowSuppressedSpecularRecovery)
		&& NeedsReadabilityRecovery(RetainedEvaluation))
	{
		CaptureComponent->ShowFlags.SetSpecular(true);
		SetCaptureRecoveryLightsVisible(true, true);
		const float KeyExposureDeltas[] = {
			FallbackKeyInitialExposureDelta,
			FallbackKeyBrightExposureDelta,
			FallbackKeyFinalExposureDelta};
		for (const float KeyExposureDelta : KeyExposureDeltas)
		{
			if (!NeedsReadabilityRecovery(RetainedEvaluation))
			{
				break;
			}

			SetCaptureExposureDelta(CaptureComponent, World, KeyExposureDelta);
			TArray<FColor> KeyRecoveryBGRA;
			if (CaptureIntoPixels(KeyRecoveryBGRA))
			{
				const FCaptureEvaluation RecoveryEvaluation = EvaluateCapture(
					KeyRecoveryBGRA,
					Resolution,
					GeometryMaskPtr);
				LogCaptureEvaluation(
					TEXT("one-shot"),
					Subject,
					KeyExposureDelta == FallbackKeyInitialExposureDelta
						? TEXT("dimensional key")
						: KeyExposureDelta == FallbackKeyBrightExposureDelta
							? TEXT("dimensional key bright")
							: TEXT("dimensional key final"),
					RecoveryEvaluation);
				if (PreferSevereDarkDimensionalKeyRecovery(
						RetainedEvaluation.Metrics,
						RecoveryEvaluation.Metrics,
						bSuppressDirectSpecularHighlights,
						bAllowSuppressedSpecularRecovery)
					|| PreferDimensionalKeyRecovery(RetainedEvaluation, RecoveryEvaluation))
				{
					OutBGRA = MoveTemp(KeyRecoveryBGRA);
					RetainedEvaluation = RecoveryEvaluation;
				}
			}
		}
		CaptureComponent->ShowFlags.SetSpecular(!bSuppressDirectSpecularHighlights);
		SetCaptureRecoveryLightsVisible(false, false);
	}
	// Match the live calibration's final exposure-only continuation. The neutral
	// rig and dimensional specular policy stay unchanged; only accepted pixels
	// replace the retained image, and transient exposure is restored below.
	if (UseNeutralCaptureLighting(World)
		&& ShouldAttemptDimensionalReadabilityFloorRecovery(
			RetainedEvaluation.Metrics, bSuppressDirectSpecularHighlights))
	{
		SetCaptureExposureDelta(CaptureComponent, World, NeutralReadabilityExposureDelta);
		TArray<FColor> NeutralRecoveryBGRA;
		if (CaptureIntoPixels(NeutralRecoveryBGRA))
		{
			const FCaptureEvaluation NeutralEvaluation = EvaluateCapture(
				NeutralRecoveryBGRA, Resolution, GeometryMaskPtr);
			const bool bAccepted = PreferDimensionalReadabilityFloorRecovery(
				RetainedEvaluation.Metrics, NeutralEvaluation.Metrics);
			if (bAccepted)
			{
				OutBGRA = MoveTemp(NeutralRecoveryBGRA);
				RetainedEvaluation = NeutralEvaluation;
			}
			LogCaptureEvaluation(TEXT("one-shot"), Subject, TEXT("neutral readability +3.5 EV"), NeutralEvaluation);
			UE_LOG(LogConvaiSceneAutoTaggerCapture, Verbose,
				TEXT("one-shot '%s' neutral readability +3.5 EV accepted=%s"),
				*Subject, bAccepted ? TEXT("true") : TEXT("false"));
		}
	}
	SetCaptureExposureDelta(CaptureComponent, World, TOptional<float>());

	if (!HasUsefulCapturedAppearance(RetainedEvaluation))
	{
		OutBGRA.Reset();
		OutError = bHasGeometryMask
			? TEXT("unreadable_dark_capture")
			: TEXT("empty_capture");
		return false;
	}

	return true;
}

bool CompositeGridPng(
	const TArray<TArray<FColor>>& OccupiedCells,
	const int32 GridN,
	const int32 CellPixels,
	const int32 OutputPixels,
	TArray64<uint8>& OutPngBytes,
	FString& OutError)
{
	OutError.Reset();
	OutPngBytes.Reset();

	if (GridN <= 0 || GridN > 16 || CellPixels <= 0 || OutputPixels <= 0
		|| OutputPixels > 4096 || OutputPixels % GridN != 0)
	{
		OutError = TEXT("grid composite failed: invalid dimensions");
		return false;
	}
	const int32 Capacity = GridN * GridN;
	const int32 FilledCount = OccupiedCells.Num();
	if (FilledCount <= 0 || FilledCount > Capacity)
	{
		OutError = TEXT("grid composite failed: occupied cell count exceeds grid capacity or is empty");
		return false;
	}
	for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
	{
		if (OccupiedCells[CellIndex].Num() != CellPixels * CellPixels)
		{
			OutError = FString::Printf(
				TEXT("grid composite failed: occupied cell %d has an invalid pixel buffer"),
				CellIndex + 1);
			return false;
		}
	}

	const int32 OutputCellPixels = OutputPixels / GridN;
	TArray<FColor> Composite;
	Composite.Init(FColor(24, 24, 26, 255), OutputPixels * OutputPixels);
	for (int32 CellIndex = 0; CellIndex < FilledCount; ++CellIndex)
	{
		const int32 Row = CellIndex / GridN;
		const int32 Column = CellIndex % GridN;
		const int32 DestinationX = Column * OutputCellPixels;
		const int32 DestinationY = Row * OutputCellPixels;
		const TArray<FColor>& Cell = OccupiedCells[CellIndex];
		for (int32 Y = 0; Y < OutputCellPixels; ++Y)
		{
			const int32 SourceY = FMath::Clamp((Y * CellPixels) / OutputCellPixels, 0, CellPixels - 1);
			for (int32 X = 0; X < OutputCellPixels; ++X)
			{
				const int32 SourceX = FMath::Clamp((X * CellPixels) / OutputCellPixels, 0, CellPixels - 1);
				Composite[(DestinationY + Y) * OutputPixels + DestinationX + X] =
					Cell[SourceY * CellPixels + SourceX];
			}
		}
		DrawNumber(Composite, OutputPixels, OutputPixels,
			DestinationX + 10, DestinationY + 10, CellIndex + 1);
	}
	const FColor SeparatorColor(85, 88, 92, 255);
	for (int32 Line = 0; Line <= GridN; ++Line)
	{
		const int32 Position = FMath::Clamp(Line * OutputCellPixels, 0, OutputPixels - 1);
		FillRect(Composite, OutputPixels, OutputPixels, Position, 0, 2, OutputPixels, SeparatorColor);
		FillRect(Composite, OutputPixels, OutputPixels, 0, Position, OutputPixels, 2, SeparatorColor);
	}

	return EncodePng(Composite, OutputPixels, OutputPixels, OutPngBytes, OutError);
}

bool EncodePng(
	const TArray<FColor>& BGRA,
	const int32 Width,
	const int32 Height,
	TArray64<uint8>& OutPngBytes,
	FString& OutError)
{
	OutPngBytes.Reset();
	OutError.Reset();
	if (Width <= 0 || Height <= 0 || BGRA.Num() != Width * Height)
	{
		OutError = TEXT("PNG encode failed: invalid BGRA pixel buffer");
		return false;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!ImageWrapper.IsValid())
	{
		OutError = TEXT("PNG encode failed: could not create encoder");
		return false;
	}
	if (!ImageWrapper->SetRaw(
		BGRA.GetData(),
		static_cast<int64>(BGRA.Num()) * sizeof(FColor),
		Width,
		Height,
		ERGBFormat::BGRA,
		8))
	{
		OutError = TEXT("PNG encode failed: encoder rejected raw pixels");
		return false;
	}

	OutPngBytes = ImageWrapper->GetCompressed(90);
	if (OutPngBytes.IsEmpty())
	{
		OutError = TEXT("PNG encode failed: encoder returned no data");
		return false;
	}
	return true;
}

bool SavePngDebug(
	const FString& AbsoluteFilePath,
	const TArray64<uint8>& PngBytes,
	FString& OutError)
{
	OutError.Reset();
	if (AbsoluteFilePath.IsEmpty() || FPaths::IsRelative(AbsoluteFilePath) || PngBytes.IsEmpty())
	{
		OutError = TEXT("PNG debug save failed: expected an absolute path and non-empty PNG data");
		return false;
	}

	const FString Directory = FPaths::GetPath(AbsoluteFilePath);
	if (!IFileManager::Get().MakeDirectory(*Directory, true) && !IFileManager::Get().DirectoryExists(*Directory))
	{
		OutError = FString::Printf(TEXT("PNG debug save failed: could not create directory '%s'"), *Directory);
		return false;
	}

	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*AbsoluteFilePath));
	if (!Writer)
	{
		OutError = FString::Printf(TEXT("PNG debug save failed: could not open '%s'"), *AbsoluteFilePath);
		return false;
	}
	Writer->Serialize(const_cast<uint8*>(PngBytes.GetData()), PngBytes.Num());
	const bool bSucceeded = !Writer->IsError() && Writer->Close();
	if (!bSucceeded)
	{
		OutError = FString::Printf(TEXT("PNG debug save failed while writing '%s'"), *AbsoluteFilePath);
		return false;
	}
	return true;
}

bool SavePixelsPngDebug(
	const FString& AbsoluteFilePath,
	const TArray<FColor>& BGRA,
	const int32 Width,
	const int32 Height,
	FString& OutError)
{
	TArray64<uint8> PngBytes;
	if (!EncodePng(BGRA, Width, Height, PngBytes, OutError))
	{
		return false;
	}
	return SavePngDebug(AbsoluteFilePath, PngBytes, OutError);
}
}
