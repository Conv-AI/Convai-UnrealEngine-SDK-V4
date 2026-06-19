// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTest_Audio.h"



#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundWave.h"
#include "TimerManager.h"

namespace ConvaiAudioTestConstants
{
	constexpr float PollInterval = 0.25f;
	// Pump cadence: one 100ms chunk every 100ms so the wire sees realistic streaming pacing.
	constexpr float PumpInterval = 0.1f;
	constexpr float ChunkDurationSeconds = 0.1f;
}

bool UConvaiTest_Audio::Setup()
{
	if (!Super::Setup()) return false;

	if (Context.SampleAudio.IsNull())
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError,
			TEXT("Context.SampleAudio is not set — the audio test needs a pre-recorded USoundWave"));
		return false;
	}

	LoadedAudio = Context.SampleAudio.LoadSynchronous();
	if (!LoadedAudio)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError,
			FString::Printf(TEXT("Failed to load USoundWave at '%s'"), *Context.SampleAudio.ToString()));
		return false;
	}

	PcmBytes = UConvaiUtils::ExtractPCMDataFromSoundWave(LoadedAudio, SampleRate, NumChannels);
	if (PcmBytes.Num() == 0 || SampleRate <= 0 || NumChannels <= 0)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError,
			FString::Printf(TEXT("PCM extract failed: bytes=%d rate=%d ch=%d"),
				PcmBytes.Num(), SampleRate, NumChannels));
		return false;
	}

	FramesPerChunk = FMath::Max(1, (int32)(SampleRate * ConvaiAudioTestConstants::ChunkDurationSeconds));
	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("Loaded audio: bytes=%d rate=%d ch=%d framesPerChunk=%d"),
			PcmBytes.Num(), SampleRate, NumChannels, FramesPerChunk),
		TEXT("audio.load"));

	UWorld* World = GetTestWorld();
	if (!World)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("No world"));
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	OwnerActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);

	Chatbot = NewObject<UConvaiChatbotComponent>(OwnerActor, UConvaiChatbotComponent::StaticClass(), NAME_None, RF_Transient);
	Chatbot->CharacterID = Context.CharacterID;
	Chatbot->bAutoInitializeSession = false;
	Chatbot->RegisterComponent();
	OwnerActor->AddInstanceComponent(Chatbot);
	Chatbot->RegisterComponentWithWorld(World);

	Player = NewObject<UConvaiPlayerComponent>(OwnerActor, UConvaiPlayerComponent::StaticClass(), NAME_None, RF_Transient);
	Player->bAutoInitializeSession = false;
	Player->RegisterComponent();
	OwnerActor->AddInstanceComponent(Player);
	Player->RegisterComponentWithWorld(World);

	Chatbot->OnTranscriptionReceivedDelegate.AddDynamic(this, &UConvaiTest_Audio::OnTranscription);
	Chatbot->OnFailureEvent.AddDynamic(this, &UConvaiTest_Audio::OnChatbotFailure);
	return true;
}

void UConvaiTest_Audio::Execute()
{
	Phase = EPhase::WaitingForConnect;
	Chatbot->StartSession();
	Player->StartSession();
	StartPollTimer();
}

void UConvaiTest_Audio::Teardown()
{
	StopTimers();

	if (Chatbot)
	{
		Chatbot->OnTranscriptionReceivedDelegate.RemoveDynamic(this, &UConvaiTest_Audio::OnTranscription);
		Chatbot->OnFailureEvent.RemoveDynamic(this, &UConvaiTest_Audio::OnChatbotFailure);
		Chatbot->StopSession();
		Chatbot = nullptr;
	}
	if (Player)
	{
		Player->StopSession();
		Player = nullptr;
	}
	if (OwnerActor)
	{
		OwnerActor->Destroy();
		OwnerActor = nullptr;
	}
	LoadedAudio = nullptr;
	PcmBytes.Reset();
}

void UConvaiTest_Audio::StartPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(PollHandle,
			FTimerDelegate::CreateUObject(this, &UConvaiTest_Audio::PollConnectionReady),
			ConvaiAudioTestConstants::PollInterval, true);
	}
}

void UConvaiTest_Audio::StartPumpTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(PumpHandle,
			FTimerDelegate::CreateUObject(this, &UConvaiTest_Audio::PumpNextChunk),
			ConvaiAudioTestConstants::PumpInterval, true, 0.0f);
	}
}

