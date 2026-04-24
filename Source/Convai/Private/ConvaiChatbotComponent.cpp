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
#include "ConvaiFaceSync.h"
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
	// Calculate elapsed time based on actual audio playback
	return static_cast<float>(GetAudioPlaybackTime());
}

float UConvaiChatbotComponent::GetTalkingTimeRemaining()
{

	return static_cast<float>(GetRemainingContentDuration());
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

void UConvaiChatbotComponent::UpdateContext(const FString& Text, EC_ContextUpdateMode Mode, EC_RunLLMOption ShouldRespond)
{
#if ConvaiDebugMode
	auto ModeToString = [](EC_ContextUpdateMode M) -> const TCHAR*
	{
		switch (M)
		{
			case EC_ContextUpdateMode::Append:  return TEXT("Append");
			case EC_ContextUpdateMode::Replace: return TEXT("Replace");
			case EC_ContextUpdateMode::Reset:   return TEXT("Reset");
			default:                            return TEXT("Unknown");
		}
	};
	auto RunLLMToString = [](EC_RunLLMOption R) -> const TCHAR*
	{
		switch (R)
		{
			case EC_RunLLMOption::Always: return TEXT("Always");
			case EC_RunLLMOption::Auto:   return TEXT("Auto");
			case EC_RunLLMOption::Never:  return TEXT("Never");
			default:                      return TEXT("Unknown");
		}
	};
	CONVAI_LOG(ConvaiChatbotComponentLog, Log,
		TEXT("UpdateContext [%s | ShouldRespond=%s] | Character ID : %s | Session ID : %s\n----- BEGIN CONTEXT -----\n%s\n----- END CONTEXT -----"),
		ModeToString(Mode), RunLLMToString(ShouldRespond),
		*CharacterID, *SessionID,
		*Text);
#endif

	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->UpdateContext(Text, Mode, ShouldRespond);
	}
}

// ── Dynamic Context ──────────────────────────────────────────────

bool UConvaiChatbotComponent::IsChatbotConnected() const
{
	return IsValid(SessionProxyInstance) && GetChatbotConnectionState() == EC_ConnectionState::Connected;
}

void UConvaiChatbotComponent::ScheduleContextFlush()
{
	NextContextFlushTime = FPlatformTime::Seconds() + FMath::Max(0.0f, ContextAggregationDelay);
}


void UConvaiChatbotComponent::SetContextState(const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond)
{
	PendingContextBatch.StageState(DynamicContextTracker, Name, Value, ShouldRespond);
	ScheduleContextFlush();
}

void UConvaiChatbotComponent::SetContextStates(const TMap<FString, FString>& States, EC_RunLLMOption ShouldRespond)
{
	if (States.Num() == 0)
	{
		return;
	}
	for (const auto& Pair : States)
	{
		PendingContextBatch.StageState(DynamicContextTracker, Pair.Key, Pair.Value, ShouldRespond);
	}
	ScheduleContextFlush();
}

void UConvaiChatbotComponent::AddContextEvent(const FString& Text, EC_RunLLMOption ShouldRespond)
{
	PendingContextBatch.StageEvent(Text, ShouldRespond);
	ScheduleContextFlush();
}

void UConvaiChatbotComponent::RemoveContextState(const FString& Name)
{
	// Drop any staged set for this key so the upcoming flush won't re-add it.
	PendingContextBatch.DropStateKey(Name);
	if (!DynamicContextTracker.RemoveState(Name))
	{
		return;
	}

	// Batch like the other updates — rapid removes coalesce into one
	// canonical Replace on flush.
	PendingContextBatch.bForceReplace = true;
	ScheduleContextFlush();
}

void UConvaiChatbotComponent::ResetDynamicContext()
{
	// Reset is terminal for queued triggers — they belonged to the pre-reset
	// conversation and shouldn't carry over.
	PendingTriggers.Empty();

	// Mark the reset. The flush drives the ordering: drain the staged batch
	// first, emit the Reset afterwards, then wipe the local tracker.  We
	// deliberately DO NOT clear the tracker or staged batch up-front so any
	// in-flight updates still get delivered.
	PendingContextBatch.bPendingReset = true;

	if (IsChatbotConnected())
	{
		FlushDynamicContext();
	}
	else
	{
		ScheduleContextFlush();
	}
}

