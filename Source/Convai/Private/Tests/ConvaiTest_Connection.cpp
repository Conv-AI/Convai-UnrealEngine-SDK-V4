// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTest_Connection.h"



#include "ConvaiChatbotComponent.h"
#include "ConvaiDefinitions.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

namespace ConvaiConnectionTestConstants
{
	constexpr float PollInterval = 0.25f;

	const TCHAR* StateToString(EC_ConnectionState S)
	{
		switch (S)
		{
		case EC_ConnectionState::Disconnected: return TEXT("Disconnected");
		case EC_ConnectionState::Connecting:   return TEXT("Connecting");
		case EC_ConnectionState::Connected:    return TEXT("Connected");
		case EC_ConnectionState::Reconnecting: return TEXT("Reconnecting");
		default:                               return TEXT("Unknown");
		}
	}
}

bool UConvaiTest_Connection::Setup()
{
	if (!Super::Setup()) return false;

	UWorld* World = GetTestWorld();
	if (!World)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("No world available"));
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	OwnerActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!OwnerActor)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("Failed to spawn owner actor"));
		return false;
	}

	Chatbot = NewObject<UConvaiChatbotComponent>(OwnerActor, UConvaiChatbotComponent::StaticClass(), NAME_None, RF_Transient);
	Chatbot->CharacterID = Context.CharacterID;
	Chatbot->bAutoInitializeSession = false;
	Chatbot->RegisterComponent();
	OwnerActor->AddInstanceComponent(Chatbot);
	Chatbot->RegisterComponentWithWorld(World);

	Chatbot->OnFailureEvent.AddDynamic(this, &UConvaiTest_Connection::OnChatbotFailure);

	LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("Spawned chatbot for '%s'"), *Context.CharacterID), TEXT("setup"));
	return true;
}

void UConvaiTest_Connection::Execute()
{
	HandlePhaseTransition(EPhase::WaitingForFirstConnect);
	Chatbot->StartSession();
	StartPollTimer();
}

void UConvaiTest_Connection::Teardown()
{
	StopPollTimer();
	if (Chatbot)
	{
		Chatbot->OnFailureEvent.RemoveDynamic(this, &UConvaiTest_Connection::OnChatbotFailure);
		Chatbot->StopSession();
		Chatbot = nullptr;
	}
	if (OwnerActor)
	{
		OwnerActor->Destroy();
		OwnerActor = nullptr;
	}
}

void UConvaiTest_Connection::StartPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(PollHandle, FTimerDelegate::CreateUObject(this, &UConvaiTest_Connection::PollConnection), ConvaiConnectionTestConstants::PollInterval, true);
	}
}

void UConvaiTest_Connection::StopPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(PollHandle);
	}
}

void UConvaiTest_Connection::HandlePhaseTransition(EPhase NewPhase)
{
	Phase = NewPhase;
	PhaseStartMs = ElapsedMs();

	const TCHAR* Name =
		NewPhase == EPhase::WaitingForFirstConnect  ? TEXT("first_connect")  :
		NewPhase == EPhase::WaitingForDisconnect    ? TEXT("disconnect")     :
		NewPhase == EPhase::WaitingForSecondConnect ? TEXT("second_connect") : TEXT("idle");

	LogEvent(ELogVerbosity::Log, FString::Printf(TEXT("phase=%s"), Name), TEXT("phase"));
}

void UConvaiTest_Connection::PollConnection()
{
	if (!Chatbot)
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Chatbot became null mid-run"));
		return;
	}

	const EC_ConnectionState State = Chatbot->GetChatbotConnectionState();
	LogEvent(ELogVerbosity::Verbose, FString::Printf(TEXT("state=%s"), ConvaiConnectionTestConstants::StateToString(State)), TEXT("poll"));

	switch (Phase)
	{
	case EPhase::WaitingForFirstConnect:
		if (State == EC_ConnectionState::Connected)
		{
			RecordPhase(TEXT("first_connect"), PhaseStartMs, ElapsedMs() - PhaseStartMs);
			LogEvent(ELogVerbosity::Log, TEXT("First connect OK — issuing StopSession for reconnect test"), TEXT("phase"));
			Chatbot->StopSession();
			HandlePhaseTransition(EPhase::WaitingForDisconnect);
		}
		break;

	case EPhase::WaitingForDisconnect:
		if (State == EC_ConnectionState::Disconnected)
		{
			RecordPhase(TEXT("disconnect"), PhaseStartMs, ElapsedMs() - PhaseStartMs);
			LogEvent(ELogVerbosity::Log, TEXT("Disconnected — starting second session"), TEXT("phase"));
			Chatbot->StartSession();
			HandlePhaseTransition(EPhase::WaitingForSecondConnect);
		}
		break;

	case EPhase::WaitingForSecondConnect:
		if (State == EC_ConnectionState::Connected)
		{
			RecordPhase(TEXT("second_connect"), PhaseStartMs, ElapsedMs() - PhaseStartMs);
			FinishWithPass(TEXT("Reconnect cycle completed"));
		}
		break;

	default:
		break;
	}
}

void UConvaiTest_Connection::OnChatbotFailure()
{
	LogEvent(ELogVerbosity::Error, TEXT("OnFailureEvent raised by chatbot"), TEXT("failure"));
	FinishWithFailure(EConvaiTestFailureReason::FailureEventRaised, TEXT("Chatbot OnFailureEvent raised during connection test"));
}

