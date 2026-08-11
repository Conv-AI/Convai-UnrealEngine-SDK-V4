// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Tests/ConvaiTest_ActionResponse.h"

#include "ConvaiChatbotComponent.h"
#include "ConvaiConversationComponent.h"
#include "ConvaiDefinitions.h"
#include "ConvaiPlayerComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

namespace ConvaiActionResponseTest
{
	constexpr float PollIntervalSeconds = 0.25f;
	constexpr double TransportReadyGraceMs = 2000.0;
	constexpr double ActionResponseTimeoutMs = 45000.0;
	constexpr float RequiredOverallTimeoutSeconds = 60.0f;

	const TCHAR* EnglishPrompt = TEXT("execute option A.");

	const FConvaiResultAction* FindAction(
		const TArray<FConvaiResultAction>& Actions,
		const TCHAR* ExpectedName)
	{
		return Actions.FindByPredicate([ExpectedName](const FConvaiResultAction& Action)
		{
			return Action.Action.Equals(ExpectedName, ESearchCase::IgnoreCase);
		});
	}

	bool ReadStringParameter(
		const FConvaiResultAction& Action,
		const TCHAR* ParameterName,
		FString& OutValue)
	{
		const FConvaiResultParam* Parameter = Action.Parameters.Find(ParameterName);
		if (!Parameter)
		{
			for (const TPair<FString, FConvaiResultParam>& Pair : Action.Parameters)
			{
				if (Pair.Key.Equals(ParameterName, ESearchCase::IgnoreCase))
				{
					Parameter = &Pair.Value;
					break;
				}
			}
		}

		if (!Parameter)
		{
			return false;
		}

		OutValue = Parameter->StringValue.TrimStartAndEnd();
		return true;
	}
}

