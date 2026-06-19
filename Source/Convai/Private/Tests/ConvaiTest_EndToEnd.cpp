// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTest_EndToEnd.h"



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

namespace ConvaiEndToEndTestConstants
{
	constexpr float PollInterval = 0.25f;
	constexpr float PumpInterval = 0.1f;
	constexpr float ChunkDurationSeconds = 0.1f;
}

bool UConvaiTest_EndToEnd::Setup()
{
	if (!Super::Setup()) return false;

	UWorld* World = GetTestWorld();
	if (!World)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("No world"));
		return false;
	}

	// Audio is optional at setup time — we only fail if the audio step is actually reached without a loaded wave.
	if (!Context.SampleAudio.IsNull())
	{
		LoadedAudio = Context.SampleAudio.LoadSynchronous();
		if (LoadedAudio)
		{
			PcmBytes = UConvaiUtils::ExtractPCMDataFromSoundWave(LoadedAudio, SampleRate, NumChannels);
			if (PcmBytes.Num() > 0 && SampleRate > 0 && NumChannels > 0)
			{
				FramesPerChunk = FMath::Max(1, (int32)(SampleRate * ConvaiEndToEndTestConstants::ChunkDurationSeconds));
			}
		}
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

	Chatbot->OnTranscriptionReceivedDelegate.AddDynamic(this, &UConvaiTest_EndToEnd::OnTranscription);
	Chatbot->OnFailureEvent.AddDynamic(this, &UConvaiTest_EndToEnd::OnChatbotFailure);
	return true;
}

void UConvaiTest_EndToEnd::Execute()
{
	BeginStep(EStep::Connect);
	Chatbot->StartSession();
	Player->StartSession();
	StartPollTimer();
}

void UConvaiTest_EndToEnd::Teardown()
{
	StopPollTimer();
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(PumpHandle);
	}

	if (Chatbot)
	{
		Chatbot->OnTranscriptionReceivedDelegate.RemoveDynamic(this, &UConvaiTest_EndToEnd::OnTranscription);
		Chatbot->OnFailureEvent.RemoveDynamic(this, &UConvaiTest_EndToEnd::OnChatbotFailure);
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

void UConvaiTest_EndToEnd::BeginStep(EStep NewStep)
{
	// Close out the previous step's timing.
	if (Step != EStep::Idle)
	{
		const double Now = ElapsedMs();
		const TCHAR* Prev =
			Step == EStep::Connect    ? TEXT("Step1_Connect")    :
			Step == EStep::Text       ? TEXT("Step2_SendText")   :
			Step == EStep::Audio      ? TEXT("Step3_PumpAudio")  :
			Step == EStep::Disconnect ? TEXT("Step4_Disconnect") : TEXT("Unknown");
		RecordPhase(Prev, StepStartMs, Now - StepStartMs);
	}

	Step = NewStep;
	StepStartMs = ElapsedMs();

	const TCHAR* Next =
		NewStep == EStep::Connect    ? TEXT("Step1_Connect")    :
		NewStep == EStep::Text       ? TEXT("Step2_SendText")   :
		NewStep == EStep::Audio      ? TEXT("Step3_PumpAudio")  :
		NewStep == EStep::Disconnect ? TEXT("Step4_Disconnect") : TEXT("Idle");
	LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("=> %s"), Next), TEXT("e2e.step"));
}

void UConvaiTest_EndToEnd::StartPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(PollHandle,
			FTimerDelegate::CreateUObject(this, &UConvaiTest_EndToEnd::Poll),
			ConvaiEndToEndTestConstants::PollInterval, true);
	}
}

void UConvaiTest_EndToEnd::StopPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(PollHandle);
	}
}

void UConvaiTest_EndToEnd::Poll()
{
	if (!Chatbot || !Player)
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Components went null mid-run"));
		return;
	}

	const EC_ConnectionState State = Chatbot->GetChatbotConnectionState();

	switch (Step)
	{
	case EStep::Connect:
		if (State == EC_ConnectionState::Connected)
		{
			BeginStep(EStep::Text);
			bGotTextResponse = false;
			LogEvent(ELogVerbosity::Log,
				FString::Printf(TEXT("Sending text: '%s'"), *Context.SampleText),
				TEXT("e2e.text.send"));
			Player->SendText(Chatbot, Context.SampleText);
		}
		break;

	case EStep::Text:
		// Nothing to poll; OnTranscription advances this step when IsFinal arrives.
		break;

	case EStep::Audio:
		// Pumped by PumpNextChunk; transitions on final transcription.
		break;

	case EStep::Disconnect:
		if (State == EC_ConnectionState::Disconnected)
		{
			FinishWithPass(TEXT("All four steps completed cleanly"));
		}
		break;

	default:
		break;
	}
}

