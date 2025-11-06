// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiChatbotComponent.h"
#include "ConvaiPlayerComponent.h"
#include "../Convai.h"
#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "LipSyncInterface.h"
#include "VisionInterface.h"
#include "ConvaiSubsystem.h"
#include "ConvaiConnectionSessionProxy.h"

#include "Sound/SoundWaveProcedural.h"
#include "Net/UnrealNetwork.h"
#include "ConvaiChatBotProxy.h"
#include "Kismet/KismetSystemLibrary.h"
#include "TimerManager.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"

DEFINE_LOG_CATEGORY(ConvaiChatbotComponentLog);

UConvaiChatbotComponent::UConvaiChatbotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 1 / 15;
	InterruptVoiceFadeOutDuration = 1.0;
	bAutoInitializeSession = true;
}

void UConvaiChatbotComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UConvaiChatbotComponent, CharacterID);
	DOREPLIFETIME(UConvaiChatbotComponent, SessionID);
	DOREPLIFETIME(UConvaiChatbotComponent, CharacterName);
	DOREPLIFETIME(UConvaiChatbotComponent, VoiceType);
	DOREPLIFETIME(UConvaiChatbotComponent, Backstory);
	DOREPLIFETIME(UConvaiChatbotComponent, ReadyPlayerMeLink);
	DOREPLIFETIME(UConvaiChatbotComponent, LanguageCode);
	DOREPLIFETIME(UConvaiChatbotComponent, ActionsQueue);
	DOREPLIFETIME(UConvaiChatbotComponent, EmotionState);
	DOREPLIFETIME(UConvaiChatbotComponent, LockEmotionState);
	DOREPLIFETIME(UConvaiChatbotComponent, ConvaiEnvironmentDetails);
}

bool UConvaiChatbotComponent::IsInConversation()
{
	return (IsProcessing() || IsListening() || GetIsTalking());
}

bool UConvaiChatbotComponent::IsProcessing()
{
	// TODO is processing when the user have finished talking and we're now expecting some answer
	// if the answer takes more than some time (5 seconds) we should consider that it might not longer be processing
	return false;
}

bool UConvaiChatbotComponent::IsListening()
{
	// To Do chracter is listening based on if the session detects that the user is talking
	if (IsValid(SessionProxyInstance))
	{
		//return SessionProxyInstance->IsConnected();
	}
	return false;
}

bool UConvaiChatbotComponent::GetIsTalking()
{
	return IsTalking;
}

float UConvaiChatbotComponent::GetTalkingTimeElapsed()
{
	float TimeElapsed = 0;
	if (IsValid(GetWorld()))
	{
		// TODO: Reset DataBuffer.TotalAudioDurationElapsed after response is complete
		TimeElapsed = GetWorld()->GetTimerManager().GetTimerElapsed(AudioFinishedTimerHandle);
	}

	return TimeElapsed;
}

float UConvaiChatbotComponent::GetTalkingTimeRemaining()
{
	float TimeRemaing = 0;
	if (IsValid(GetWorld()))
	{
		TimeRemaing = GetWorld()->GetTimerManager().GetTimerRemaining(AudioFinishedTimerHandle);
		TimeRemaing = TimeRemaing < 0 ? 0 : TimeRemaing;
		float BufferedInSyncTimeRemaining = GetRemainingContentDuration();
		TimeRemaing += BufferedInSyncTimeRemaining;
	}
	return TimeRemaing;
}

void UConvaiChatbotComponent::UpdateNarrativeTemplateKeys(TMap<FString, FString> InNarrativeTemplateKeys)
{
	NarrativeTemplateKeys = MoveTemp(InNarrativeTemplateKeys);

	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->UpdateTemplateKeys(NarrativeTemplateKeys);
	}	
}

