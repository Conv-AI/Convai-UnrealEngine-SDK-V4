// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ConvaiConnectionTest.generated.h"

class UConvaiChatbotComponent;
class UConvaiConnectionSessionProxy;
class UConvaiConnectionManager;

DECLARE_LOG_CATEGORY_EXTERN(ConvaiConnectionTestLog, Log, All);

UENUM(BlueprintType)
enum class EConvaiTestScenario : uint8
{
	BasicReuse            UMETA(DisplayName = "Basic Reuse"),
	DifferentCharacterID  UMETA(DisplayName = "Different Character ID"),
	DoubleAcquire         UMETA(DisplayName = "Double Acquire"),
	ExpiryTimeout         UMETA(DisplayName = "Expiry Timeout"),
	RapidCycle            UMETA(DisplayName = "Rapid Cycle"),
};

UENUM()
enum class EConvaiTestPhase : uint8
{
	Idle,
	WaitingForConnection,
	WaitingToRespawn,
	WaitingForExpiry,
};

UCLASS(BlueprintType, Blueprintable)
class CONVAI_API AConvaiConnectionTest : public AActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	void RunScenario(
		EConvaiTestScenario Scenario,
		const FString& InPrimaryCharacterID,
		const FString& InSecondaryCharacterID,
		int32 NumIterations = 3,
		float DelayBetweenSpawns = 2.0f);

	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	void RunAllScenarios(
		const FString& InPrimaryCharacterID,
		const FString& InSecondaryCharacterID,
		float DelayBetweenSpawns = 2.0f);

	UFUNCTION(BlueprintCallable, Category = "Convai|Test")
	void StopTest();

private:
	AActor* SpawnChatbotActor(const FString& CharacterID, UConvaiChatbotComponent*& OutChatbot);
	void DestroyPrimaryChatbot();
	void DestroySecondaryChatbot();
	void OnTimerTick();
	void OnConnectionEstablished();
	void FinishScenario(bool bSuccess, const FString& Reason);
	void AdvanceScenarioQueue();
	UConvaiConnectionManager* GetConnectionManager() const;

	static FString GetScenarioName(EConvaiTestScenario Scenario);

	EConvaiTestScenario ActiveScenario = EConvaiTestScenario::BasicReuse;
	EConvaiTestPhase Phase = EConvaiTestPhase::Idle;

	FString PrimaryCharacterID;
	FString SecondaryCharacterID;
	int32 TotalIterations = 0;
	int32 CurrentIteration = 0;
	float RespawnDelay = 2.0f;
	float QueuedDelay = 2.0f;
	float ConnectionTimeout = 30.0f;
	float PhaseElapsed = 0.0f;
	bool bFirstConnectionDone = false;
	bool bRunningAll = false;

	const UConvaiConnectionSessionProxy* TrackedProxy = nullptr;

	FTimerHandle TickTimerHandle;

	UPROPERTY()
	AActor* PrimaryActor = nullptr;

	UPROPERTY()
	UConvaiChatbotComponent* PrimaryChatbot = nullptr;

	UPROPERTY()
	AActor* SecondaryActor = nullptr;

	UPROPERTY()
	UConvaiChatbotComponent* SecondaryChatbot = nullptr;

	TArray<EConvaiTestScenario> ScenarioQueue;
	int32 PassCount = 0;
	int32 FailCount = 0;
};
