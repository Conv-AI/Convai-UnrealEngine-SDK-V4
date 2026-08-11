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
#include "ConvaiTest_Audio.generated.h"

class UConvaiChatbotComponent;
class UConvaiPlayerComponent;
class UConvaiConversationComponent;
class USoundWave;

/**
 * Pumps a pre-recorded USoundWave into the Convai session as if it were microphone input,
 * then waits for the character to respond.
 *
 * Flow:
 *   1. Spawn chatbot + player components.
 *   2. Start sessions, wait for chatbot Connected.
 *   3. Load Context.SampleAudio synchronously, extract raw PCM via UConvaiUtils.
 *   4. Schedule chunk pumps on a timer via Player->GetSessionProxyForTesting()->SendAudio().
 *   5. Subscribe to chatbot transcription + audio-data + failure delegates.
 *   6. PASS when a final transcription from the chatbot arrives with non-empty text.
 *   7. FAIL on missing sample audio, failed PCM extract, failure event, or overall timeout.
 */
UCLASS()
class CONVAI_API UConvaiTest_Audio : public UConvaiTestBase
{
	GENERATED_BODY()

public:
	virtual FString GetTestName() const override { return TEXT("Audio"); }

protected:
	virtual bool Setup() override;
	virtual void Execute() override;
	virtual void Teardown() override;

private:
	enum class EPhase : uint8
	{
		Idle,
		WaitingForConnect,
		Pumping,
		WaitingForResponse,
	};

	void StartPollTimer();
	void StartPumpTimer();
	void StopTimers();
	void PollConnectionReady();
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
	EPhase Phase = EPhase::Idle;

	// PCM state
	TArray<uint8> PcmBytes;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
	int32 BytesSent = 0;
	int32 FramesPerChunk = 0;
	double PumpStartMs = 0.0;
};