void UConvaiChatbotComponent::UpdateDynamicEnvironmentInfo(FString InDynamicEnvironmentInfo)
{
	DynamicEnvironmentInfo = MoveTemp(InDynamicEnvironmentInfo);
	
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->UpdateDynamicInfo(DynamicEnvironmentInfo);
	}
}

void UConvaiChatbotComponent::ResetConversation()
{
	SessionID = "-1";
}

void UConvaiChatbotComponent::LoadCharacter(FString NewCharacterID)
{
	CharacterID = NewCharacterID;
	ConvaiGetDetails();
}

void UConvaiChatbotComponent::AppendActionsToQueue(TArray<FConvaiResultAction> NewActions)
{
	if (ActionsQueue.Num() > 0)
	{
		FConvaiResultAction FirstAction = ActionsQueue[0];
		NewActions.Insert(FirstAction, 0);
		ActionsQueue = NewActions;
	}
	else
	{
		ActionsQueue = NewActions;
	}
}

void UConvaiChatbotComponent::HandleActionCompletion(bool IsSuccessful, float Delay)
{
	if (!UConvaiUtils::IsNewActionSystemEnabled())
	{

		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("HandleActionCompletion: New Action System is not enabled in settings"));
		return;
	}


	if (IsSuccessful)
		DequeueAction();

	if (IsActionsQueueEmpty())
		return;

	// Create a timer to call StartFirstAction after a delay
	if (Delay > 0.0f)
	{
		FTimerHandle TimerHandle;
		FTimerDelegate TimerDelegate;

		// Bind the function with parameters
		TimerDelegate.BindUFunction(this, FName("StartFirstAction"));

		// Set the timer
		GetWorld()->GetTimerManager().SetTimer(TimerHandle, TimerDelegate, Delay, false);
	}
	else
	{
		// Call the function immediately
		StartFirstAction();
	}
}

bool UConvaiChatbotComponent::IsActionsQueueEmpty()
{
	if (!UConvaiUtils::IsNewActionSystemEnabled())
	{

		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("IsActionsQueueEmpty: New Action System is not enabled in settings"));
		return true;
	}

	return ActionsQueue.Num() == 0;
}

void UConvaiChatbotComponent::ClearActionQueue()
{
	ActionsQueue.Empty();
}

bool UConvaiChatbotComponent::FetchFirstAction(FConvaiResultAction& ConvaiResultAction)
{
	if (!UConvaiUtils::IsNewActionSystemEnabled())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("FetchFirstAction: New Action System is not enabled in settings"));
		return false;
	}

	if (ActionsQueue.Num() == 0)
		return false;

	ConvaiResultAction = ActionsQueue[0];
	return true;
}

bool UConvaiChatbotComponent::DequeueAction()
{
	if (ActionsQueue.Num() > 0)
	{
		ActionsQueue.RemoveAt(0);
		return true;
	}
	return false;
}

bool UConvaiChatbotComponent::StartFirstAction()
{
	FConvaiResultAction ConvaiResultAction;
	if (FetchFirstAction(ConvaiResultAction))
	{
		if (ConvaiResultAction.Action.Compare(FString("None"), ESearchCase::IgnoreCase) == 0)
		{
			HandleActionCompletion(true, 0);
			return true;
		}

		AsyncTask(ENamedThreads::GameThread, [this, ConvaiResultAction]
		{
			TriggerNamedBlueprintAction(ConvaiResultAction.Action, ConvaiResultAction);
		});
		return true;
	}
	return false;
}

bool UConvaiChatbotComponent::TriggerNamedBlueprintAction(const FString& ActionName, FConvaiResultAction ConvaiActionStruct)
{
	if (!ActionName.IsEmpty())
	{
		// Check the owning actor first
		if (AActor* Owner = GetOwner())
		{
			if (TryCallFunction(Owner, ActionName, ConvaiActionStruct))
			{
				return true;
			}
		}

		// Fallback to self (BP_ConvaiChatbotComponent)
		if (TryCallFunction(this, ActionName, ConvaiActionStruct))
		{
			return true;
		}

		// Log an error if the function is not found in both places
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("TriggerNamedBlueprintAction: Could not find a valid function '%s' on the owning actor or the component (self)."), *ActionName);
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("TriggerNamedBlueprintAction: Provided action name is empty."));
	}

	return false;
}