void UConvaiTest_Audio::StopTimers()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(PollHandle);
		World->GetTimerManager().ClearTimer(PumpHandle);
	}
}

void UConvaiTest_Audio::PollConnectionReady()
{
	if (!Chatbot || !Player)
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Components went null"));
		return;
	}

	if (Phase != EPhase::WaitingForConnect) return;

	if (Chatbot->GetChatbotConnectionState() == EC_ConnectionState::Connected)
	{
		RecordPhase(TEXT("handshake"), 0.0, ElapsedMs());
		LogEvent(ELogVerbosity::Log, TEXT("Connected — starting audio pump"), TEXT("audio.begin"));

		Phase = EPhase::Pumping;
		PumpStartMs = ElapsedMs();
		BytesSent = 0;
		StartPumpTimer();
	}
}

void UConvaiTest_Audio::PumpNextChunk()
{
	if (!Player)
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Player went null mid-pump"));
		return;
	}
#ifdef WITH_CONVAI_TESTS
	#if WITH_CONVAI_TESTS
		UConvaiConnectionSessionProxy* Proxy = Player->GetSessionProxyForTesting();
	#else 
		UConvaiConnectionSessionProxy* Proxy = nullptr;
	#endif
#endif
	
	if (!Proxy)
	{
		LogEvent(ELogVerbosity::Warning, TEXT("Player has no active session proxy — skipping chunk"), TEXT("audio.pump"));
		return;
	}

	const int32 BytesPerSample = 2 * NumChannels; // 16-bit PCM
	const int32 ChunkBytes = FramesPerChunk * BytesPerSample;
	const int32 Remaining = PcmBytes.Num() - BytesSent;

	if (Remaining <= 0)
	{
		// Done streaming; flip to waiting for response.
		if (UWorld* World = GetTestWorld())
		{
			World->GetTimerManager().ClearTimer(PumpHandle);
		}
		RecordPhase(TEXT("audio_pump"), PumpStartMs, ElapsedMs() - PumpStartMs);
		Phase = EPhase::WaitingForResponse;
		LogEvent(ELogVerbosity::Log, TEXT("All PCM sent — waiting for response"), TEXT("audio.pump"));
		return;
	}

	const int32 ThisChunkBytes = FMath::Min(ChunkBytes, Remaining);
	const int32 ThisChunkFrames = ThisChunkBytes / BytesPerSample;
	const int16_t* Data = reinterpret_cast<const int16_t*>(PcmBytes.GetData() + BytesSent);

	const int32 Result = Proxy->SendAudio(Data, static_cast<size_t>(ThisChunkFrames));
	LogEvent(ELogVerbosity::Verbose,
		FString::Printf(TEXT("sent frames=%d bytes=%d rc=%d progress=%d/%d"),
			ThisChunkFrames, ThisChunkBytes, Result, BytesSent + ThisChunkBytes, PcmBytes.Num()),
		TEXT("audio.pump"));

	BytesSent += ThisChunkBytes;
}

void UConvaiTest_Audio::OnTranscription(
	UConvaiConversationComponent* Speaker,
	UConvaiConversationComponent* /*Listener*/,
	FString Transcription,
	bool /*IsTranscriptionReady*/,
	bool IsFinal)
{
	if (Speaker != Chatbot) return;

	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("transcription final=%d '%s'"), IsFinal ? 1 : 0, *Transcription.Left(120)),
		TEXT("audio.recv"));

	if (IsFinal)
	{
		if (Transcription.TrimStartAndEnd().IsEmpty())
		{
			FinishWithFailure(EConvaiTestFailureReason::BadPayload,
				TEXT("Final chatbot transcription was empty"));
			return;
		}
		FinishWithPass(FString::Printf(TEXT("Got %d-char response after audio input"), Transcription.Len()));
	}
}

void UConvaiTest_Audio::OnChatbotFailure()
{
	LogEvent(ELogVerbosity::Error, TEXT("OnFailureEvent raised"), TEXT("failure"));
	FinishWithFailure(EConvaiTestFailureReason::FailureEventRaised,
		TEXT("Chatbot raised OnFailureEvent during audio test"));
}