bool UConvaiTest_ActionResponse::Setup()
{
	if (!Super::Setup())
	{
		return false;
	}

	// The shared harness defaults to 30 seconds, but this live regression needs
	// enough time to distinguish a missing action callback from a slow model.
	Context.TimeoutSeconds = FMath::Max(
		Context.TimeoutSeconds,
		ConvaiActionResponseTest::RequiredOverallTimeoutSeconds);

	UWorld* World = GetTestWorld();
	if (!World)
	{
		FinishWithFailure(EConvaiTestFailureReason::SetupError, TEXT("No world"));
		return false;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	OwnerActor = World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);

	Chatbot = NewObject<UConvaiChatbotComponent>(
		OwnerActor, UConvaiChatbotComponent::StaticClass(), NAME_None, RF_Transient);
	Chatbot->CharacterID = Context.CharacterID;
	Chatbot->bAutoInitializeSession = false;
	Chatbot->EnvironmentData.bEnableActions = true;
	Chatbot->EnvironmentData.Actions.Reset();

	Prompt = Context.SampleText.TrimStartAndEnd();
	if (Prompt.IsEmpty() ||
		Prompt.Equals(TEXT("Hello, can you hear me?"), ESearchCase::IgnoreCase))
	{
		Prompt = ConvaiActionResponseTest::EnglishPrompt;
	}
	bFollowScenario = Prompt.Contains(TEXT("follow"), ESearchCase::IgnoreCase);
	bParameterizedControlScenario =
		Prompt.Contains(TEXT("sensitive skin"), ESearchCase::IgnoreCase);
	bSpanishRoutineScenario =
		Prompt.Contains(TEXT("piel sensible"), ESearchCase::IgnoreCase);
	bProductScenario =
		Prompt.Contains(TEXT("producto"), ESearchCase::IgnoreCase) ||
		Prompt.Contains(TEXT("product"), ESearchCase::IgnoreCase);

	if (bFollowScenario)
	{
		Chatbot->EnvironmentData.Actions.Add(FConvaiAction(
			TEXT("Follow"),
			TEXT("Execute this action whenever the user asks you to follow them."),
			{}));
	}
	else if (bProductScenario)
	{
		FConvaiActionParam Product(
			TEXT("Producto"),
			TEXT("El nombre exacto del producto que se debe recomendar."),
			EConvaiActionParamType::String);
		Chatbot->EnvironmentData.Actions.Add(FConvaiAction(
			TEXT("Recomendar producto"),
			TEXT("Llama a esta accion siempre que el usuario pida una recomendacion de producto o cuando recomiendes un producto concreto. La llamada es obligatoria aunque tambien respondas verbalmente. Incluye siempre el nombre exacto. Ejemplo: para recomendar Sensibio, ejecuta Recomendar producto Sensibio."),
			{ Product }));
	}
	else if (bParameterizedControlScenario)
	{
		FConvaiActionParam Routine(
			TEXT("routine"),
			TEXT("The exact available routine the user asks about or wants to see."),
			EConvaiActionParamType::String);
		Routine.Choices = { TEXT("Sensitive Skin"), TEXT("Mixed Skin") };
		Chatbot->EnvironmentData.Actions.Add(FConvaiAction(
			TEXT("Show Routine"),
			TEXT("Call this action whenever the user asks about, requests, or wants to see one of the available routines. Calling it is required even if you also answer verbally. Always set routine to the exact requested choice."),
			{ Routine }));
	}
	else if (bSpanishRoutineScenario)
	{
		FConvaiActionParam SpanishRoutine(
			TEXT("Rutina"),
			TEXT("La rutina exacta que el usuario solicita o quiere ver."),
			EConvaiActionParamType::String);
		SpanishRoutine.Choices = {
			TEXT("Piel Sensible"),
			TEXT("Piel Mixta"),
			TEXT("Piel Deshidratada"),
			TEXT("Piel Hiperpigmentada"),
			TEXT("Piel Normal a Seca")
		};
		Chatbot->EnvironmentData.Actions.Add(FConvaiAction(
			TEXT("MostrarRutina"),
			TEXT("Llama a esta accion siempre que el usuario pregunte por, solicite o quiera ver una de las rutinas disponibles. La llamada es obligatoria aunque tambien respondas verbalmente. Asigna Rutina a la opcion exacta solicitada."),
			{ SpanishRoutine }));
	}
	else
	{
		FConvaiActionParam EnglishOption(
			TEXT("option"),
			TEXT("Required parameter. Return exactly A or B. Never omit this value."),
			EConvaiActionParamType::String);
		Chatbot->EnvironmentData.Actions.Add(FConvaiAction(
			TEXT("TestAction"),
			TEXT("Call this action whenever the user asks to execute option A or option B. Calling it is required even if you also answer verbally. Always set option to the exact requested value."),
			{ EnglishOption }));
	}

	Chatbot->RegisterComponent();
	OwnerActor->AddInstanceComponent(Chatbot);
	Chatbot->RegisterComponentWithWorld(World);

	Player = NewObject<UConvaiPlayerComponent>(
		OwnerActor, UConvaiPlayerComponent::StaticClass(), NAME_None, RF_Transient);
	Player->bAutoInitializeSession = false;
	Player->RegisterComponent();
	OwnerActor->AddInstanceComponent(Player);
	Player->RegisterComponentWithWorld(World);

	Chatbot->OnActionReceivedEvent_V2.AddDynamic(
		this, &UConvaiTest_ActionResponse::OnActionsReceived);
	Chatbot->OnTranscriptionReceivedDelegate.AddDynamic(
		this, &UConvaiTest_ActionResponse::OnTranscription);
	Chatbot->OnFailureEvent.AddDynamic(
		this, &UConvaiTest_ActionResponse::OnChatbotFailure);

	LogEvent(
		ELogVerbosity::Log,
		FString::Printf(
			TEXT("Spawned reporter %s scenario for '%s' with %d actions; prompt='%s'"),
			bFollowScenario ? TEXT("Follow control") :
				bParameterizedControlScenario ? TEXT("Parameterized control") :
				bSpanishRoutineScenario ? TEXT("Spanish routine") :
				bProductScenario ? TEXT("Product") : TEXT("English"),
			*Context.CharacterID,
			Chatbot->EnvironmentData.Actions.Num(),
			*Prompt),
		TEXT("setup"));
	return true;
}

void UConvaiTest_ActionResponse::Execute()
{
	Phase = EPhase::WaitingForConnect;
	Chatbot->StartSession();
	if (!Player->StartSession())
	{
		FinishWithFailure(
			EConvaiTestFailureReason::SetupError,
			TEXT("Player session could not be started"));
		return;
	}

	// This regression is text-only. Disable the player's microphone stream and
	// tick work while retaining its session proxy.
	Player->MuteStreamingAudio();
	Player->SetComponentTickEnabled(false);
	StartPollTimer();
}

void UConvaiTest_ActionResponse::Teardown()
{
	StopPollTimer();

	if (Chatbot)
	{
		Chatbot->OnActionReceivedEvent_V2.RemoveDynamic(
			this, &UConvaiTest_ActionResponse::OnActionsReceived);
		Chatbot->OnTranscriptionReceivedDelegate.RemoveDynamic(
			this, &UConvaiTest_ActionResponse::OnTranscription);
		Chatbot->OnFailureEvent.RemoveDynamic(
			this, &UConvaiTest_ActionResponse::OnChatbotFailure);
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

void UConvaiTest_ActionResponse::StartPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().SetTimer(
			PollHandle,
			FTimerDelegate::CreateUObject(
				this, &UConvaiTest_ActionResponse::PollConnectionReady),
			ConvaiActionResponseTest::PollIntervalSeconds,
			true);
	}
}

