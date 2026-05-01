// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiChatbotComponent.h"
#include "Environment/ConvaiEnvironmentLegacy.h"
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
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Pawn.h"

DEFINE_LOG_CATEGORY(ConvaiChatbotComponentLog);

UConvaiChatbotComponent::UConvaiChatbotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 1 / 15;
	InterruptVoiceFadeOutDuration = 1.0;
	bAutoInitializeSession = true;
}

void UConvaiChatbotComponent::RefreshActionRenderedStrings()
{
	for (FConvaiAction& A : EnvironmentData.Actions)
	{
		// No-op when already in sync — without this, every refresh marks the asset
		// dirty and emits an undo entry, which can cascade with editor reloads.
		const FString Fresh = A.ToActionConfigString();
		if (A.RenderedString != Fresh)
		{
			A.RenderedString = Fresh;
		}
	}
}

void UConvaiChatbotComponent::PostInitProperties()
{
	Super::PostInitProperties();
#if WITH_EDITOR
	// Skip the CDO and any object that's still in the load pipeline — Super may have
	// posted async work that's not safe to touch yet (was hitting "pure virtual called"
	// when a Blueprint subclass instance was being initialized before its vtable was patched).
	// PostLoad picks up real instances; this is just for newly-spawned-in-editor cases.
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects))
	{
		RefreshActionRenderedStrings();
	}
#endif
}

void UConvaiChatbotComponent::PostLoad()
{
	Super::PostLoad();
#if WITH_EDITOR
	// Re-render after deserialization so older saved chatbots pick up wire-format
	// changes (e.g. the new `: ref` hint) without a manual re-edit.
	RefreshActionRenderedStrings();
#endif
}

#if WITH_EDITOR
void UConvaiChatbotComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Re-entrancy guard: when we write back to Name/Description/Parameters during
	// preview-string parsing, that triggers another PostEditChangeProperty call.
	static thread_local bool bInsideActionPreviewSync = false;
	if (bInsideActionPreviewSync) { return; }
	TGuardValue<bool> Guard(bInsideActionPreviewSync, true);

	const FName ChangedName = PropertyChangedEvent.GetPropertyName();
	const bool bRenderedEdited = (ChangedName == GET_MEMBER_NAME_CHECKED(FConvaiAction, RenderedString));

	for (FConvaiAction& A : EnvironmentData.Actions)
	{
		if (bRenderedEdited && !A.RenderedString.IsEmpty())
		{
			// Only treat the box as user-edited when it actually differs from a fresh
			// render of the structured fields. Without this guard, any commit event
			// (focus changes, accidental clicks) would parse-back the unchanged text
			// and silently downgrade types like Reference to Auto.
			const FString FreshRender = A.ToActionConfigString();
			if (A.RenderedString != FreshRender)
			{
				FConvaiAction Parsed;
				if (FConvaiAction::ParseFromActionConfigString(A.RenderedString, Parsed))
				{
					A.Name        = Parsed.Name;
					A.Description = Parsed.Description;
					A.Parameters  = Parsed.Parameters;
				}
			}
		}

		// Re-render only when the box would actually change — otherwise we'd dirty
		// the asset on every PostEditChangeProperty call, which can chain.
		const FString Fresh = A.ToActionConfigString();
		if (A.RenderedString != Fresh)
		{
			A.RenderedString = Fresh;
		}
	}
}
#endif

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
	// ActionsQueue intentionally not replicated — see header comment.
	DOREPLIFETIME(UConvaiChatbotComponent, EmotionState);
	DOREPLIFETIME(UConvaiChatbotComponent, LockEmotionState);
	DOREPLIFETIME(UConvaiChatbotComponent, EnvironmentData);
	DOREPLIFETIME(UConvaiChatbotComponent, bEnableActions);
	DOREPLIFETIME(UConvaiChatbotComponent, ConversationPartner);
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

