// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Engine/TimerHandle.h"
#else
#include "Engine/EngineTypes.h"
#endif
#include "ConvaiDefinitions.h"
#include "Tests/ConvaiTestBase.h"
#include "ConvaiTest_ActionResponse.generated.h"

class AActor;
class UConvaiChatbotComponent;
class UConvaiPlayerComponent;
class UConvaiConversationComponent;

/**
 * Live regression for the parameterized realtime-action compatibility report.
 *
 * Invoke each single-action control independently:
 *   Text=execute_option_A.
 *   Text=Quiero_una_rutina_para_la_piel_sensible.
 *   Text=Recomienda_y_muestra_el_producto_Sensibio.
 *   Text=Show_me_the_routine_for_sensitive_skin.
 *   Text=Follow_me.
 *
 * The selected prompt controls the advertised action catalogue. Each control
 * deliberately advertises one action so model selection and client parsing
 * can be diagnosed independently. No level assets are created: the harness
 * spawns transient components, uses project credentials, and writes its trace
 * under Saved/ConvaiTests.
 */
UCLASS()
class CONVAI_API UConvaiTest_ActionResponse : public UConvaiTestBase
{
	GENERATED_BODY()

public:
	virtual FString GetTestName() const override { return TEXT("ActionResponse"); }

protected:
	virtual bool Setup() override;
	virtual void Execute() override;
	virtual void Teardown() override;

private:
	enum class EPhase : uint8
	{
		Idle,
		WaitingForConnect,
		WaitingForAction,
	};

	void StartPollTimer();
	void StopPollTimer();
	void PollConnectionReady();

	UFUNCTION()
	void OnActionsReceived(
		UConvaiChatbotComponent* InChatbot,
		UConvaiPlayerComponent* InPlayer,
		const TArray<FConvaiResultAction>& Actions);

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

	FTimerHandle PollHandle;
	EPhase Phase = EPhase::Idle;
	FString Prompt;
	double ChatbotConnectedAtMs = -1.0;
	double SendAtMs = -1.0;
	bool bFollowScenario = false;
	bool bParameterizedControlScenario = false;
	bool bSpanishRoutineScenario = false;
	bool bProductScenario = false;
};