bool UConvaiChatbotComponent::GetContextStateValue(const FString& Name, FString& OutValue) const
{
	return DynamicContextTracker.GetStateValue(Name, OutValue);
}

void UConvaiChatbotComponent::FlushDynamicContext()
{
	const bool bHasStagedBatch = PendingContextBatch.HasStagedWork();
	const bool bHasReset       = PendingContextBatch.bPendingReset;
	const bool bHasTriggers    = PendingTriggers.Num() > 0;

	if (!bHasStagedBatch && !bHasReset && !bHasTriggers)
	{
		NextContextFlushTime = -1.0;
		return;
	}

	// ── Step 1: drain the staged batch (state + events) ────────────
	// State values are already in the tracker (StageState writes through).
	// Build a delta summary of what changed this batch, commit events to the
	// tracker, then send one Replace containing the full canonical context
	// followed by the delta so the LLM knows both the current state and what
	// specifically changed.
	if (bHasStagedBatch)
	{
		TArray<FString> DeltaLines;
		DeltaLines.Reserve(PendingContextBatch.StagedStateOrder.Num() + PendingContextBatch.EventsOrdered.Num());

		for (const FString& Key : PendingContextBatch.StagedStateOrder)
		{
			FString CurrentValue;
			DynamicContextTracker.GetStateValue(Key, CurrentValue);
			const FString* OldValue = PendingContextBatch.OldValues.Find(Key);
			if (OldValue)
				DeltaLines.Add(FString::Printf(TEXT("%s changed from %s to %s"), *Key, **OldValue, *CurrentValue));
			else
				DeltaLines.Add(FString::Printf(TEXT("%s is %s"), *Key, *CurrentValue));
		}

		for (const FString& Event : PendingContextBatch.EventsOrdered)
		{
			DynamicContextTracker.AddEvent(Event);
		}

		FString Canonical = DynamicContextTracker.BuildCanonicalContext();
		FString Payload = DeltaLines.Num() > 0
			? Canonical + TEXT("\n") + FString::Join(DeltaLines, TEXT("\n"))
			: Canonical;

		UpdateContext(Payload, EC_ContextUpdateMode::Replace, PendingContextBatch.AggregateRunLLM);
	}
	PendingContextBatch.ClearStaged();

	// ── Step 2: fire queued triggers after the context update ───────
	for (const FPendingTrigger& Trigger : PendingTriggers)
	{
		InvokeTrigger_Internal(Trigger.TriggerName, TEXT(""),
			Trigger.bGenerateActions, Trigger.bReplicateOnNetwork);
	}
	PendingTriggers.Empty();

	// ── Step 3: pending reset LAST so the batch/triggers above drain first ──
	if (bHasReset)
	{
		UpdateContext(TEXT(""), EC_ContextUpdateMode::Reset, EC_RunLLMOption::Never);
		DynamicContextTracker.Reset();
		PendingContextBatch.bPendingReset = false;
	}

	NextContextFlushTime = -1.0;
}