void UConvaiTest_ActionResponse::StopPollTimer()
{
	if (UWorld* World = GetTestWorld())
	{
		World->GetTimerManager().ClearTimer(PollHandle);
	}
}

void UConvaiTest_ActionResponse::PollConnectionReady()
{
	if (!Chatbot)
	{
		FinishWithFailure(
			EConvaiTestFailureReason::UnexpectedState,
			TEXT("Chatbot went null"));
		return;
	}

	if (Phase == EPhase::WaitingForAction)
	{
		if (ElapsedMs() - SendAtMs >= ConvaiActionResponseTest::ActionResponseTimeoutMs)
		{
			FinishWithFailure(
				EConvaiTestFailureReason::CallbackMissing,
				FString::Printf(
					TEXT("Server acknowledged '%s' but no matching action-response arrived within %.0f seconds"),
					*Prompt,
					ConvaiActionResponseTest::ActionResponseTimeoutMs / 1000.0));
		}
		return;
	}

	if (Phase != EPhase::WaitingForConnect)
	{
		return;
	}

	if (Chatbot->GetChatbotConnectionState() == EC_ConnectionState::Connected)
	{
		if (ChatbotConnectedAtMs < 0.0)
		{
			ChatbotConnectedAtMs = ElapsedMs();
			LogEvent(
				ELogVerbosity::Log,
				TEXT("Chatbot handshake complete; waiting briefly for the realtime data channel"),
				TEXT("action.connect"));
			return;
		}
		if (ElapsedMs() - ChatbotConnectedAtMs <
			ConvaiActionResponseTest::TransportReadyGraceMs)
		{
			return;
		}

		RecordPhase(TEXT("handshake"), 0.0, ElapsedMs());
		Phase = EPhase::WaitingForAction;
		SendAtMs = ElapsedMs();
		LogEvent(
			ELogVerbosity::Log,
			FString::Printf(
				TEXT("Realtime transport ready; sending %s prompt: '%s'"),
				bFollowScenario ? TEXT("Follow control") :
					bParameterizedControlScenario ? TEXT("Parameterized control") :
					bSpanishRoutineScenario ? TEXT("Spanish routine") :
					bProductScenario ? TEXT("Product") : TEXT("English"),
				*Prompt),
			TEXT("action.send"));
		Player->SendText(Chatbot, Prompt);
	}
}