bool UConvaiChatbotComponent::TryCallFunction(UObject* Object, const FString& FunctionName, FConvaiResultAction& ConvaiResultAction) const
{
	if (!Object)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("TryCallFunction: Null object provided."));
		return false;
	}

	UFunction* Function = Object->FindFunction(FName(*FunctionName));
	if (!Function)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Verbose, TEXT("TryCallFunction: Function '%s' not found on '%s'."), *FunctionName, *Object->GetName());
		return false;
	}

	// Check function parameters (if any)
	bool bCanCall = false;
	if (FProperty* FirstParam = Function->PropertyLink)
	{
		if (const FStructProperty* StructProp = CastField<FStructProperty>(FirstParam))
		{
			if (StructProp->Struct == FConvaiResultAction::StaticStruct())
			{
				bCanCall = true;
			}
		}
	}
	else
	{
		bCanCall = true; // No parameters
	}

	if (bCanCall)
	{
		Object->ProcessEvent(Function, Function->PropertyLink ? &ConvaiResultAction : nullptr);
		return true;
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("TryCallFunction: Function '%s' found on '%s' but has incompatible parameters. Ensure it accepts 'FConvaiResultAction' or has no parameters."), *FunctionName, *Object->GetName());
	}

	return false;
}

void UConvaiChatbotComponent::ForceSetEmotion(EBasicEmotions BasicEmotion, EEmotionIntensity Intensity, bool ResetOtherEmotions)
{
	EmotionState.ForceSetEmotion(BasicEmotion, Intensity, ResetOtherEmotions);
	OnEmotionStateChangedEvent.Broadcast(this, nullptr);
}

float UConvaiChatbotComponent::GetEmotionScore(EBasicEmotions Emotion)
{
	return EmotionState.GetEmotionScore(Emotion);
}

TMap<FName, float> UConvaiChatbotComponent::GetEmotionBlendshapes()
{
	return EmotionBlendshapes;
}

void UConvaiChatbotComponent::ResetEmotionState()
{
	EmotionState.ResetEmotionScores();
	OnEmotionStateChangedEvent.Broadcast(this, nullptr);
}

void UConvaiChatbotComponent::StartRecordingVoice()
{
	if (IsRecordingAudio)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Cannot start Recording voice while already recording voice"));
		return;
	}
	IsRecordingAudio = true;
}

USoundWave* UConvaiChatbotComponent::FinishRecordingVoice()
{
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Finished Recording Audio - Total bytes: %d - Duration: %f"), RecordedAudio.Num(), UConvaiUtils::CalculateAudioDuration(RecordedAudio.Num(), 1,RecordedAudioSampleRate, 2));

	if (!IsRecordingAudio)
		return nullptr;
	USoundWave* SoundWave = UConvaiUtils::PCMDataToSoundWav(RecordedAudio, 1, RecordedAudioSampleRate);
	IsRecordingAudio = false;
	RecordedAudio.Empty();
	return SoundWave;
}

bool UConvaiChatbotComponent::PlayRecordedVoice(USoundWave* RecordedVoice)
{
	if (!IsValid(RecordedVoice))
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Recorded voice is not valid"));
		return false;
	}

	if (IsRecordingAudio)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Cannot Play Recorded voice while Recording voice"));
		return false;
	}

	if (GetIsTalking())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Playing Recorded voice and stopping currently playing voice"));
		InterruptSpeech(0);
	}

	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Play Recorded Audio - Duration: %f"), RecordedVoice->Duration);

	InterruptSpeech(InterruptVoiceFadeOutDuration);

	ForcePlayVoice(RecordedVoice);

	return true;
}