void UConvaiChatbotComponent::TickDynamicContext()
{
	if (NextContextFlushTime < 0.0)
	{
		return;
	}
	if (!PendingContextBatch.HasWork() && PendingTriggers.Num() == 0)
	{
		NextContextFlushTime = -1.0;
		return;
	}
	if (!IsChatbotConnected())
	{
		// Keep accumulating until we reconnect.
		return;
	}
	if (FPlatformTime::Seconds() >= NextContextFlushTime)
	{
		FlushDynamicContext();
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

		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelfForAction(this);
		AsyncTask(ENamedThreads::GameThread, [WeakSelfForAction, ConvaiResultAction]
		{
			if (WeakSelfForAction.IsValid())
			{
				WeakSelfForAction->TriggerNamedBlueprintAction(ConvaiResultAction.Action, ConvaiResultAction);
			}
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

EC_ConnectionState UConvaiChatbotComponent::GetChatbotConnectionState() const
{
	if (IsValid(SessionProxyInstance))
	{
		return SessionProxyInstance->GetConnectionState();
	}
	return EC_ConnectionState::Disconnected;
}

void UConvaiChatbotComponent::ExecuteNarrativeTrigger(const FString TriggerMessage, bool InGenerateActions, bool InReplicateOnNetwork)
{
	if (TriggerMessage.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Invoke Speech: TriggerMessage is missing"));
		return;
	}
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Invoke Speech: Executed | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);

	AddContextEvent(TriggerMessage, EC_RunLLMOption::Always);
}

void UConvaiChatbotComponent::InvokeNarrativeDesignTrigger(const FString TriggerName, bool InGenerateActions, bool InReplicateOnNetwork)
{
	if (TriggerName.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Invoke Narrative Design Trigger: TriggerName is missing"));
		return;
	}
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Invoke Narrative Design Trigger: Executed | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);

	// While disconnected, narrative design triggers are always queued as their
	// own command — never merged into context — and fired after the flush.
	if (!IsChatbotConnected())
	{
		FPendingTrigger Pending;
		Pending.TriggerName = TriggerName;
		Pending.bGenerateActions = InGenerateActions;
		Pending.bReplicateOnNetwork = InReplicateOnNetwork;
		PendingTriggers.Add(MoveTemp(Pending));
		ScheduleContextFlush();
		return;
	}

	InvokeTrigger_Internal(TriggerName, "", InGenerateActions, InReplicateOnNetwork);
}

void UConvaiChatbotComponent::InvokeTrigger_Internal(const FString& TriggerName, const FString& TriggerMessage, bool InGenerateActions, bool InReplicateOnNetwork)
{
	if (TriggerMessage.IsEmpty() && TriggerName.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("InvokeTrigger_Internal: TriggerName and TriggerMessage are missing - Please supply one of them"));
		return;
	}

	InterruptSpeech(InterruptVoiceFadeOutDuration);

	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->SendTriggerMessage(TriggerName, TriggerMessage);
	}
}

void UConvaiChatbotComponent::InterruptSpeech(float InVoiceFadeOutDuration)
{

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

	TickDynamicContext();
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

EC_LipSyncMode UConvaiChatbotComponent::GetLipSyncMode()
{
	if (!ConvaiLipSync)
		FindFirstLipSyncComponent();

	if (!ConvaiLipSync)
		return EC_LipSyncMode::Off;

	return ConvaiLipSync->GetLipSyncMode();
}

bool UConvaiChatbotComponent::RequiresPrecomputedFaceData()
{
	if (!ConvaiLipSync)
		FindFirstLipSyncComponent();

	if (!ConvaiLipSync)
		return true;

	return ConvaiLipSync->RequiresPrecomputedFaceData();
}

// IConvaiConnectionInterface implementation
void UConvaiChatbotComponent::OnConnectedToServer()
{
	// Server connection established, but attendee not yet connected to WebRTC room
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("OnConnectedToServer"))
}

void UConvaiChatbotComponent::OnDisconnectedFromServer()
{
	StopVoice();
	IsConnectionTalking = false;
}

void UConvaiChatbotComponent::BroadcastConnectionStateChanged(const FString& AttendeeId, EC_ConnectionState State)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, AttendeeId, State]()
		{
			if (UConvaiChatbotComponent* Component = WeakThis.Get())
			{
				Component->OnAttendeeConnectionStateChangedEvent.Broadcast(Component, AttendeeId, State);
			}
		});
		return;
	}

	// Already on game thread - broadcast directly
	OnAttendeeConnectionStateChangedEvent.Broadcast(this, AttendeeId, State);
}

void UConvaiChatbotComponent::OnAttendeeConnecting()
{
	BroadcastConnectionStateChanged("", EC_ConnectionState::Connecting);
}

void UConvaiChatbotComponent::OnAttendeeConnected(FString AttendeeId)
{
	//TODO @ Anmol check if the Attendee id is character id

	BroadcastConnectionStateChanged(AttendeeId, EC_ConnectionState::Connected);

	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->UpdateTemplateKeys(NarrativeTemplateKeys);
		SessionProxyInstance->UpdateDynamicInfo(DynamicEnvironmentInfo);
	}
	
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("OnAttendeeConnected"))
}