void UConvaiTest_ActionResponse::OnActionsReceived(
	UConvaiChatbotComponent* InChatbot,
	UConvaiPlayerComponent* /*InPlayer*/,
	const TArray<FConvaiResultAction>& Actions)
{
	if (InChatbot != Chatbot)
	{
		return;
	}

	for (const FConvaiResultAction& Action : Actions)
	{
		LogEvent(
			ELogVerbosity::Log,
			FString::Printf(
				TEXT("action='%s' raw='%s' params=%d"),
				*Action.Action,
				*Action.ActionString,
				Action.Parameters.Num()),
			TEXT("action.recv"));
	}

	if (Phase != EPhase::WaitingForAction)
	{
		return;
	}

	if (bFollowScenario)
	{
		const FConvaiResultAction* Action =
			ConvaiActionResponseTest::FindAction(Actions, TEXT("Follow"));
		if (!Action)
		{
			if (!Actions.IsEmpty())
			{
				FinishWithFailure(
					EConvaiTestFailureReason::BadPayload,
					FString::Printf(
						TEXT("Expected Follow, received '%s'"),
						*Actions[0].Action));
			}
			return;
		}

		LogEvent(
			ELogVerbosity::Log,
			TEXT("Validated Follow action response"),
			TEXT("assert"));
		FinishWithPass(TEXT("Follow action response arrived"));
		return;
	}

	if (bParameterizedControlScenario)
	{
		const FConvaiResultAction* Action =
			ConvaiActionResponseTest::FindAction(Actions, TEXT("Show Routine"));
		if (!Action)
		{
			if (!Actions.IsEmpty())
			{
				FinishWithFailure(
					EConvaiTestFailureReason::BadPayload,
					FString::Printf(
						TEXT("Expected Show Routine, received '%s'"),
						*Actions[0].Action));
			}
			return;
		}

		FString Value;
		if (!ConvaiActionResponseTest::ReadStringParameter(
			*Action, TEXT("routine"), Value))
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				TEXT("Show Routine arrived without the declared 'routine' parameter"));
			return;
		}
		if (!Value.Equals(TEXT("Sensitive Skin"), ESearchCase::IgnoreCase))
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				FString::Printf(
					TEXT("Show Routine value was '%s', expected 'Sensitive Skin'"),
					*Value));
			return;
		}

		LogEvent(
			ELogVerbosity::Log,
			TEXT("Validated Show Routine routine=Sensitive Skin"),
			TEXT("assert"));
		FinishWithPass(
			TEXT("Parameterized action response arrived with routine=Sensitive Skin"));
		return;
	}

	if (bProductScenario)
	{
		const FConvaiResultAction* Action =
			ConvaiActionResponseTest::FindAction(Actions, TEXT("Recomendar producto"));
		if (!Action)
		{
			if (!Actions.IsEmpty())
			{
				FinishWithFailure(
					EConvaiTestFailureReason::BadPayload,
					FString::Printf(
						TEXT("Expected Recomendar producto, received '%s'"),
						*Actions[0].Action));
			}
			return;
		}

		FString Value;
		if (!ConvaiActionResponseTest::ReadStringParameter(
			*Action, TEXT("Producto"), Value))
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				TEXT("Product action arrived without the declared 'Producto' parameter"));
			return;
		}
		if (!Value.Contains(TEXT("Sensibio"), ESearchCase::IgnoreCase))
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				FString::Printf(
					TEXT("Product action Producto was '%s', expected Sensibio"),
					*Value));
			return;
		}

		LogEvent(
			ELogVerbosity::Log,
			TEXT("Validated Recomendar producto Producto=Sensibio"),
			TEXT("assert"));
		FinishWithPass(TEXT("Product action arrived with Producto=Sensibio"));
		return;
	}

	if (!bSpanishRoutineScenario)
	{
		const FConvaiResultAction* Action =
			ConvaiActionResponseTest::FindAction(Actions, TEXT("TestAction"));
		if (!Action)
		{
			if (!Actions.IsEmpty())
			{
				FinishWithFailure(
					EConvaiTestFailureReason::BadPayload,
					FString::Printf(
						TEXT("Expected TestAction, received '%s'"),
						*Actions[0].Action));
			}
			return;
		}

		FString Value;
		if (!ConvaiActionResponseTest::ReadStringParameter(*Action, TEXT("option"), Value))
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				TEXT("TestAction arrived without the declared 'option' parameter"));
			return;
		}
		if (!Value.Equals(TEXT("A"), ESearchCase::IgnoreCase))
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				FString::Printf(
					TEXT("TestAction option was '%s', expected 'A'"),
					*Value));
			return;
		}

		LogEvent(
			ELogVerbosity::Log,
			TEXT("Validated TestAction option=A"),
			TEXT("assert"));
		FinishWithPass(TEXT("English TestAction arrived with option=A"));
		return;
	}

	const FConvaiResultAction* Action =
		ConvaiActionResponseTest::FindAction(Actions, TEXT("MostrarRutina"));
	if (!Action)
	{
		if (!Actions.IsEmpty())
		{
			FinishWithFailure(
				EConvaiTestFailureReason::BadPayload,
				FString::Printf(
					TEXT("Expected MostrarRutina, received '%s'"),
					*Actions[0].Action));
		}
		return;
	}

	FString Value;
	if (!ConvaiActionResponseTest::ReadStringParameter(*Action, TEXT("Rutina"), Value))
	{
		FinishWithFailure(
			EConvaiTestFailureReason::BadPayload,
			TEXT("MostrarRutina arrived without the declared 'Rutina' parameter"));
		return;
	}
	if (!Value.Equals(TEXT("Piel Sensible"), ESearchCase::IgnoreCase))
	{
		FinishWithFailure(
			EConvaiTestFailureReason::BadPayload,
			FString::Printf(
				TEXT("MostrarRutina Rutina was '%s', expected 'Piel Sensible'"),
				*Value));
		return;
	}

	LogEvent(
		ELogVerbosity::Log,
		TEXT("Validated MostrarRutina Rutina=Piel Sensible"),
		TEXT("assert"));
	FinishWithPass(TEXT("Spanish MostrarRutina arrived with Rutina=Piel Sensible"));
}

void UConvaiTest_ActionResponse::OnTranscription(
	UConvaiConversationComponent* Speaker,
	UConvaiConversationComponent* /*Listener*/,
	FString Transcription,
	bool /*IsTranscriptionReady*/,
	bool IsFinal)
{
	if (Speaker != Chatbot || !IsFinal)
	{
		return;
	}

	LogEvent(
		ELogVerbosity::Log,
		FString::Printf(
			TEXT("final transcription len=%d '%s'"),
			Transcription.Len(),
			*Transcription.Left(120)),
		TEXT("text.recv"));
}

void UConvaiTest_ActionResponse::OnChatbotFailure()
{
	LogEvent(
		ELogVerbosity::Error,
		TEXT("OnFailureEvent raised"),
		TEXT("failure"));
	FinishWithFailure(
		EConvaiTestFailureReason::FailureEventRaised,
		TEXT("Chatbot raised OnFailureEvent during action-response test"));
}
