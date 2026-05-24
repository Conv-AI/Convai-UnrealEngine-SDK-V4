// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
// Engine/TimerHandle.h was extracted from EngineTypes.h in UE 5.1. On 5.0
// the type still lives inside EngineTypes.h, so include that instead to
// keep this header self-contained (don't rely on PCH transitivity).
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Engine/TimerHandle.h"
#else
#include "Engine/EngineTypes.h"
#endif
#include "Tests/ConvaiTestBase.h"
#include "ConvaiTest_EndToEnd.generated.h"

class UConvaiChatbotComponent;
class UConvaiPlayerComponent;
class UConvaiConversationComponent;
class USoundWave;

/**
 * Full regression scenario: connect once, then exercise text and audio modalities on the same session,
 * then disconnect. Designed so a single failure gives a clear picture of which modality broke.
 *
 * Steps (each a named phase in the run record):
 *   Step1_Connect      — StartSession + wait for Connected.
 *   Step2_SendText     — Player->SendText, wait for final chatbot transcription.
 *   Step3_PumpAudio    — Pump Context.SampleAudio chunks, wait for final chatbot transcription.
 *   Step4_Disconnect   — StopSession + wait for Disconnected.
 *
 * Any failure short-circuits remaining steps.
 */
UCLASS()
class CONVAI_API UConvaiTest_EndToEnd : public UConvaiTestBase
{
	GENERATED_BODY()

public:
	virtual FString GetTestName() const override { return TEXT("EndToEnd"); }

protected:
	virtual bool Setup() override;
	virtual void Execute() override;
	virtual void Teardown() override;

private:
	enum class EStep : uint8
	{
		Idle,
		Connect,
		Text,
		Audio,
		Disconnect,
	};

	void BeginStep(EStep NewStep);
	void StartPollTimer();
	void StopPollTimer();
	void Poll();
	void StartPump();
	void PumpNextChunk();

	UFUNCTION()
	void OnTranscription(
		UConvaiConversationComponent* Speaker,
		UConvaiConversationComponent* Listener,
		FString Transcription,
		bool IsTranscriptionReady,
		bool IsFinal);

	UFUNCTION()
	void OnChatbotFailure();

	UPROPERTY()
	AActor* OwnerActor = nullptr;

	UPROPERTY()
	UConvaiChatbotComponent* Chatbot = nullptr;

	UPROPERTY()
	UConvaiPlayerComponent* Player = nullptr;

	UPROPERTY()
	USoundWave* LoadedAudio = nullptr;

	FTimerHandle PollHandle;
	FTimerHandle PumpHandle;
	EStep Step = EStep::Idle;
	double StepStartMs = 0.0;

	// Text step
	bool bGotTextResponse = false;

	// Audio step
	TArray<uint8> PcmBytes;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
	int32 BytesSent = 0;
	int32 FramesPerChunk = 0;
	bool bAllAudioSent = false;
	bool bGotAudioResponse = false;
};