void UConvaiChatbotComponent::OnAttendeeDisconnected(FString AttendeeId)
{
	BroadcastConnectionStateChanged(AttendeeId, EC_ConnectionState::Disconnected);
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
	// Skip face data processing if interrupted by user speaking
	if (bIsInterrupted)
		return;

	HandleLipSyncReceived(FaceDataAnimation);
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
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelfForInteraction(this);
		AsyncTask(ENamedThreads::GameThread, [WeakSelfForInteraction, ReceivedInteractionID]
			{
				if (WeakSelfForInteraction.IsValid())
				{
					// Send Interaction ID to blueprint event
					WeakSelfForInteraction->OnInteractionIDReceivedEvent.Broadcast(WeakSelfForInteraction.Get(), nullptr, ReceivedInteractionID);
				}
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
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelfForActions(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelfForActions, ReceivedSequenceOfActions] {
		if (WeakSelfForActions.IsValid())
		{
			WeakSelfForActions->OnActionReceivedEvent_V2.Broadcast(WeakSelfForActions.Get(), nullptr, ReceivedSequenceOfActions);
		}
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
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelfForEmotion(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelfForEmotion] {
		if (WeakSelfForEmotion.IsValid())
		{
			WeakSelfForEmotion->OnEmotionStateChangedEvent.Broadcast(WeakSelfForEmotion.Get(), nullptr);
		}
		});
}

void UConvaiChatbotComponent::OnNarrativeSectionReceived(FString BT_Code, FString BT_Constants, FString ReceivedNarrativeSectionID)
{
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelfForNarrative(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelfForNarrative, ReceivedNarrativeSectionID]
		{
			if (WeakSelfForNarrative.IsValid())
			{
				WeakSelfForNarrative->OnNarrativeSectionReceivedEvent.Broadcast(WeakSelfForNarrative.Get(), ReceivedNarrativeSectionID);
			}
		});
}

void UConvaiChatbotComponent::OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels)
{
	if (NumFrames == 0)
		return;

	// Skip audio processing if interrupted by user speaking
	if (bIsInterrupted)
		return;

	if (!IsConnectionTalking)
	{
		// No timestamp yet? Treat as just-finished: skip work this call.
		if (FinishedTalkingTimestamp < 0.0)
		{
			return;
		}

		if (FPlatformTime::Seconds() - FinishedTalkingTimestamp >= AudioContentCheckDelay)
		{
			// Past the delay window → mark end of audio once and return
			if (!bHasMarkedEndOfAudio && SupportsLipSync())
			{
				MarkEndOfAudio();
				bHasMarkedEndOfAudio = true;
			}
			return;
		}
		
		// Inside the window → only continue if there’s real audio
		if (!UConvaiUtils::ContainsAudioContent(AudioData, NumFrames, NumChannels))
		{
			// No real audio content, but force play any buffered audio
			UConvaiAudioStreamer::ForcePlayBufferedAudio();
			return;
		}
			
		FinishedTalkingTimestamp = FPlatformTime::Seconds();
	}

	// Track silence statistics - only count continuous silence > 0.5s
	TotalAudioFramesReceived += NumFrames;

    // Convert the audio data to a format that can be used by the existing methods
	const size_t BytesPerSample = BitsPerSample / 8;
	const size_t TotalBytes = NumFrames * NumChannels * BytesPerSample;

	//float ReceivedAudioDuration = static_cast<float>(TotalBytes) / static_cast<float>(SampleRate * NumChannels * BytesPerSample);
	//UE_LOG(LogTemp, Display, TEXT("Audio Duration: %f seconds"), ReceivedAudioDuration);
	
    if (TotalBytes > 0)
    {
        // Directly call HandleAudioReceived
        HandleAudioReceived((uint8*)AudioData, TotalBytes, false, SampleRate, NumChannels);
    }
}

