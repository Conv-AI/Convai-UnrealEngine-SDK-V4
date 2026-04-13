// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTest_Text.h"



#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiDefinitions.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

namespace ConvaiTextTestConstants { constexpr float PollInterval = 0.25f; }

bool UConvaiTest_Text::Setup()
{
	if (!Super::Setup()) return false;

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

	Chatbot->OnTranscriptionReceivedDelegate.AddDynamic(this, &UConvaiTest_Text::OnTranscription);
	Chatbot->OnInteractionIDReceivedEvent.AddDynamic(this, &UConvaiTest_Text::OnInteractionID);
	Chatbot->OnFailureEvent.AddDynamic(this, &UConvaiTest_Text::OnChatbotFailure);

	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("Spawned chatbot+player for '%s'"), *Context.CharacterID),
		TEXT("setup"));
	return true;
}

void UConvaiTest_Text::Execute()
{
	Phase = EPhase::WaitingForConnect;
	Chatbot->StartSession();
	StartPollTimer();
}

void UConvaiTest_Text::Teardown()
{
	StopPollTimer();

	if (Chatbot)
	{
		Chatbot->OnTranscriptionReceivedDelegate.RemoveDynamic(this, &UConvaiTest_Text::OnTranscription);
		Chatbot->OnInteractionIDReceivedEvent.RemoveDynamic(this, &UConvaiTest_Text::OnInteractionID);
		Chatbot->OnFailureEvent.RemoveDynamic(this, &UConvaiTest_Text::OnChatbotFailure);
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
}

void UConvaiTest_Text::StartPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(
			PollHandle,
			FTimerDelegate::CreateUObject(this, &UConvaiTest_Text::PollConnectionReady),
			ConvaiTextTestConstants::PollInterval, true);
	}
}

void UConvaiTest_Text::StopPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(PollHandle);
	}
}

void UConvaiTest_Text::PollConnectionReady()
{
	if (!Chatbot)
	{
		FinishWithFailure(EConvaiTestFailureReason::UnexpectedState, TEXT("Chatbot went null"));
		return;
	}

	if (Phase != EPhase::WaitingForConnect) return;

	if (Chatbot->GetChatbotConnectionState() == EC_ConnectionState::Connected)
	{
		RecordPhase(TEXT("handshake"), 0.0, ElapsedMs());
		LogEvent(ELogVerbosity::Log,
			FString::Printf(TEXT("Connected — sending text: '%s'"), *Context.SampleText),
			TEXT("text.send"));

		Phase = EPhase::WaitingForResponse;
		SendAtMs = ElapsedMs();
		Player->SendText(Chatbot, Context.SampleText);
		// Poll timer keeps ticking; we may eventually detect a silent failure via the overall timeout.
	}
}

void UConvaiTest_Text::OnTranscription(
	UConvaiConversationComponent* Speaker,
	UConvaiConversationComponent* /*Listener*/,
	FString Transcription,
	bool /*IsTranscriptionReady*/,
	bool IsFinal)
{
	// We only care about the character's own output (Speaker == Chatbot).
	if (Speaker != Chatbot) return;

	AccumulatedResponse = Transcription;

	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("transcription final=%d len=%d '%s'"),
			IsFinal ? 1 : 0, Transcription.Len(), *Transcription.Left(120)),
		TEXT("text.recv"));

	if (IsFinal)
	{
		if (Transcription.TrimStartAndEnd().IsEmpty())
		{
			FinishWithFailure(EConvaiTestFailureReason::BadPayload,
				TEXT("Final transcription from chatbot was empty"));
			return;
		}

		RecordPhase(TEXT("text_response"), SendAtMs, ElapsedMs() - SendAtMs);
		FinishWithPass(FString::Printf(TEXT("Got final response of %d chars"), Transcription.Len()));
	}
}

void UConvaiTest_Text::OnInteractionID(
	UConvaiChatbotComponent* /*InChatbot*/,
	UConvaiPlayerComponent* /*InPlayer*/,
	FString InteractionID)
{
	LogEvent(ELogVerbosity::Log,
		FString::Printf(TEXT("interaction_id=%s"), *InteractionID),
		TEXT("text.meta"));
}

void UConvaiTest_Text::OnChatbotFailure()
{
	LogEvent(ELogVerbosity::Error, TEXT("OnFailureEvent raised"), TEXT("failure"));
	FinishWithFailure(EConvaiTestFailureReason::FailureEventRaised,
		TEXT("Chatbot raised OnFailureEvent during text test"));
}