void UConvaiChatbotComponent::ExecuteNarrativeTrigger(const FString TriggerMessage, UConvaiEnvironment* InEnvironment, bool InGenerateActions, bool InVoiceResponse, bool InReplicateOnNetwork)
{
	if (TriggerMessage.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Invoke Speech: TriggerMessage is missing"));
		return;
	}
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Invoke Speech: Executed | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);
	InvokeTrigger_Internal("", TriggerMessage, InEnvironment, InGenerateActions, InVoiceResponse, InReplicateOnNetwork);
}

void UConvaiChatbotComponent::InvokeNarrativeDesignTrigger(const FString TriggerName, UConvaiEnvironment* InEnvironment, bool InGenerateActions, bool InVoiceResponse, bool InReplicateOnNetwork)
{
	if (TriggerName.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Invoke Narrative Design Trigger: TriggerName is missing"));
		return;
	}
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Invoke Narrative Design Trigger: Executed | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);
	InvokeTrigger_Internal(TriggerName, "", InEnvironment, InGenerateActions, InVoiceResponse, InReplicateOnNetwork);
}

void UConvaiChatbotComponent::InvokeTrigger_Internal(const FString& TriggerName, const FString& TriggerMessage, UConvaiEnvironment* InEnvironment, bool InGenerateActions, bool InVoiceResponse, bool InReplicateOnNetwork)
{
	if (TriggerMessage.IsEmpty() && TriggerName.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("InvokeTrigger_Internal: TriggerName and TriggerMessage are missing - Please supply one of them"));
		return;
	}

	if (IsValid(Environment))
	{
		Environment->SetFromEnvironment(InEnvironment);
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("InvokeTrigger_Internal: Environment is not valid"));
	}

	FString Error;
	if (const bool ValidEnvironment = UConvaiActions::ValidateEnvironment(Environment, Error); !ValidEnvironment)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("InvokeTrigger_Internal: %s"), *Error);
		CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("InvokeTrigger_Internal: Environment object seems to have issues -> setting GenerateActions to false"));
		
	}

	InterruptSpeech(InterruptVoiceFadeOutDuration);

	ReplicateVoiceToNetwork = InReplicateOnNetwork;

	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->SendTriggerMessage(TriggerName, TriggerMessage);
	}
}

void UConvaiChatbotComponent::InterruptSpeech(float InVoiceFadeOutDuration)
{
	// Broadcast to clients
	if (UKismetSystemLibrary::IsServer(this) && ReplicateVoiceToNetwork)
	{
		Broadcast_InterruptSpeech(InVoiceFadeOutDuration);
	}

	if (GetIsTalking() || IsProcessing())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("InterruptSpeech: Interrupting character | Character ID : %s | Session ID : %s"),
			*CharacterID,
			*SessionID);

		StopVoiceWithFade(InVoiceFadeOutDuration);

		AsyncTask(ENamedThreads::GameThread, [WeakThis = MakeWeakObjectPtr(this)]
			{
				if (!WeakThis.IsValid())
				{
					// The object is no longer valid or is being destroyed.
					return;
				}
				WeakThis->OnInterruptedEvent.Broadcast(WeakThis.Get(), nullptr);
			});
	}
	else
	{
		StopVoice(); // Make sure to stop the voice either way
	}
}

void UConvaiChatbotComponent::Broadcast_InterruptSpeech_Implementation(float InVoiceFadeOutDuration)
{
	// Execute if you are a client
	if (!UKismetSystemLibrary::IsServer(this))
	{
		InterruptSpeech(InVoiceFadeOutDuration);
	}
}

void UConvaiChatbotComponent::OnRep_EnvironmentData()
{
	if (IsValid(Environment))
	{
		Environment->SetFromEnvironment(ConvaiEnvironmentDetails);
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("OnRep_EnvironmentData: Environment is not valid"));
	}
}

