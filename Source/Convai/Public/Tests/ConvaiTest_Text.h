// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tests/ConvaiTestBase.h"
#include "ConvaiTest_Text.generated.h"

class UConvaiChatbotComponent;
class UConvaiPlayerComponent;
class UConvaiConversationComponent;

/**
 * Sends a text message to a character and validates that a final response transcription arrives.
 *
 * Flow:
 *   1. Spawn an actor with a chatbot + player component.
 *   2. Start both sessions, wait for chatbot to reach Connected.
 *   3. Subscribe to the chatbot's OnTranscriptionReceivedDelegate, OnInteractionIDReceivedEvent, OnFailureEvent.
 *   4. Player->SendText(Chatbot, Context.SampleText).
 *   5. Accumulate incremental transcription; PASS on IsFinal=true with non-empty text.
 *   6. FAIL on timeout, failure event, or empty final response.
 */
UCLASS()
class CONVAI_API UConvaiTest_Text : public UConvaiTestBase
{
	GENERATED_BODY()

public:
	virtual FString GetTestName() const override { return TEXT("Text"); }

protected:
	virtual bool Setup() override;
	virtual void Execute() override;
	virtual void Teardown() override;

private:
	enum class EPhase : uint8
	{
		Idle,
		WaitingForConnect,
		WaitingForResponse,
	};

	void StartPollTimer();
	void StopPollTimer();
	void PollConnectionReady();

	UFUNCTION()
	void OnTranscription(
		UConvaiConversationComponent* Speaker,
		UConvaiConversationComponent* Listener,
		FString Transcription,
		bool IsTranscriptionReady,
		bool IsFinal);

	UFUNCTION()
	void OnInteractionID(
		UConvaiChatbotComponent* InChatbot,
		UConvaiPlayerComponent* InPlayer,
		FString InteractionID);

	UFUNCTION()
	void OnChatbotFailure();

	UPROPERTY()
	AActor* OwnerActor = nullptr;

	UPROPERTY()
	UConvaiChatbotComponent* Chatbot = nullptr;

	UPROPERTY()
	UConvaiPlayerComponent* Player = nullptr;

	FTimerHandle PollHandle;
	EPhase Phase = EPhase::Idle;
	FString AccumulatedResponse;
	double SendAtMs = 0.0;
};
