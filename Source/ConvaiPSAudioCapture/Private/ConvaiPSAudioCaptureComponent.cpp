// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiPSAudioCaptureComponent.h"
#include "Utility/Log/ConvaiLogger.h"
#include "Sound/SoundSubmix.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(ConvaiPSAudioCaptureLog);

UConvaiPSAudioCaptureComponent::UConvaiPSAudioCaptureComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The base UPixelStreamingAudioComponent auto-finds and attaches to the connected
	// peer's audio sink from its TickComponent (bAutoFindPeer). Ticking must stay enabled
	// or the component never binds to a player that connects after Start() and captures
	// only silence. The tick early-outs once it is listening, so the cost is negligible.
	PrimaryComponentTick.bCanEverTick = true;

	// Default the output submix to Convai's AudioInput submix. The ConvaiPlayer captures
	// voice by recording exactly this submix, so anything rendered elsewhere is never
	// heard. Setting it here means a user only has to add the component - no manual
	// "pick the submix" step - and the component renders to the right place from the
	// first frame. If the asset is missing the finder fails quietly and the player's
	// RouteAdoptedCaptureComponent still re-routes at runtime.
	static ConstructorHelpers::FObjectFinder<USoundSubmixBase> AudioInputSubmix(TEXT("/ConvAI/Submixes/AudioInput.AudioInput"));
	if (AudioInputSubmix.Succeeded())
	{
		SoundSubmix = AudioInputSubmix.Object;
	}
}

void UConvaiPSAudioCaptureComponent::BeginPlay()
{
	Super::BeginPlay();

	// Only take over Convai's audio capture when Pixel Streaming is actually running.
	// The Convai Player adopts any IConvaiAudioCaptureInterface component on its owner and
	// STOPS the default local microphone. In a normal (non-streamed) build there is no
	// browser peer, so leaving this component in place would give the app a dead mic. Gate
	// on the Pixel Streaming launch flags: if none are present, remove ourselves so the
	// Convai Player keeps its local-mic capture. Adoption happens at session start (well
	// after BeginPlay), so destroying here reliably runs before the player looks for us.
	const TCHAR* CmdLine = FCommandLine::Get();
	FString Unused;
	const bool bPixelStreamingLaunch =
		FParse::Param(CmdLine, TEXT("PixelStreaming")) ||
		FParse::Value(CmdLine, TEXT("PixelStreamingURL="), Unused) ||
		FParse::Value(CmdLine, TEXT("PixelStreamingIP="), Unused);

	if (!bPixelStreamingLaunch)
	{
		CONVAI_LOG(ConvaiPSAudioCaptureLog, Log,
			TEXT("No Pixel Streaming launch flag (-PixelStreamingURL/-PixelStreaming) - removing "
			     "ConvaiPSAudioCaptureComponent so the Convai Player keeps the local microphone."));
		DestroyComponent();
		return;
	}

	CONVAI_LOG(ConvaiPSAudioCaptureLog, Log, TEXT("ConvaiPSAudioCaptureComponent initialized (Pixel Streaming active)"));
}

void UConvaiPSAudioCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Stop();
	Super::EndPlay(EndPlayReason);
}

void UConvaiPSAudioCaptureComponent::Start()
{
	CONVAI_LOG(ConvaiPSAudioCaptureLog, Log, TEXT("Starting Pixel Streaming audio capture"));
	UPixelStreamingAudioComponent::Start();
}

void UConvaiPSAudioCaptureComponent::Stop()
{
	CONVAI_LOG(ConvaiPSAudioCaptureLog, Log, TEXT("Stopping Pixel Streaming audio capture"));
	UPixelStreamingAudioComponent::Stop();
}

void UConvaiPSAudioCaptureComponent::SetVolumeMultiplier(float VolumeMultiplier)
{
	// Multiply by 2 because Pixel Streaming mic is usually very low
	float AdjustedMultiplier = VolumeMultiplier * 2.0f;
	
	CONVAI_LOG(ConvaiPSAudioCaptureLog, Log, 
		TEXT("Setting volume multiplier to %f (adjusted from %f)"), AdjustedMultiplier, VolumeMultiplier);
	
	UPixelStreamingAudioComponent::SetVolumeMultiplier(AdjustedMultiplier);
}