void UConvaiChatbotComponent::OnStartedTalking()
{
	IsConnectionTalking = true;
	FinishedTalkingTimestamp = -1.0;
	bHasMarkedEndOfAudio = false;
}

void UConvaiChatbotComponent::OnFinishedTalking()
{
	IsConnectionTalking = false;
	FinishedTalkingTimestamp = FPlatformTime::Seconds();
}

void UConvaiChatbotComponent::OnLLMStarted()
{
	// Bot is about to speak - Reset all buffers and audio tracking for safety
	StopVoice();
}

void UConvaiChatbotComponent::OnLLMStopped()
{
}

void UConvaiChatbotComponent::OnInterrupt()
{
	InterruptSpeech(0);
	bIsInterrupted = true;
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Character interrupted by user speaking"));
}

void UConvaiChatbotComponent::OnInterruptEnd()
{
	bIsInterrupted = false;
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Character interrupt ended"));
}

void UConvaiChatbotComponent::onAudioFinished()
{
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("onAudioFinished - IsConnectionTalking: %s - bHasMarkedEndOfAudio: %s"), IsConnectionTalking ? TEXT("true") : TEXT("false"), bHasMarkedEndOfAudio ? TEXT("true") : TEXT("false"));

	bIsPlayingAudio = false;

	// Broadcast that audio has finished
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf] {
		if (WeakSelf.IsValid())
		{
			WeakSelf->OnFinishedTalkingDelegate.Broadcast();
		}
	});
	IsTalking = false;

	// Decide whether to pause or stop lipsync based on whether more audio is expected
	if (IsConnectionTalking)
	{
		// Connection is still active, more audio may be coming → pause lipsync
		CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Pausing lipsync - more audio expected"));
		PauseLipSync();
	}
	else
	{
		// Connection finished, no more audio expected → reset buffers
		CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Stopping voice - no more audio expected"));
		StopVoice();
	}
}

void UConvaiChatbotComponent::OnFailure(FString Message)
{
	CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("UConvaiChatbotComponent Get Response Failed! | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);

	// Broadcast the failure
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelfForFailure(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelfForFailure] {
		if (WeakSelfForFailure.IsValid())
		{
			WeakSelfForFailure->OnFailureEvent.Broadcast();
		}
	});

    // Log the failure
    CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Connection failure: %s"), *Message);
}

void UConvaiChatbotComponent::StartSession()
{
	switch (GetLipSyncMode()) {
	case EC_LipSyncMode::Off:
	case EC_LipSyncMode::Auto:
	break;
	case EC_LipSyncMode::VisemeBased:
	case EC_LipSyncMode::BS_MHA:
	case EC_LipSyncMode::BS_ARKit:
	case EC_LipSyncMode::BS_CC4_Extended:
		{
			if (!UConvaiSettingsUtils::GetParamValueAsFloat("MinBufferDuration", MinBufferDuration))
			{
				MinBufferDuration = 0.12;
			}
		}
		break;
	}

	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}

	SessionProxyInstance = UConvaiConnectionSessionProxy::RequestSessionProxy(this, CharacterID, this);

	if (!IsValid(SessionProxyInstance))
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Failed to acquire connection for character ID: %s"), *CharacterID);
		BroadcastConnectionStateChanged("", EC_ConnectionState::Disconnected);
		return;
	}

	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Session started for character ID: %s"), *CharacterID);
}

void UConvaiChatbotComponent::StopSession()
{
	if (IsValid(SessionProxyInstance))
	{
		UConvaiConnectionSessionProxy::ReturnSessionProxy(this, CharacterID, SessionProxyInstance);
		SessionProxyInstance = nullptr;
		CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Session stopped for character ID: %s"), *CharacterID);
	}
}

void UConvaiChatbotComponent::SendImage(const float& DeltaTime)
{
	if (!ConvaiVision || ConvaiVision->GetState() != EVisionState::Capturing || !IsValid(SessionProxyInstance))
	{
		if (bWasPublishingVideo && IsValid(SessionProxyInstance))
		{
			SessionProxyInstance->StopVideoPublishing();
			bWasPublishingVideo = false;
		}
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
		bWasPublishingVideo = true;

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