void UConvaiChatbotComponent::UpdateContextWithAttention(const FString& Text, EC_ContextUpdateMode Mode,
	EC_RunLLMOption ShouldRespond, const FConvaiObjectEntry* OptionalAttention)
{
	if (IsValid(SessionProxyInstance))
	{
		SessionProxyInstance->UpdateContext(Text, Mode, ShouldRespond, OptionalAttention);
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


void UConvaiChatbotComponent::SetContextState(const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond, bool bFlushImmediately)
{
	PendingContextBatch.StageState(DynamicContextTracker, Name, Value, ShouldRespond);
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

void UConvaiChatbotComponent::SetContextStates(const TMap<FString, FString>& States, EC_RunLLMOption ShouldRespond, bool bFlushImmediately)
{
	if (States.Num() == 0)
	{
		return;
	}
	for (const auto& Pair : States)
	{
		PendingContextBatch.StageState(DynamicContextTracker, Pair.Key, Pair.Value, ShouldRespond);
	}
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

void UConvaiChatbotComponent::AddContextEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bFlushImmediately)
{
	PendingContextBatch.StageEvent(Text, ShouldRespond);
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

void UConvaiChatbotComponent::RemoveContextState(const FString& Name, bool bFlushImmediately)
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
	ScheduleOrFlushDynamicContext(bFlushImmediately);
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
	const bool bHasAttention   = PendingContextBatch.bHasPendingAttention;
	const bool bHasReset       = PendingContextBatch.bPendingReset;
	const bool bHasTriggers    = PendingTriggers.Num() > 0;
	const bool bHasEnvWork     = PendingEnvironmentBatch.HasWork();

	if (!bHasStagedBatch && !bHasAttention && !bHasReset && !bHasTriggers && !bHasEnvWork)
	{
		NextContextFlushTime = -1.0;
		return;
	}

	// ── Step 1: drain the staged batch (state + events + optional attention) ─
	// State values are already in the tracker (StageState writes through).
	// Build a delta summary of what changed this batch, commit events to the
	// tracker, promote the (last-wins) attention text to a single tracker event,
	// then send one Replace containing the full canonical context followed by
	// the delta. If attention is staged, fold current_attention_object into the
	// same context-update so both lanes reach the server in one message.
	if (bHasStagedBatch || bHasAttention)
	{
		TArray<FString> DeltaLines;
		DeltaLines.Reserve(PendingContextBatch.StagedStateOrder.Num() + PendingContextBatch.EventsOrdered.Num() + 1);

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

		// Last-wins attention text: promote to exactly one tracker event so the
		// canonical context contains it once, regardless of how many
		// SetObjectInAttention calls landed in the window.
		if (bHasAttention && !PendingContextBatch.PendingAttentionText.IsEmpty())
		{
			DynamicContextTracker.AddEvent(PendingContextBatch.PendingAttentionText);
			DeltaLines.Add(PendingContextBatch.PendingAttentionText);
		}

		FString Canonical = DynamicContextTracker.BuildCanonicalContext();
		FString Payload = DeltaLines.Num() > 0
			? Canonical + TEXT("\n") + FString::Join(DeltaLines, TEXT("\n"))
			: Canonical;

		const FConvaiObjectEntry* AttentionPtr = bHasAttention ? &PendingContextBatch.PendingAttentionObject : nullptr;
		UpdateContextWithAttention(Payload, EC_ContextUpdateMode::Replace, PendingContextBatch.AggregateRunLLM, AttentionPtr);
	}
	PendingContextBatch.ClearStaged();

	// ── Step 2: env-batch — emit one update-scene-metadata snapshot ──
	if (bHasEnvWork)
	{
		// Subtract connect-time entries so action_config-bound objects/characters aren't
		// re-sent via update-scene-metadata (which is descriptive-only on the server).
		TArray<FConvaiObjectEntry> ScenePayload;
		ScenePayload.Reserve(EnvironmentData.Objects.Num() + EnvironmentData.Characters.Num());
		for (const FConvaiObjectEntry& O : EnvironmentData.Objects)
		{
			if (!EnvironmentSnapshotAtConnect.Contains(O.Name))
			{
				ScenePayload.Add(O);
			}
		}
		for (const FConvaiObjectEntry& C : EnvironmentData.Characters)
		{
			if (!EnvironmentSnapshotAtConnect.Contains(C.Name))
			{
				ScenePayload.Add(C);
			}
		}

		if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
		{
			if (UConvaiSubsystem* Subsystem = GameInstance->GetSubsystem<UConvaiSubsystem>())
			{
				Subsystem->UpdateSceneMetadata(SessionProxyInstance, ScenePayload);
			}
		}
		PendingEnvironmentBatch.Clear();
	}

	// ── Step 3: fire queued triggers after the context update ───────
	for (const FPendingTrigger& Trigger : PendingTriggers)
	{
		InvokeTrigger_Internal(Trigger.TriggerName, TEXT(""),
			Trigger.bGenerateActions, Trigger.bReplicateOnNetwork);
	}
	PendingTriggers.Empty();

	// ── Step 4: pending reset LAST so the batch/triggers above drain first ──
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
	if (!PendingContextBatch.HasWork() && PendingTriggers.Num() == 0 && !PendingEnvironmentBatch.HasWork())
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

void UConvaiChatbotComponent::HandleActionCompletion(bool IsSuccessful, float Delay,
	FString EventText, EC_RunLLMOption ShouldRespond)
{
	// Optional outcome event — empty text means the caller doesn't want to push anything
	// to the dynamic context this call.
	if (!EventText.IsEmpty())
	{
		AddContextEvent(EventText, ShouldRespond);
	}

	if (IsSuccessful)
		DequeueAction();

	// (See AbortActionSequence for the "stop the whole queue and let the LLM replan" path.)

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

void UConvaiChatbotComponent::AbortActionSequence(FString EventText, EC_RunLLMOption ShouldRespond)
{
	// Drop the rest of the queue first so any subsequent flush sees a clean slate.
	ClearActionQueue();

	if (!EventText.IsEmpty())
	{
		// Tell the bot what happened so the LLM can plan a fresh sequence on the next turn.
		AddContextEvent(EventText, ShouldRespond);
	}
}

bool UConvaiChatbotComponent::IsActionsQueueEmpty()
{
	return ActionsQueue.Num() == 0;
}

void UConvaiChatbotComponent::ClearActionQueue()
{
	ActionsQueue.Empty();
}

bool UConvaiChatbotComponent::FetchFirstAction(FConvaiResultAction& ConvaiResultAction)
{
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

void UConvaiChatbotComponent::ScheduleOrFlushDynamicContext(bool bFlushImmediately)
{
	if (bFlushImmediately)
	{
		FlushDynamicContext();
	}
	else
	{
		ScheduleContextFlush();
	}
}

namespace
{
	// Same-name replace-or-append helper used by Add{Object,Character} so descriptions/refs
	// update in place without duplicating entries.
	void AddOrReplaceEntry(TArray<FConvaiObjectEntry>& List, const FConvaiObjectEntry& Entry)
	{
		if (Entry.Name.IsEmpty())
		{
			return;
		}
		for (FConvaiObjectEntry& Existing : List)
		{
			if (Existing.Name == Entry.Name)
			{
				Existing.Description = Entry.Description;
				Existing.OptionalPositionVector = Entry.OptionalPositionVector;
				Existing.Ref = Entry.Ref;
				return;
			}
		}
		List.Add(Entry);
	}

	// Returns true if any entry was removed.
	bool RemoveEntryByName(TArray<FConvaiObjectEntry>& List, const FString& Name)
	{
		for (int32 i = 0; i < List.Num(); ++i)
		{
			if (List[i].Name == Name)
			{
				List.RemoveAt(i);
				return true;
			}
		}
		return false;
	}
}

// ── Object mutation ───────────────────────────────────────────────────

void UConvaiChatbotComponent::AddObject(FConvaiObjectEntry Object, bool bFlushImmediately)
{
	if (Object.Name.IsEmpty())
	{
		return;
	}
	AddOrReplaceEntry(EnvironmentData.Objects, Object);
	// Only mark dirty if this entry isn't part of the connect snapshot — otherwise the env-batch
	// would re-send an action_config-bound entry via update-scene-metadata, which is redundant.
	if (!EnvironmentSnapshotAtConnect.Contains(Object.Name))
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::AddObjects(TArray<FConvaiObjectEntry> Objects, bool bFlushImmediately)
{
	bool bAnyDirty = false;
	for (const FConvaiObjectEntry& Entry : Objects)
	{
		if (Entry.Name.IsEmpty())
		{
			continue;
		}
		AddOrReplaceEntry(EnvironmentData.Objects, Entry);
		if (!EnvironmentSnapshotAtConnect.Contains(Entry.Name))
		{
			bAnyDirty = true;
		}
	}
	if (bAnyDirty)
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::RemoveObject(FString ObjectName, bool bFlushImmediately)
{
	if (RemoveEntryByName(EnvironmentData.Objects, ObjectName))
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::RemoveObjects(TArray<FString> ObjectNames, bool bFlushImmediately)
{
	bool bAnyRemoved = false;
	for (const FString& Name : ObjectNames)
	{
		if (RemoveEntryByName(EnvironmentData.Objects, Name))
		{
			bAnyRemoved = true;
		}
	}
	if (bAnyRemoved)
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::ClearObjects(bool bFlushImmediately)
{
	if (EnvironmentData.Objects.Num() == 0)
	{
		return;
	}
	EnvironmentData.Objects.Empty();
	PendingEnvironmentBatch.bSceneMetadataDirty = true;
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

// ── Character mutation ────────────────────────────────────────────────

void UConvaiChatbotComponent::AddCharacter(FConvaiObjectEntry Character, bool bFlushImmediately)
{
	if (Character.Name.IsEmpty())
	{
		return;
	}
	AddOrReplaceEntry(EnvironmentData.Characters, Character);
	if (!EnvironmentSnapshotAtConnect.Contains(Character.Name))
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::AddCharacters(TArray<FConvaiObjectEntry> Characters, bool bFlushImmediately)
{
	bool bAnyDirty = false;
	for (const FConvaiObjectEntry& Entry : Characters)
	{
		if (Entry.Name.IsEmpty())
		{
			continue;
		}
		AddOrReplaceEntry(EnvironmentData.Characters, Entry);
		if (!EnvironmentSnapshotAtConnect.Contains(Entry.Name))
		{
			bAnyDirty = true;
		}
	}
	if (bAnyDirty)
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::RemoveCharacter(FString InCharacterName, bool bFlushImmediately)
{
	if (RemoveEntryByName(EnvironmentData.Characters, InCharacterName))
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::RemoveCharacters(TArray<FString> InCharacterNames, bool bFlushImmediately)
{
	bool bAnyRemoved = false;
	for (const FString& Name : InCharacterNames)
	{
		if (RemoveEntryByName(EnvironmentData.Characters, Name))
		{
			bAnyRemoved = true;
		}
	}
	if (bAnyRemoved)
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::ClearCharacters(bool bFlushImmediately)
{
	if (EnvironmentData.Characters.Num() == 0)
	{
		return;
	}
	EnvironmentData.Characters.Empty();
	PendingEnvironmentBatch.bSceneMetadataDirty = true;
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

// ── Action mutation (local-only — no RTVI delivery channel) ───────────

namespace
{
	int32 IndexOfActionByName(const TArray<FConvaiAction>& List, const FString& Name)
	{
		for (int32 i = 0; i < List.Num(); ++i)
		{
			if (List[i].Name == Name) return i;
		}
		return INDEX_NONE;
	}

	// Same-name replace-or-append for action templates.
	void AddOrReplaceAction(TArray<FConvaiAction>& List, const FConvaiAction& Action)
	{
		if (Action.Name.IsEmpty())
		{
			return;
		}
		const int32 i = IndexOfActionByName(List, Action.Name);
		if (i == INDEX_NONE)
		{
			List.Add(Action);
		}
		else
		{
			List[i] = Action;
		}
	}
}

void UConvaiChatbotComponent::AddAction(FConvaiAction Action)
{
	AddOrReplaceAction(EnvironmentData.Actions, Action);
}

void UConvaiChatbotComponent::AddActions(TArray<FConvaiAction> Actions)
{
	for (const FConvaiAction& A : Actions)
	{
		AddOrReplaceAction(EnvironmentData.Actions, A);
	}
}

void UConvaiChatbotComponent::AddActionByName(FString Name)
{
	if (!Name.IsEmpty())
	{
		AddOrReplaceAction(EnvironmentData.Actions, FConvaiAction(Name));
	}
}

void UConvaiChatbotComponent::RemoveAction(FString Name)
{
	const int32 i = IndexOfActionByName(EnvironmentData.Actions, Name);
	if (i != INDEX_NONE)
	{
		EnvironmentData.Actions.RemoveAt(i);
	}
}

void UConvaiChatbotComponent::RemoveActions(TArray<FString> Names)
{
	for (const FString& N : Names)
	{
		const int32 i = IndexOfActionByName(EnvironmentData.Actions, N);
		if (i != INDEX_NONE)
		{
			EnvironmentData.Actions.RemoveAt(i);
		}
	}
}

void UConvaiChatbotComponent::ClearActions()
{
	EnvironmentData.Actions.Empty();
}

// ── Attention object ──────────────────────────────────────────────────

void UConvaiChatbotComponent::SetObjectInAttention(FConvaiObjectEntry AttentionObject,
	FString Text, EC_RunLLMOption ShouldRespond, bool bFlushImmediately)
{
	if (!bEnableActions)
	{
		// Server resolves attention only when action_config was sent at /connect.
		// Skip the message to save bandwidth.
		CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
			TEXT("SetObjectInAttention skipped: bEnableActions is false on this chatbot."));
		return;
	}

	// 1. Update local mirror — required for next reconnect's action_config.current_attention_object.
	EnvironmentData.CurrentAttentionObject = AttentionObject;

	// 2. Auto-register in EnvironmentData.Objects if missing. Server resolves attention only
	//    against action_config.objects, even if the entry is also in Characters.
	if (!AttentionObject.Name.IsEmpty() && EnvironmentData.FindObject(AttentionObject.Name) == nullptr)
	{
		EnvironmentData.Objects.Add(AttentionObject);
		if (!EnvironmentSnapshotAtConnect.Contains(AttentionObject.Name))
		{
			PendingEnvironmentBatch.bSceneMetadataDirty = true;
		}
	}

	// 3. Stage attention slot + optional text (last-wins within debounce window).
	PendingContextBatch.StageAttention(AttentionObject, Text, ShouldRespond);

	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

// ── Legacy environment shim (DEPRECATED) ─────────────────────────────

UConvaiEnvironment* UConvaiChatbotComponent::GetEnvironment()
{
	// Lazy-init the shim on first BP access. The Environment UPROPERTY is the storage;
	// this keeps the GC happy and survives across calls.
	if (!IsValid(Environment))
	{
		Environment = NewObject<UConvaiEnvironment>(this);
		Environment->OwningChatbot = this;
	}
	return Environment;
}

// ── Conversation partner ──────────────────────────────────────────────

void UConvaiChatbotComponent::SetConversationPartner(FConvaiObjectEntry Partner, bool bFlushImmediately)
{
	ConversationPartner = Partner;

	// Empty name = clear partner without touching the character list.
	if (Partner.Name.IsEmpty())
	{
		return;
	}

	// Mirror the partner into EnvironmentData.Characters so the bot's scene context includes
	// them. Server treats this as a normal character entry — there is no distinct
	// "conversation partner" concept on the server side.
	const bool bWasInCharacters = EnvironmentData.FindCharacter(Partner.Name) != nullptr;
	AddOrReplaceEntry(EnvironmentData.Characters, Partner);

	// Only schedule a scene-metadata send if this is a brand-new character (not already in
	// the connect-time action_config snapshot). Re-describing a connect-time character only
	// takes effect on reconnect.
	if (!bWasInCharacters && !EnvironmentSnapshotAtConnect.Contains(Partner.Name))
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::BeginPlay()
{
	Super::BeginPlay();

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
	EnvironmentSnapshotAtConnect.Reset();
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

	// Snapshot the action_config-bound entries at connect time so the env-batch flush
	// can subtract them out and avoid redundantly re-sending action_config items via
	// update-scene-metadata.
	EnvironmentSnapshotAtConnect.Reset();
	if (bEnableActions)
	{
		for (const FConvaiObjectEntry& O : EnvironmentData.Objects)
		{
			if (!O.Name.IsEmpty()) { EnvironmentSnapshotAtConnect.Add(O.Name); }
		}
		for (const FConvaiObjectEntry& C : EnvironmentData.Characters)
		{
			if (!C.Name.IsEmpty()) { EnvironmentSnapshotAtConnect.Add(C.Name); }
		}
	}

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
	const bool bActionsAlreadyStarted = !IsActionsQueueEmpty();

	// Fill the current queue of actions
	AppendActionsToQueue(ReceivedSequenceOfActions);

	if (!bActionsAlreadyStarted)
		StartFirstAction();

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

	if (bIsInterrupted)
		return;

	if (!IsConnectionTalking)
	{
		double LocalTimestamp;
		{
			FScopeLock Lock(&FinishedTalkingLock);
			LocalTimestamp = FinishedTalkingTimestamp;
		}

		if (LocalTimestamp < 0.0)
		{
			bForceFlushPending = true;
			return;
		}

		const double SilentGap = FPlatformTime::Seconds() - LocalTimestamp;

		if (SilentGap >= StutterGraceWindow)
		{
			{
				FScopeLock Lock(&FinishedTalkingLock);
				FinishedTalkingTimestamp = -1.0;
			}
			bForceFlushPending = true;
			return;
		}
	}

	// Track silence statistics - only count continuous silence > 0.5s
	TotalAudioFramesReceived += NumFrames;

    // Convert the audio data to a format that can be used by the existing methods
	const size_t BytesPerSample = BitsPerSample / 8;
	const size_t TotalBytes = NumFrames * NumChannels * BytesPerSample;
    if (TotalBytes > 0)
    {
        // Directly call HandleAudioReceived
        HandleAudioReceived((uint8*)AudioData, TotalBytes, false, SampleRate, NumChannels);
    }
}

void UConvaiChatbotComponent::OnStartedTalking()
{
	IsConnectionTalking = true;
	FScopeLock Lock(&FinishedTalkingLock);
	FinishedTalkingTimestamp = -1.0;
}

void UConvaiChatbotComponent::OnFinishedTalking()
{
	IsConnectionTalking = false;
	FScopeLock Lock(&FinishedTalkingLock);
	FinishedTalkingTimestamp = FPlatformTime::Seconds();
}

void UConvaiChatbotComponent::OnLLMStarted()
{
	MarkEndOfAudio();
	StopVoice();
	FScopeLock Lock(&FinishedTalkingLock);
	FinishedTalkingTimestamp = -1.0;
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
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("onAudioFinished - IsConnectionTalking: %s"),
		IsConnectionTalking ? TEXT("true") : TEXT("false"));

	bIsPlayingAudio = false;

	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf] {
		if (WeakSelf.IsValid())
		{
			WeakSelf->OnFinishedTalkingDelegate.Broadcast();
		}
	});
	IsTalking = false;

	PauseLipSync();
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

	// Auto-fill conversation partner before the BP virtual fires so designers can inspect
	// or override the auto-filled value from GatherEnvironmentExtras.
	if (bAutoFillConversationPartnerFromPlayer)
	{
		FConvaiObjectEntry Partner;
		if (UConvaiPlayerComponent* PC = UConvaiUtils::GetFirstConvaiPlayerComponent(this))
		{
			Partner.Name = PC->PlayerName;
			Partner.Ref = PC->GetOwner();
		}
		else if (APawn* Pawn = UGameplayStatics::GetPlayerPawn(this, 0))
		{
			Partner.Name = TEXT("User");
			Partner.Ref = Pawn;
		}
		if (!Partner.Name.IsEmpty())
		{
			SetConversationPartner(Partner);
		}
	}

	// Let BPs append dynamic affordances right before the connect payload is built.
	// Edit-time defaults from the Environment property are still sent — this only appends.
	{
		TArray<FConvaiAction> ExtraActions;
		TArray<FConvaiObjectEntry> ExtraObjects;
		TArray<FConvaiObjectEntry> ExtraCharacters;
		GatherEnvironmentExtras(ExtraActions, ExtraObjects, ExtraCharacters);
		for (const FConvaiAction& A : ExtraActions) { AddOrReplaceAction(EnvironmentData.Actions, A); }
		for (const FConvaiObjectEntry& O : ExtraObjects) { AddOrReplaceEntry(EnvironmentData.Objects, O); }
		for (const FConvaiObjectEntry& C : ExtraCharacters) { AddOrReplaceEntry(EnvironmentData.Characters, C); }
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
