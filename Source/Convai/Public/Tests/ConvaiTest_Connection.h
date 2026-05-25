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
#include "ConvaiTest_Connection.generated.h"

class UConvaiChatbotComponent;

/**
 * Low-level integration check for the DLL's session lifecycle.
 *
 * Flow:
 *   1. Spawn a throwaway actor with a UConvaiChatbotComponent for Context.CharacterID.
 *   2. Call StartSession(), poll GetChatbotConnectionState() every tick.
 *   3. Record handshake time when state -> Connected.
 *   4. StopSession(), wait until Disconnected.
 *   5. StartSession() a second time to validate reconnect.
 *   6. PASS when second connection completes, else FAIL with a tagged reason.
 *
 * Also subscribes to OnFailureEvent so any DLL-side error flip is captured into the trace.
 */
UCLASS()
class CONVAI_API UConvaiTest_Connection : public UConvaiTestBase
{
	GENERATED_BODY()

public:
	virtual FString GetTestName() const override { return TEXT("Connection"); }

protected:
	virtual bool Setup() override;
	virtual void Execute() override;
	virtual void Teardown() override;

private:
	enum class EPhase : uint8
	{
		Idle,
		WaitingForFirstConnect,
		WaitingForDisconnect,
		WaitingForSecondConnect,
	};

	void StartPollTimer();
	void StopPollTimer();
	void PollConnection();
	void HandlePhaseTransition(EPhase NewPhase);

	UFUNCTION()
	void OnChatbotFailure();

	UPROPERTY()
	AActor* OwnerActor = nullptr;

	UPROPERTY()
	UConvaiChatbotComponent* Chatbot = nullptr;

	FTimerHandle PollHandle;
	EPhase Phase = EPhase::Idle;
	double PhaseStartMs = 0.0;
};