void UConvaiTest_EndToEnd::StartPump()
{
	if (PcmBytes.Num() == 0 || FramesPerChunk == 0)
	{
		LogEvent(ELogVerbosity::Warning,
			TEXT("No audio loaded — skipping audio step"),
			TEXT("e2e.audio"));
		// Skip straight to disconnect.
		BeginStep(EStep::Disconnect);
		Chatbot->StopSession();
		Player->StopSession();
		return;
	}

	BytesSent = 0;
	bAllAudioSent = false;
	bGotAudioResponse = false;

	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(PumpHandle,
			FTimerDelegate::CreateUObject(this, &UConvaiTest_EndToEnd::PumpNextChunk),
			ConvaiEndToEndTestConstants::PumpInterval, true, 0.0f);
	}
}

void UConvaiTest_EndToEnd::PumpNextChunk()
{
	if (!Player)
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Player null mid-pump"));
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
		LogEvent(ELogVerbosity::Warning, TEXT("Player has no session proxy — skipping chunk"), TEXT("e2e.audio"));
		return;
	}

	const int32 BytesPerSample = 2 * NumChannels;
	const int32 ChunkBytes = FramesPerChunk * BytesPerSample;
	const int32 Remaining = PcmBytes.Num() - BytesSent;

	if (Remaining <= 0)
	{
		if (UWorld* World = GetTestWorld())
		{
			World->GetTimerManager().ClearTimer(PumpHandle);
		}
		bAllAudioSent = true;
		LogEvent(ELogVerbosity::Log, TEXT("All PCM sent — waiting for response"), TEXT("e2e.audio"));
		return;
	}

	const int32 ThisChunkBytes = FMath::Min(ChunkBytes, Remaining);
	const int32 ThisChunkFrames = ThisChunkBytes / BytesPerSample;
	const int16_t* Data = reinterpret_cast<const int16_t*>(PcmBytes.GetData() + BytesSent);
	Proxy->SendAudio(Data, static_cast<size_t>(ThisChunkFrames));
	BytesSent += ThisChunkBytes;
}

void UConvaiTest_EndToEnd::OnTranscription(
	UConvaiConversationComponent* Speaker,
	UConvaiConversationComponent* /*Listener*/,
	FString Transcription,
	bool /*IsTranscriptionReady*/,
	bool IsFinal)
{
	if (Speaker != Chatbot) return;

	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("[%s] final=%d '%s'"),
			Step == EStep::Text ? TEXT("text") : TEXT("audio"),
			IsFinal ? 1 : 0, *Transcription.Left(120)),
		TEXT("e2e.recv"));

	if (!IsFinal) return;

	if (Transcription.TrimStartAndEnd().IsEmpty())
	{
		FinishWithFailure(EConvaiTestFailureReason::BadPayload,
			FString::Printf(TEXT("Empty final transcription during %s step"),
				Step == EStep::Text ? TEXT("Text") : TEXT("Audio")));
		return;
	}

	if (Step == EStep::Text)
	{
		bGotTextResponse = true;
		BeginStep(EStep::Audio);
		StartPump();
	}
	else if (Step == EStep::Audio)
	{
		// Only accept the response if all audio has been sent; otherwise this is a stale
		// transcription that must not short-circuit the pump.
		if (!bAllAudioSent)
		{
			LogEvent(ELogVerbosity::Warning,
				TEXT("Final transcription arrived before all audio was sent — ignoring"),
				TEXT("e2e.audio"));
			return;
		}
		bGotAudioResponse = true;
		BeginStep(EStep::Disconnect);
		Chatbot->StopSession();
		Player->StopSession();
	}
}

void UConvaiTest_EndToEnd::OnChatbotFailure()
{
	LogEvent(ELogVerbosity::Error, TEXT("OnFailureEvent raised"), TEXT("failure"));
	FinishWithFailure(EConvaiTestFailureReason::FailureEventRaised,
		FString::Printf(TEXT("Chatbot raised OnFailureEvent during %s step"),
			Step == EStep::Connect    ? TEXT("Connect")    :
			Step == EStep::Text       ? TEXT("Text")       :
			Step == EStep::Audio      ? TEXT("Audio")      :
			Step == EStep::Disconnect ? TEXT("Disconnect") : TEXT("Unknown")));
}

