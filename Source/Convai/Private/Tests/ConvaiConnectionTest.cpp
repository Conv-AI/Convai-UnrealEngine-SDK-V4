// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiConnectionTest.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiSubsystem.h"
#include "Core/ConvaiConnectionManager.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiUtils.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(ConvaiConnectionTestLog);

static constexpr float TickInterval = 0.5f;

FString AConvaiConnectionTest::GetScenarioName(EConvaiTestScenario Scenario)
{
	switch (Scenario)
	{
	case EConvaiTestScenario::BasicReuse:           return TEXT("BasicReuse");
	case EConvaiTestScenario::DifferentCharacterID: return TEXT("DifferentCharacterID");
	case EConvaiTestScenario::DoubleAcquire:        return TEXT("DoubleAcquire");
	case EConvaiTestScenario::ExpiryTimeout:        return TEXT("ExpiryTimeout");
	case EConvaiTestScenario::RapidCycle:           return TEXT("RapidCycle");
	default:                                        return TEXT("Unknown");
	}
}

UConvaiConnectionManager* AConvaiConnectionTest::GetConnectionManager() const
{
	if (const UGameInstance* GI = GetWorld()->GetGameInstance())
	{
		if (UConvaiSubsystem* Sub = GI->GetSubsystem<UConvaiSubsystem>())
		{
			return Sub->GetConnectionManager();
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void AConvaiConnectionTest::RunScenario(
	EConvaiTestScenario Scenario,
	const FString& InPrimaryCharacterID,
	const FString& InSecondaryCharacterID,
	int32 NumIterations,
	float DelayBetweenSpawns)
{
	if (Phase != EConvaiTestPhase::Idle)
	{
		UE_LOG(ConvaiConnectionTestLog, Warning, TEXT("Test already running — call StopTest() first"));
		return;
	}

	if (InPrimaryCharacterID.IsEmpty())
	{
		UE_LOG(ConvaiConnectionTestLog, Error, TEXT("PrimaryCharacterID cannot be empty"));
		return;
	}

	if (Scenario == EConvaiTestScenario::DifferentCharacterID && InSecondaryCharacterID.IsEmpty())
	{
		UE_LOG(ConvaiConnectionTestLog, Error,
			TEXT("DifferentCharacterID scenario requires a non-empty SecondaryCharacterID"));
		return;
	}

	ActiveScenario = Scenario;
	PrimaryCharacterID = InPrimaryCharacterID;
	SecondaryCharacterID = InSecondaryCharacterID;
	CurrentIteration = 0;
	bFirstConnectionDone = false;
	TrackedProxy = nullptr;

	switch (Scenario)
	{
	case EConvaiTestScenario::BasicReuse:
		TotalIterations = FMath::Max(NumIterations, 2);
		RespawnDelay = FMath::Max(DelayBetweenSpawns, 0.5f);
		break;
	case EConvaiTestScenario::RapidCycle:
		TotalIterations = FMath::Max(NumIterations, 3);
		RespawnDelay = 0.5f;
		break;
	case EConvaiTestScenario::DoubleAcquire:
		TotalIterations = 1;
		RespawnDelay = 0.0f;
		break;
	case EConvaiTestScenario::DifferentCharacterID:
	case EConvaiTestScenario::ExpiryTimeout:
		TotalIterations = 2;
		RespawnDelay = FMath::Max(DelayBetweenSpawns, 0.5f);
		break;
	}

	UE_LOG(ConvaiConnectionTestLog, Log, TEXT("========== SCENARIO: %s =========="), *GetScenarioName(Scenario));
	UE_LOG(ConvaiConnectionTestLog, Log, TEXT("  PrimaryID   : %s"), *PrimaryCharacterID);
	if (!SecondaryCharacterID.IsEmpty())
	{
		UE_LOG(ConvaiConnectionTestLog, Log, TEXT("  SecondaryID : %s"), *SecondaryCharacterID);
	}
	UE_LOG(ConvaiConnectionTestLog, Log, TEXT("  Iterations  : %d"), TotalIterations);
	UE_LOG(ConvaiConnectionTestLog, Log, TEXT("  Delay       : %.1fs"), RespawnDelay);
	UE_LOG(ConvaiConnectionTestLog, Log, TEXT("============================================="));

	CurrentIteration = 1;
	PrimaryActor = SpawnChatbotActor(PrimaryCharacterID, PrimaryChatbot);
	if (!PrimaryActor)
	{
		FinishScenario(false, TEXT("Failed to spawn initial actor"));
		return;
	}

	Phase = EConvaiTestPhase::WaitingForConnection;
	PhaseElapsed = 0.0f;

	GetWorld()->GetTimerManager().SetTimer(
		TickTimerHandle, this, &AConvaiConnectionTest::OnTimerTick, TickInterval, true);
}

void AConvaiConnectionTest::RunAllScenarios(
	const FString& InPrimaryCharacterID,
	const FString& InSecondaryCharacterID,
	float DelayBetweenSpawns)
{
	if (Phase != EConvaiTestPhase::Idle)
	{
		UE_LOG(ConvaiConnectionTestLog, Warning, TEXT("Test already running — call StopTest() first"));
		return;
	}

	ScenarioQueue = {
		EConvaiTestScenario::BasicReuse,
		EConvaiTestScenario::DifferentCharacterID,
		EConvaiTestScenario::DoubleAcquire,
		EConvaiTestScenario::ExpiryTimeout,
		EConvaiTestScenario::RapidCycle,
	};

	PrimaryCharacterID = InPrimaryCharacterID;
	SecondaryCharacterID = InSecondaryCharacterID;
	QueuedDelay = DelayBetweenSpawns;
	PassCount = 0;
	FailCount = 0;
	bRunningAll = true;

	UE_LOG(ConvaiConnectionTestLog, Log,
		TEXT("========== RUNNING ALL %d SCENARIOS =========="), ScenarioQueue.Num());

	AdvanceScenarioQueue();
}

void AConvaiConnectionTest::StopTest()
{
	GetWorld()->GetTimerManager().ClearTimer(TickTimerHandle);
	DestroySecondaryChatbot();
	DestroyPrimaryChatbot();
	Phase = EConvaiTestPhase::Idle;
	ScenarioQueue.Empty();
	bRunningAll = false;
	UE_LOG(ConvaiConnectionTestLog, Warning, TEXT("Test stopped by user"));
}

// ---------------------------------------------------------------------------
// Spawning / Teardown
// ---------------------------------------------------------------------------

AActor* AConvaiConnectionTest::SpawnChatbotActor(
	const FString& CharacterID, UConvaiChatbotComponent*& OutChatbot)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = GetWorld()->SpawnActor<AActor>(
		AActor::StaticClass(), GetActorLocation(), FRotator::ZeroRotator, Params);

	if (!Actor)
	{
		UE_LOG(ConvaiConnectionTestLog, Error,
			TEXT("Failed to spawn actor for CharacterID: %s"), *CharacterID);
		OutChatbot = nullptr;
		return nullptr;
	}

	OutChatbot = NewObject<UConvaiChatbotComponent>(
		Actor, UConvaiChatbotComponent::StaticClass(), NAME_None, RF_Transient);
	OutChatbot->CharacterID = CharacterID;
	OutChatbot->bAutoInitializeSession = false;
	OutChatbot->RegisterComponent();
	Actor->AddInstanceComponent(OutChatbot);
	OutChatbot->RegisterComponentWithWorld(GetWorld());
	OutChatbot->StartSession();

	UE_LOG(ConvaiConnectionTestLog, Log,
		TEXT("[%d/%d] Spawned chatbot for CharacterID: %s"),
		CurrentIteration, TotalIterations, *CharacterID);

	return Actor;
}

void AConvaiConnectionTest::DestroyPrimaryChatbot()
{
	if (PrimaryChatbot)
	{
		PrimaryChatbot->StopSession();
		PrimaryChatbot = nullptr;
	}
	if (PrimaryActor)
	{
		PrimaryActor->Destroy();
		PrimaryActor = nullptr;
	}
}

void AConvaiConnectionTest::DestroySecondaryChatbot()
{
	if (SecondaryChatbot)
	{
		SecondaryChatbot->StopSession();
		SecondaryChatbot = nullptr;
	}
	if (SecondaryActor)
	{
		SecondaryActor->Destroy();
		SecondaryActor = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Timer Tick — drives the state machine
// ---------------------------------------------------------------------------

void AConvaiConnectionTest::OnTimerTick()
{
	PhaseElapsed += TickInterval;

	switch (Phase)
	{
	case EConvaiTestPhase::WaitingForConnection:
	{
		if (!PrimaryChatbot)
		{
			FinishScenario(false, TEXT("ChatbotComponent was destroyed unexpectedly"));
			return;
		}

		const EC_ConnectionState State = PrimaryChatbot->GetChatbotConnectionState();

		if (State == EC_ConnectionState::Connected)
		{
			UE_LOG(ConvaiConnectionTestLog, Log,
				TEXT("[%d/%d] Connected (%.1fs)"), CurrentIteration, TotalIterations, PhaseElapsed);
			OnConnectionEstablished();
		}
		else if (PhaseElapsed >= ConnectionTimeout)
		{
			FinishScenario(false,
				FString::Printf(TEXT("Timed out waiting for connection (%.0fs)"), ConnectionTimeout));
		}
		break;
	}

	case EConvaiTestPhase::WaitingToRespawn:
	{
		if (PhaseElapsed >= RespawnDelay)
		{
			CurrentIteration++;

			FString CharID = PrimaryCharacterID;
			if (ActiveScenario == EConvaiTestScenario::DifferentCharacterID && bFirstConnectionDone)
			{
				CharID = SecondaryCharacterID;
			}

			UE_LOG(ConvaiConnectionTestLog, Log,
				TEXT("[%d/%d] Respawning with CharacterID: %s"),
				CurrentIteration, TotalIterations, *CharID);

			PrimaryActor = SpawnChatbotActor(CharID, PrimaryChatbot);
			if (!PrimaryActor)
			{
				FinishScenario(false, TEXT("Failed to spawn actor on respawn"));
				return;
			}

			Phase = EConvaiTestPhase::WaitingForConnection;
			PhaseElapsed = 0.0f;
		}
		break;
	}

	case EConvaiTestPhase::WaitingForExpiry:
	{
		const UConvaiConnectionManager* Mgr = GetConnectionManager();
		const float ExpiryWait = Mgr ? (UConvaiUtils::GetConnectionProxyTTL() + 2.0f) : 12.0f;

		if (PhaseElapsed >= ExpiryWait)
		{
			UE_LOG(ConvaiConnectionTestLog, Log,
				TEXT("Expiry wait done (%.1fs). Manager state: %d. Respawning..."),
				PhaseElapsed, Mgr ? static_cast<int32>(Mgr->GetManagedState()) : -1);

			CurrentIteration++;
			PrimaryActor = SpawnChatbotActor(PrimaryCharacterID, PrimaryChatbot);
			if (!PrimaryActor)
			{
				FinishScenario(false, TEXT("Failed to spawn actor after expiry wait"));
				return;
			}

			Phase = EConvaiTestPhase::WaitingForConnection;
			PhaseElapsed = 0.0f;
		}
		else if (FMath::Fmod(PhaseElapsed, 3.0f) < TickInterval)
		{
			UE_LOG(ConvaiConnectionTestLog, Log,
				TEXT("  Waiting for expiry... %.1f / %.1fs"), PhaseElapsed, ExpiryWait);
		}
		break;
	}

	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Connection established — scenario-specific verification
// ---------------------------------------------------------------------------

void AConvaiConnectionTest::OnConnectionEstablished()
{
	const UConvaiConnectionManager* Mgr = GetConnectionManager();
	const UConvaiConnectionSessionProxy* CurrentProxy = Mgr ? Mgr->GetManagedProxy() : nullptr;

	// ---- DoubleAcquire: try a second acquire while the first is still active ----
	if (ActiveScenario == EConvaiTestScenario::DoubleAcquire)
	{
		UE_LOG(ConvaiConnectionTestLog, Log,
			TEXT("[DoubleAcquire] First chatbot connected. Spawning second with same CharacterID..."));

		SecondaryActor = SpawnChatbotActor(PrimaryCharacterID, SecondaryChatbot);

		const EC_ConnectionState FirstState = PrimaryChatbot->GetChatbotConnectionState();
		const EC_ConnectionState SecondState = SecondaryChatbot
			? SecondaryChatbot->GetChatbotConnectionState()
			: EC_ConnectionState::Disconnected;

		UE_LOG(ConvaiConnectionTestLog, Log,
			TEXT("[DoubleAcquire] First=%d  Second=%d  (expect First=Connected, Second=Disconnected)"),
			static_cast<int32>(FirstState), static_cast<int32>(SecondState));

		DestroySecondaryChatbot();
		DestroyPrimaryChatbot();

		if (FirstState == EC_ConnectionState::Connected
			&& SecondState == EC_ConnectionState::Disconnected)
		{
			FinishScenario(true, TEXT("Second acquire correctly rejected — first remained connected"));
		}
		else
		{
			FinishScenario(false, FString::Printf(
				TEXT("Expected First=Connected(2) Second=Disconnected(0), got First=%d Second=%d"),
				static_cast<int32>(FirstState), static_cast<int32>(SecondState)));
		}
		return;
	}

	// ---- First connection: establish baseline ----
	if (!bFirstConnectionDone)
	{
		TrackedProxy = CurrentProxy;
		bFirstConnectionDone = true;

		UE_LOG(ConvaiConnectionTestLog, Log,
			TEXT("[%d/%d] Baseline proxy recorded (%p). Releasing connection..."),
			CurrentIteration, TotalIterations, TrackedProxy);

		DestroyPrimaryChatbot();

		if (ActiveScenario == EConvaiTestScenario::ExpiryTimeout)
		{
			Phase = EConvaiTestPhase::WaitingForExpiry;
		}
		else
		{
			Phase = EConvaiTestPhase::WaitingToRespawn;
		}
		PhaseElapsed = 0.0f;
		return;
	}

	// ---- Subsequent connections: verify proxy reuse / new allocation ----
	const bool bExpectSameProxy =
		ActiveScenario == EConvaiTestScenario::BasicReuse ||
		ActiveScenario == EConvaiTestScenario::RapidCycle;

	const bool bIsSameProxy = (CurrentProxy == TrackedProxy && CurrentProxy != nullptr);

	UE_LOG(ConvaiConnectionTestLog, Log,
		TEXT("[%d/%d] Proxy=%p  prev=%p  same=%s  expected_same=%s"),
		CurrentIteration, TotalIterations, CurrentProxy, TrackedProxy,
		bIsSameProxy ? TEXT("YES") : TEXT("NO"),
		bExpectSameProxy ? TEXT("YES") : TEXT("NO"));

	if (bExpectSameProxy && !bIsSameProxy)
	{
		DestroyPrimaryChatbot();
		FinishScenario(false, FString::Printf(
			TEXT("Expected REUSED proxy but got new. Old=%p New=%p"), TrackedProxy, CurrentProxy));
		return;
	}

	if (!bExpectSameProxy && bIsSameProxy)
	{
		DestroyPrimaryChatbot();
		FinishScenario(false, FString::Printf(
			TEXT("Expected NEW proxy but got reused. Proxy=%p"), CurrentProxy));
		return;
	}

	TrackedProxy = CurrentProxy;
	DestroyPrimaryChatbot();

	if (CurrentIteration >= TotalIterations)
	{
		FinishScenario(true, TEXT("All iterations passed"));
	}
	else
	{
		Phase = EConvaiTestPhase::WaitingToRespawn;
		PhaseElapsed = 0.0f;
	}
}

// ---------------------------------------------------------------------------
// Finish / Queue
// ---------------------------------------------------------------------------

void AConvaiConnectionTest::FinishScenario(bool bSuccess, const FString& Reason)
{
	GetWorld()->GetTimerManager().ClearTimer(TickTimerHandle);
	DestroySecondaryChatbot();
	DestroyPrimaryChatbot();
	Phase = EConvaiTestPhase::Idle;

	const FString ScenarioName = GetScenarioName(ActiveScenario);

	if (bSuccess)
	{
		PassCount++;
		UE_LOG(ConvaiConnectionTestLog, Log,
			TEXT("========== [PASS] %s: %s =========="), *ScenarioName, *Reason);
	}
	else
	{
		FailCount++;
		UE_LOG(ConvaiConnectionTestLog, Error,
			TEXT("========== [FAIL] %s: %s =========="), *ScenarioName, *Reason);
	}

	if (ScenarioQueue.Num() > 0)
	{
		AdvanceScenarioQueue();
	}
	else if (bRunningAll)
	{
		bRunningAll = false;
		UE_LOG(ConvaiConnectionTestLog, Log,
			TEXT("========== ALL SCENARIOS COMPLETE: %d passed, %d failed =========="),
			PassCount, FailCount);
	}
}

void AConvaiConnectionTest::AdvanceScenarioQueue()
{
	if (ScenarioQueue.Num() == 0)
	{
		return;
	}

	const EConvaiTestScenario Next = ScenarioQueue[0];
	ScenarioQueue.RemoveAt(0);

	RunScenario(Next, PrimaryCharacterID, SecondaryCharacterID, 3, QueuedDelay);
}