void UConvaiChatbotComponent::UpdateEnvironmentData()
{
	if (IsValid(Environment))
	{
		ConvaiEnvironmentDetails = Environment->ToEnvironmentStruct();
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("UpdateEnvironmentData: Environment is not valid"));
	}
}

void UConvaiChatbotComponent::LoadEnvironment(UConvaiEnvironment* NewConvaiEnvironment)
{
	if (IsValid(Environment))
	{
		Environment->SetFromEnvironment(NewConvaiEnvironment);
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("LoadEnvironment: Environment is not valid"));
	}
}

void UConvaiChatbotComponent::BeginPlay()
{
	Super::BeginPlay();

	Environment = NewObject<UConvaiEnvironment>();

	if (IsValid(Environment))
	{
		Environment->OnEnvironmentChanged.BindUObject(this, &UConvaiChatbotComponent::UpdateEnvironmentData);
	}
	else
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("BeginPlay: Environment is not valid"));
	}

	// Get character details
	if (CharacterID != "")
		ConvaiGetDetails();

	// Register with the ConvaiSubsystem
	if (UGameInstance* GameInstance = GetWorld()->GetGameInstance())
	{
		if (UConvaiSubsystem* ConvaiSubsystem = GameInstance->GetSubsystem<UConvaiSubsystem>())
		{
			if (IsValid(ConvaiSubsystem))
			{
				ConvaiSubsystem->RegisterChatbotComponent(this);
			}
			else
			{
				CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("BeginPlay: ConvaiSubsystem is not valid"));
			}
		}
	}
	
	// Initialize session if auto-initialize is enabled
	if (bAutoInitializeSession)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Auto Initializing Session"));
		StartSession();
	}
}

void UConvaiChatbotComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Unregister from the ConvaiSubsystem
	if (UGameInstance* GameInstance = GetWorld()->GetGameInstance())
	{
		if (UConvaiSubsystem* ConvaiSubsystem = GameInstance->GetSubsystem<UConvaiSubsystem>())
		{
			if (IsValid(ConvaiSubsystem))
			{
				ConvaiSubsystem->UnregisterChatbotComponent(this);
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}

void UConvaiChatbotComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	SendImage(DeltaTime);
}

void UConvaiChatbotComponent::BeginDestroy()
{
	// Fallback unregistration in case EndPlay wasn't called
	if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UConvaiSubsystem* ConvaiSubsystem = GameInstance->GetSubsystem<UConvaiSubsystem>())
		{
			if (IsValid(ConvaiSubsystem))
			{
				ConvaiSubsystem->UnregisterChatbotComponent(this);
			}
		}
	}
	
	if (IsValid(Environment))
	{
		Environment->OnEnvironmentChanged.Unbind();
	}
	
	// Clean up any active session
	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}
	
	Super::BeginDestroy();
}

bool UConvaiChatbotComponent::CanUseLipSync()
{
	return true;
}

bool UConvaiChatbotComponent::CanUseVision()
{
    return true;
}

UConvaiChatBotGetDetailsProxy* UConvaiChatbotComponent::ConvaiGetDetails()
{
	ConvaiChatBotGetDetailsDelegate.BindUFunction(this, "OnConvaiGetDetailsCompleted");

	if (IsValid(ConvaiChatBotGetDetailsProxy))
	{
		ConvaiChatBotGetDetailsProxy->OnSuccess.Clear();
		ConvaiChatBotGetDetailsProxy->OnFailure.Clear();
	}


	ConvaiChatBotGetDetailsProxy = UConvaiChatBotGetDetailsProxy::CreateCharacterGetDetailsProxy(this, CharacterID);
	ConvaiChatBotGetDetailsProxy->OnSuccess.Add(ConvaiChatBotGetDetailsDelegate);
	ConvaiChatBotGetDetailsProxy->OnFailure.Add(ConvaiChatBotGetDetailsDelegate);
	ConvaiChatBotGetDetailsProxy->Activate();
	return ConvaiChatBotGetDetailsProxy;
}

