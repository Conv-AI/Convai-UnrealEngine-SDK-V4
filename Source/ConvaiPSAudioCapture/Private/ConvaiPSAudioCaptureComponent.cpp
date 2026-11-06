// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiPSAudioCaptureComponent.h"
#include "Utility/Log/ConvaiLogger.h"
#include "Sound/SoundSubmix.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(ConvaiPSAudioCaptureLog);

namespace
{
	bool IsPixelStreamingLaunch()
	{
		const TCHAR* CmdLine = FCommandLine::Get();
		FString Unused;
		return FParse::Param(CmdLine, TEXT("PixelStreaming")) ||
			FParse::Value(CmdLine, TEXT("PixelStreamingURL="), Unused) ||
			FParse::Value(CmdLine, TEXT("PixelStreamingIP="), Unused);
	}

	const TCHAR* const RemovingMessage =
		TEXT("No Pixel Streaming launch flag (-PixelStreamingURL/-PixelStreaming) - removing "
		     "ConvaiPSAudioCaptureComponent so the Convai Player keeps the local microphone.");
}

UConvaiPSAudioCaptureComponent::UConvaiPSAudioCaptureComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bWantsInitializeComponent = true;

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

void UConvaiPSAudioCaptureComponent::InitializeComponent()
{
	Super::InitializeComponent();

	// Only take over Convai's audio capture when Pixel Streaming is actually running. The Convai
	// Player adopts any IConvaiAudioCaptureInterface component on its owner in its own BeginPlay and
	// stops the local microphone, so without a browser peer this component would leave the app with
	// no microphone. An owner's components are initialized before any of them begins play, so going
	// here means the player never sees us. Not once the owner has begun play: the engine then calls
	// BeginPlay right after this, and BeginPlay on a destroyed component fails check(bRegistered).
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!IsPixelStreamingLaunch() && World && World->IsGameWorld()
		&& Owner && !Owner->HasActorBegunPlay() && !Owner->IsActorBeginningPlay())
	{
		CONVAI_LOG(ConvaiPSAudioCaptureLog, Log, TEXT("%s"), RemovingMessage);
		DestroyComponent();
	}
}

void UConvaiPSAudioCaptureComponent::BeginPlay()
{
	Super::BeginPlay();

	// Reached without a flag only when the component was registered after its owner began play
	// (InitializeComponent removes it otherwise), so the player may already have adopted it; its
	// mic health check hands the microphone back to the built-in device once we are gone.
	// Destroying inside BeginPlay itself resets HasBegunPlay() and trips AActor::BeginPlay's "Failed
	// to route BeginPlay" ensure, so wait a tick.
	if (!IsPixelStreamingLaunch())
	{
		CONVAI_LOG(ConvaiPSAudioCaptureLog, Log, TEXT("%s"), RemovingMessage);
		GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			DestroyComponent();
		}));
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