void UConvaiChatbotComponent::OnConvaiGetDetailsCompleted(FString ReceivedCharacterName, FString ReceivedVoiceType, FString ReceivedBackstory, FString ReceivedLanguageCode, bool HasReadyPlayerMeLink, FString ReceivedReadyPlayerMeLink, FString ReceivedAvatarImageLink)
{
	if (ReceivedCharacterName == "" && ReceivedVoiceType == "" && ReceivedBackstory == "")
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("OnConvaiGetDetailsCompleted: Could not get character details for charID:\"%s\""), *CharacterID);
		OnCharacterDataLoadEvent_V2.Broadcast(this, false);
		return;
	}

	CharacterName = ReceivedCharacterName;
	VoiceType = ReceivedVoiceType;
	Backstory = ReceivedBackstory;
	LanguageCode = ReceivedLanguageCode;
	ReadyPlayerMeLink = ReceivedReadyPlayerMeLink;
	AvatarImageLink = ReceivedAvatarImageLink;

	OnCharacterDataLoadEvent_V2.Broadcast(this, true);
	ConvaiChatBotGetDetailsProxy = nullptr;
}

// IConvaiConnectionInterface implementation
void UConvaiChatbotComponent::OnConnectedToServer()
{
	
}

void UConvaiChatbotComponent::OnDisconnectedFromServer()
{
	
}

void UConvaiChatbotComponent::OnParticipantConnected(FString ParticipantId)
{
	//TODO @ Anmol check if the participant id is character id

	OnParticipantConnectionStateChangedEvent.Broadcast(this, ParticipantId, EC_ConnectionState::Connected);
	
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->UpdateTemplateKeys(NarrativeTemplateKeys);
		SessionProxyInstance->UpdateDynamicInfo(DynamicEnvironmentInfo);
	}
}

void UConvaiChatbotComponent::OnParticipantDisconnected(FString ParticipantId)
{
	OnParticipantConnectionStateChangedEvent.Broadcast(this, ParticipantId, EC_ConnectionState::Disconnected);
}

void UConvaiChatbotComponent::OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal)
{
	const bool bHasContent = !Transcription.IsEmpty() || IsFinal;
	if (!bHasContent)
		return;

	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakThis(this);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Transcription, IsTranscriptionReady, IsFinal]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->OnTranscriptionReceived(Transcription, IsTranscriptionReady, IsFinal);
			}
		});
		return;
	}

	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("UConvaiChatbotComponent Transcription received: %s | Character ID : %s | Session ID : %s"),
		*Transcription,
		*CharacterID,
		*SessionID);

	OnTranscriptionReceivedDelegate.Broadcast(this, nullptr, Transcription, IsTranscriptionReady, IsFinal);
}

void UConvaiChatbotComponent::OnFaceDataReceived(FAnimationSequence FaceDataAnimation)
{
	AddFaceDataToSend(FaceDataAnimation);
}

void UConvaiChatbotComponent::OnSessionIDReceived(const FString ReceivedSessionID)
{
	SessionID = ReceivedSessionID;
}

void UConvaiChatbotComponent::OnInteractionIDReceived(FString ReceivedInteractionID)
{
	if (IsInGameThread())
	{
		// Send Interaction ID to blueprint event
		OnInteractionIDReceivedEvent.Broadcast(this, nullptr, ReceivedInteractionID);
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [this, ReceivedInteractionID]
			{
				// Send Interaction ID to blueprint event
				OnInteractionIDReceivedEvent.Broadcast(this, nullptr, ReceivedInteractionID);
			});
	}
}

void UConvaiChatbotComponent::OnActionSequenceReceived(const TArray<FConvaiResultAction>& ReceivedSequenceOfActions)
{
	if (UConvaiUtils::IsNewActionSystemEnabled())
	{
		bool ActionsAlreadyStarted = !IsActionsQueueEmpty();

		// Fill the current queue of actions
		AppendActionsToQueue(ReceivedSequenceOfActions);

		if (!ActionsAlreadyStarted)
			StartFirstAction();
	}

	// Broadcast the actions
	AsyncTask(ENamedThreads::GameThread, [this, ReceivedSequenceOfActions] {
		OnActionReceivedEvent_V2.Broadcast(this, nullptr, ReceivedSequenceOfActions);
		});
}

void UConvaiChatbotComponent::OnEmotionReceived(FString ReceivedEmotionResponse, FAnimationFrame EmotionBlendshapesFrame, bool MultipleEmotions)
{
	if (LockEmotionState)
		return;

	// Update the emotion state
	if (!ReceivedEmotionResponse.IsEmpty())
	{
		if (MultipleEmotions)
		{
			EmotionState.SetEmotionData(ReceivedEmotionResponse, EmotionOffset);
		}
		else
		{
			EmotionState.SetEmotionDataSingleEmotion(ReceivedEmotionResponse, EmotionOffset);
			//EmotionBlendshapes = EmotionBlendshapesFrame.BlendShapes;
		}
	}

	// Broadcast the emotion state changed event
	AsyncTask(ENamedThreads::GameThread, [this] {
		OnEmotionStateChangedEvent.Broadcast(this, nullptr);
		});
}

void UConvaiChatbotComponent::OnNarrativeSectionReceived(FString BT_Code, FString BT_Constants, FString ReceivedNarrativeSectionID)
{
	AsyncTask(ENamedThreads::GameThread, [this, ReceivedNarrativeSectionID]
		{
			OnNarrativeSectionReceivedEvent.Broadcast(this, ReceivedNarrativeSectionID);
		});
}

void UConvaiChatbotComponent::OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels)
{
	if (!IsConnectionTalking)
	{
		// No timestamp yet? Treat as just-finished: skip work this call.
		if (FinishedTalkingTimestamp < 0.0)
		{
			return;
		}

		if (FPlatformTime::Seconds() - FinishedTalkingTimestamp >= AudioContentCheckDelay)
		{
			// Past the delay window → always return
			return;
		}
		
		// Inside the window → only continue if there’s real audio
		if (!UConvaiUtils::ContainsAudioContent(AudioData, NumFrames, NumChannels))
		{
			TryPlayBufferedContent(true);
			return;
		}
			
		FinishedTalkingTimestamp = FPlatformTime::Seconds();
	}

    // Convert the audio data to a format that can be used by the existing methods
	const size_t BytesPerSample = BitsPerSample / 8;
	const size_t TotalBytes = NumFrames * NumChannels * BytesPerSample;

	//float ReceivedAudioDuration = static_cast<float>(TotalBytes) / static_cast<float>(SampleRate * NumChannels * BytesPerSample);
	//UE_LOG(LogTemp, Display, TEXT("Audio Duration: %f seconds"), ReceivedAudioDuration);
	
    if (TotalBytes > 0)
    {
        // Cast AudioData to uint8* for compatibility with AddPCMDataToSend
        AddPCMDataToSend(TArray<uint8>((uint8*)AudioData, TotalBytes), false, SampleRate, NumChannels);

        if (IsRecordingAudio)
        {
            // Directly append to RecordedAudio without intermediate copy
            RecordedAudio.Append((uint8*)AudioData, TotalBytes);
            RecordedAudioSampleRate = SampleRate;
        }
    }
}

void UConvaiChatbotComponent::OnStartedTalking()
{
	IsConnectionTalking = true;
	FinishedTalkingTimestamp = -1.0;
	UE_LOG(LogTemp, Display, TEXT("Audio left %f"),AudioBuffer.GetTotalDuration());
	AudioBuffer.Reset();
}

void UConvaiChatbotComponent::OnFinishedTalking()
{
	IsConnectionTalking = false;
	FinishedTalkingTimestamp = FPlatformTime::Seconds();
}

void UConvaiChatbotComponent::OnFailure(FString Message)
{
	CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("UConvaiChatbotComponent Get Response Failed! | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);

	// Broadcast the failure
	AsyncTask(ENamedThreads::GameThread, [this] {OnFailureEvent.Broadcast(); });

    // Log the failure
    CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Connection failure: %s"), *Message);
}

void UConvaiChatbotComponent::StartSession()
{
    // If we already have a session, shut it down first
    if (IsValid(SessionProxyInstance))
    {
        StopSession();
    }
    
    // Create a new session proxy
    SessionProxyInstance = NewObject<UConvaiConnectionSessionProxy>(this);
    if (!IsValid(SessionProxyInstance))
    {
        CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Failed to create session proxy"));
        return;
    }
    
    // Initialize the session proxy
    if (!SessionProxyInstance->Initialize(this, false))
    {
        CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Failed to initialize session proxy"));
        SessionProxyInstance = nullptr;
        return;
    }
    
    // Connect the session with the character ID
    if (!SessionProxyInstance->Connect(CharacterID))
    {
        CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Failed to connect session"));
        SessionProxyInstance = nullptr;
        return;
    }
    
    CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Session started successfully for character ID: %s"), *CharacterID);
}

void UConvaiChatbotComponent::StopSession()
{
    if (IsValid(SessionProxyInstance))
    {
        // Disconnect the session
        SessionProxyInstance->Disconnect();
        SessionProxyInstance = nullptr;
        
        CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Session stopped for character ID: %s"), *CharacterID);
    }
}

void UConvaiChatbotComponent::SendImage(const float& DeltaTime)
{
	if (!ConvaiVision || ConvaiVision->GetState() != EVisionState::Capturing || !IsValid(SessionProxyInstance))
	{
		return;
	}

	// Keep our send cadence aligned to ConvaiVision's advertised FPS
	const int32 VisionFps = ConvaiVision->GetMaxFPS();
	if (VisionFps != CachedVisionFPS && VisionFps > 0)
	{
		CachedVisionFPS = FMath::Clamp(VisionFps, 1, 60); 
		TargetFrameInterval = 1.f / static_cast<float>(CachedVisionFPS);
	}
	else if (VisionFps <= 0)
	{
		// Failsafe: if device reports 0/invalid, fall back to default (15)
		CachedVisionFPS = FMath::Clamp(15, 1, 60); 
		TargetFrameInterval = 1.f / static_cast<float>(CachedVisionFPS);
	}

	// Accumulate time and only proceed when we're due to send a frame
	TimeSinceLastVideoSend += DeltaTime;
	if (TimeSinceLastVideoSend < TargetFrameInterval)
	{
		return; // not time yet—skip all capture work this tick
	}

	TimeSinceLastVideoSend -= TargetFrameInterval;
	if (TimeSinceLastVideoSend < 0.f) TimeSinceLastVideoSend = 0.f;

	int32 Width = 0, Height = 0;
	TArray<uint8> Data;
	bool bCaptureSuccess = false;
	if (!bCaptureSuccess)
	{
		bCaptureSuccess = ConvaiVision->CaptureRaw(Width, Height, Data);
	}

	// 0 = never logged, 1 = logged failure, 2 = logged success
	static uint8 LogState = 0; 
	if (bCaptureSuccess && Width > 0 && Height > 0 && Data.Num() > 0)
	{
		SessionProxyInstance->SendImage(Width, Height, Data);

		if (LogState != 2) // only log once when switching to success
		{
			CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("SendImage: Sending raw image"));
			LogState = 2;
		}
	}
	else
	{
		if (LogState != 1) // only log once when switching to failure
		{
			UE_LOG(ConvaiChatbotComponentLog, Warning, TEXT("SendImage: Unable to capture Raw data"));
			LogState = 1;
		}
	}
}
