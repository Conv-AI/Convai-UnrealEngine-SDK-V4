// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiChatbotComponent.h"
#include "Debug/ConvaiDebugSubsystem.h"
#include "Environment/ConvaiEnvironmentLegacy.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiObjectComponent.h"
#include "../Convai.h"
#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "LipSyncInterface.h"
#include "VisionInterface.h"
#include "ConvaiSubsystem.h"
#include "ConvaiContextSubsystem.h"
#include "ConvaiConnectionSessionProxy.h"
#include "ConvaiMergedObjectNavigation.h"
#include "Utility/ConvaiContextFormat.h"

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

namespace
{
	// Built-in (experimental) action identifiers — referenced from the queue
	// bookkeeping near the top of this file and the template builders further
	// down, so they live here rather than in either local namespace block.
	const TCHAR* const RemindSelfActionName    = TEXT("Remind Self");
	const TCHAR* const RemindSelfParamName     = TEXT("reminder");
	const TCHAR* const WatchPropertyActionName = TEXT("Watch Property");
	const TCHAR* const WatchPropertyParamName  = TEXT("property");
	const TCHAR* const CancelActionPlanActionName = TEXT("Cancel Action Plan");
	const TCHAR* const CancelHandlerPrefix = TEXT("Cancel ");
	constexpr float CancellationDiagnosticDelaySeconds = 10.0f;

	// Stable, hidden bookkeeping keys for the session-start usage hints. Only
	// the natural-language facts are rendered into dynamic context.
	const TCHAR* const RemindSelfContextFactKey = TEXT("BuiltInAction:RemindSelf");
	const TCHAR* const WatchPropertyContextFactKey = TEXT("BuiltInAction:WatchProperty");
	const TCHAR* const RemindSelfContextFact = TEXT("To continue after an action, follow it with Remind Self and what to do or say next.");
	const TCHAR* const WatchPropertyContextFact = TEXT("To react once to a tracked property's next change, use Watch Property with its exact context key; add ' = exact value' to wait until a later change reaches it.");
}

UConvaiChatbotComponent::UConvaiChatbotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 1 / 15;
	InterruptVoiceFadeOutDuration = 1.0;
	bAutoInitializeSession = true;

	// Project-wide defaults sourced via the custom-param chain
	// (hardcoded -> UConvaiSettings::CustomPrams -> command-line override).
	// Per-instance UPROPERTY overrides these on already-serialized instances.
	ContextDebounceWindow    = UConvaiUtils::GetContextDebounceWindowDefault();
	ContextMaxDebounceWindow = UConvaiUtils::GetContextMaxDebounceWindowDefault();
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
	// bEnableActions moved into FConvaiEnvironmentData (replicates with the parent struct).
	DOREPLIFETIME(UConvaiChatbotComponent, ConversationPartner);
	DOREPLIFETIME(UConvaiChatbotComponent, LookAtTarget);
	DOREPLIFETIME(UConvaiChatbotComponent, PointAtTarget);
}

bool UConvaiChatbotComponent::IsInConversation()
{
	// IsConnectionTalking is included alongside GetIsTalking() because the server's
	// bot-started-speaking signal (which closes the thinking window) leads local
	// playback by the buffering latency — without it there'd be a false-idle gap
	// between "thinking ended" and "audio actually playing".
	return IsProcessing() || IsListening() || IsConnectionTalking || GetIsTalking()
		|| AudioRingBuffer.GetAvailableBytes() > 0;
}

bool UConvaiChatbotComponent::IsProcessing()
{
	FScopeLock Lock(&ThinkingLock);
	return ThinkingStartTimestamp >= 0.0 &&
		(FPlatformTime::Seconds() - ThinkingStartTimestamp) <= ThinkingStalenessSeconds;
}

bool UConvaiChatbotComponent::IsListening()
{
	// The server's VAD brackets the user-speaking interval as OnInterrupt /
	// OnInterruptEnd on this component — bIsInterrupted IS that interval.
	return bIsInterrupted;
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
	const double Now = FPlatformTime::Seconds();
	const double Base = FMath::Max(0.1, (double)ContextDebounceWindow);
	const double MaxWindow = FMath::Max(Base, (double)ContextMaxDebounceWindow);

	// Idle: open a fresh debounce window. Burst: keep the existing window start
	// so the max-wait cap measures from the FIRST update of the burst (not the
	// most recent), which is what gives designers a predictable worst-case
	// "no update sits longer than MaxWindow before flushing" guarantee.
	if (DebounceWindowStartTime < 0.0)
	{
		DebounceWindowStartTime = Now;
	}

	// Standard debounce: reset the timer to now + Base.
	// Safety cap: never push the flush past WindowStart + MaxWindow.
	const double Deadline = DebounceWindowStartTime + MaxWindow;
	NextContextFlushTime  = FMath::Min(Now + Base, Deadline);
}


void UConvaiChatbotComponent::SetContextState(const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery, bool bFlushImmediately)
{
	SetContextStateInternal(Name, Value, ShouldRespond, Delivery,
		bFlushImmediately, /*bOmitPreviousValue*/ false);
}

void UConvaiChatbotComponent::SetContextStateInternal(const FString& Name,
	const FString& Value, EC_RunLLMOption ShouldRespond,
	EConvaiContextDelivery Delivery, bool bFlushImmediately,
	bool bOmitPreviousValue, bool bEvaluateWatches,
	const FString* PreviousValueOverride, bool bPreserveTransition)
{
	// Keys are sanitized (whitespace Pascal-cased away) at every state API choke
	// point — set / remove / get agree on the sanitized form, and the LLM never
	// sees a key that splits into multiple prompt tokens.
	const FString Key = ConvaiContextFormat::SanitizeKey(Name);

	// One-shot watch armed by the "Watch Property" action: a GENUINE change to a
	// watched key (established canonical value, different new value) responds
	// Always even when the property itself is Never/Auto. Delivery still follows
	// the tracked property's canonical settings; Never properties are therefore
	// normal-delivery and non-flushing. A first observation/seed or a same-value
	// re-set does not satisfy "next change".
	bool bWatched = bEvaluateWatches
		&& WouldWatchContextStateChange(Key, Value);
	if (!bWatched && bEvaluateWatches && PreviousValueOverride)
	{
		if (const FString* WatchTarget = WatchedContextKeys.Find(Key))
		{
			// A held/coalesced excursion may return to the unchanged canonical
			// value. In that case the explicit transition history, not canonical
			// A -> A, is the genuine edge the watch should evaluate.
			bWatched = ConvaiContextFormat::WouldWatchMatch(*WatchTarget,
				*PreviousValueOverride, Value,
				TargetWatchesWithObservedDeparture.Contains(Key));
		}
	}
	FString ExistingValue;
	const bool bHasExistingValue =
		DynamicContextTracker.GetStateValue(Key, ExistingValue);
	FString WatchedPreviousValue;
	FString HeldPreviousValue;
	const FString* EffectivePreviousValue = PreviousValueOverride;
	if (!EffectivePreviousValue && bWatched && bHasExistingValue
		&& ExistingValue != Value)
	{
		// A matching watch is itself transition-significant. Preserve the value
		// immediately before it even if the whole batch later returns to its start.
		WatchedPreviousValue = ExistingValue;
		EffectivePreviousValue = &WatchedPreviousValue;
	}
	const bool bHeldTransition =
		HeldContext.StatePreserveTransitions.Contains(Key);
	if (!EffectivePreviousValue && (bWatched || bPreserveTransition
		|| bHeldTransition))
	{
		if (const FString* HeldValue = HeldContext.StateValues.Find(Key);
			HeldValue && *HeldValue != Value)
		{
			HeldPreviousValue = *HeldValue;
			EffectivePreviousValue = &HeldPreviousValue;
		}
	}
	bool bKeepTransition = bPreserveTransition || bWatched
		|| bHeldTransition
		|| (EffectivePreviousValue != nullptr);
	// A targeted watch may deliberately match a return to an unchanged canonical
	// value after observing a held non-target departure. Without an explicit
	// transition history, "now Closed (was Closed)" would be contradictory, so
	// keep that special case present-only. A producer-supplied previous value is
	// authoritative and remains visible.
	if (bWatched && TargetWatchesWithObservedDeparture.Contains(Key))
	{
		if (!EffectivePreviousValue && bHasExistingValue
			&& ExistingValue == Value)
		{
			bOmitPreviousValue = true;
		}
	}
	if (bEvaluateWatches)
	{
		if (const FString* WatchTarget = WatchedContextKeys.Find(Key))
		{
			FString CurrentValue;
			if (DynamicContextTracker.GetStateValue(Key, CurrentValue)
				&& ConvaiContextFormat::IsTargetWatchDeparture(
					*WatchTarget, CurrentValue, Value))
			{
				// Preserve a real departure even if idle delivery parks this value and
				// it never becomes canonical before the object returns to the target.
				TargetWatchesWithObservedDeparture.Add(Key);
			}
		}
	}

	// The hold decision must see the pending upgrade (a watched change on a
	// Never property still needs to wait for the conversation), but the LANE
	// stores the property's ORIGINAL respond: release re-enters here and
	// re-evaluates against the still-untouched canonical value. Targeted watches
	// separately remember an observed non-target departure, so a held
	// Stopped → Moving → Stopped sequence can still satisfy "wait for Stopped".
	const bool bPendingWatchPromotion =
		PendingContextBatch.WatchPromotedStateKeys.Contains(Key);
	const EC_RunLLMOption HoldRespond = (bWatched || bPendingWatchPromotion)
		? EC_RunLLMOption::Always : ShouldRespond;
	const bool bContinuePreservedHold = bHeldTransition
		&& Delivery == EConvaiContextDelivery::WaitUntilConversationIsIdle;
	if (ShouldHoldForConversation(HoldRespond, Delivery)
		|| bContinuePreservedHold)
	{
		// An older normal-delivery write may still be waiting in the debounce
		// batch. It must not flush after this newer same-key value has been
		// routed to the idle lane. Restore the pre-batch canonical baseline
		// before parking the newer value.
		bool bInheritedWatchPromotion = false;
		PendingContextBatch.WithdrawStateForHold(
			DynamicContextTracker, Key, &bInheritedWatchPromotion);
		if (bInheritedWatchPromotion)
		{
			// The one-shot watch was consumed when the older value entered this
			// batch. Carry its explicit Always promise to the fresher held value
			// rather than silently losing it during withdrawal.
			ShouldRespond = EC_RunLLMOption::Always;
			bKeepTransition = true;
			if (!EffectivePreviousValue && bHasExistingValue
				&& ExistingValue != Value)
			{
				HeldPreviousValue = ExistingValue;
				EffectivePreviousValue = &HeldPreviousValue;
			}
		}

		FString CurrentCanonicalValue;
		const bool bHasCurrentCanonicalValue =
			DynamicContextTracker.GetStateValue(Key, CurrentCanonicalValue);
		if (!bKeepTransition && bHasCurrentCanonicalValue
			&& CurrentCanonicalValue == Value)
		{
			// The held value returned to the still-canonical start before anything
			// was delivered. Ordinary state churn therefore disappears completely.
			HeldContext.DropStateKey(Key);
			return;
		}
		HeldContext.HoldState(Key, Value, ShouldRespond, bOmitPreviousValue,
			EffectivePreviousValue, bKeepTransition,
			bInheritedWatchPromotion);
		HeldContext.bReleaseAsSoonAsIdle |= bFlushImmediately;
		return;
	}

	// Consume the watch and apply the upgrade only at ACTUAL staging.
	if (bWatched)
	{
		WatchedContextKeys.Remove(Key);
		TargetWatchesWithObservedDeparture.Remove(Key);
		ShouldRespond = EC_RunLLMOption::Always;
	}

	// A normal write supersedes an older held write for the same key — releasing
	// the held one later would roll the value backwards.
	bool bInheritedHeldWatchPromotion = false;
	HeldContext.DropStateKey(Key, &bInheritedHeldWatchPromotion);
	if (bInheritedHeldWatchPromotion)
	{
		ShouldRespond = EC_RunLLMOption::Always;
	}
	const bool bStaged = PendingContextBatch.StageState(DynamicContextTracker, Key, Value,
		ShouldRespond, bOmitPreviousValue, EffectivePreviousValue,
		bKeepTransition);
	if (!bStaged)
	{
		return;
	}
	if (bWatched || bInheritedHeldWatchPromotion)
	{
		PendingContextBatch.WatchPromotedStateKeys.Add(Key);
	}
	ScheduleOrFlushDynamicContext(bFlushImmediately);

	if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
	{
		Dbg->OnStateDebug.Broadcast(this, Key, Value, ShouldRespond, /*bRemoved*/ false);
	}
}

void UConvaiChatbotComponent::SetContextStates(const TMap<FString, FString>& States, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery, bool bFlushImmediately)
{
	if (States.Num() == 0)
	{
		return;
	}
	if (ShouldHoldForConversation(ShouldRespond, Delivery))
	{
		bool bHeldAny = false;
		for (const auto& Pair : States)
		{
			const FString Key = ConvaiContextFormat::SanitizeKey(Pair.Key);
			const bool bHeldTransition =
				HeldContext.StatePreserveTransitions.Contains(Key);
			FString SupersededPendingValue;
			const bool bHadSupersededPendingValue =
				DynamicContextTracker.GetStateValue(Key, SupersededPendingValue);
			bool bInheritedWatchPromotion = false;
			PendingContextBatch.WithdrawStateForHold(
				DynamicContextTracker, Key, &bInheritedWatchPromotion);
			const bool bKeepTransition =
				bHeldTransition || bInheritedWatchPromotion;
			FString ExistingValue;
			if (!bKeepTransition
				&& DynamicContextTracker.GetStateValue(Key, ExistingValue)
				&& ExistingValue == Pair.Value)
			{
				HeldContext.DropStateKey(Key);
				continue;
			}
			const FString* PreviousValueOverride =
				bInheritedWatchPromotion && bHadSupersededPendingValue
					&& SupersededPendingValue != Pair.Value
				? &SupersededPendingValue : nullptr;
			HeldContext.HoldState(Key, Pair.Value,
				bInheritedWatchPromotion
					? EC_RunLLMOption::Always : ShouldRespond,
				/*bOmitPreviousValue*/ false, PreviousValueOverride,
				bKeepTransition, bInheritedWatchPromotion);
			bHeldAny = true;
		}
		if (bHeldAny)
		{
			HeldContext.bReleaseAsSoonAsIdle |= bFlushImmediately;
		}
		return;
	}
	bool bStagedAny = false;
	for (const auto& Pair : States)
	{
		const FString Key = ConvaiContextFormat::SanitizeKey(Pair.Key);
		FString SupersededHeldValue;
		const FString* HeldValue = HeldContext.StateValues.Find(Key);
		const bool bHadSupersededHeldValue = HeldValue != nullptr;
		if (HeldValue)
		{
			SupersededHeldValue = *HeldValue;
		}
		bool bInheritedWatchPromotion = false;
		HeldContext.DropStateKey(Key, &bInheritedWatchPromotion);
		const FString* PreviousValueOverride =
			bInheritedWatchPromotion && bHadSupersededHeldValue
				&& SupersededHeldValue != Pair.Value
			? &SupersededHeldValue : nullptr;
		const bool bStaged = PendingContextBatch.StageState(
			DynamicContextTracker, Key, Pair.Value,
			bInheritedWatchPromotion
				? EC_RunLLMOption::Always : ShouldRespond,
			/*bOmitPreviousValue*/ false, PreviousValueOverride,
			/*bPreserveTransition*/ bInheritedWatchPromotion);
		if (bStaged && bInheritedWatchPromotion)
		{
			PendingContextBatch.WatchPromotedStateKeys.Add(Key);
		}
		bStagedAny |= bStaged;
	}
	if (bStagedAny)
	{
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}
}

void UConvaiChatbotComponent::AddContextEvent(const FString& Text, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery, bool bEphemeral, bool bFlushImmediately)
{
	if (ShouldHoldForConversation(ShouldRespond, Delivery))
	{
		HeldContext.HoldEvent(Text, ShouldRespond, bEphemeral);
		HeldContext.bReleaseAsSoonAsIdle |= bFlushImmediately;
		return;
	}
	// A normal event supersedes an identical held one (same text + ephemerality)
	// — otherwise the tracker would receive the same persistent event twice once
	// the held lane releases.
	HeldContext.DropEvent(Text, bEphemeral);
	PendingContextBatch.StageEvent(Text, ShouldRespond, bEphemeral);
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

void UConvaiChatbotComponent::WithdrawPendingContextEvent(
	const FString& Text,
	bool bEphemeral)
{
	check(IsInGameThread());
	HeldContext.DropEvent(Text, bEphemeral);
	PendingContextBatch.DropEvent(Text, bEphemeral);
}

void UConvaiChatbotComponent::RemoveContextState(const FString& Name, bool bFlushImmediately)
{
	const FString Key = ConvaiContextFormat::SanitizeKey(Name);
	TargetWatchesWithObservedDeparture.Remove(Key);
	// Drop any staged or held set for this key so neither the upcoming flush nor
	// a later held-lane release can resurrect it.
	HeldContext.DropStateKey(Key);
	PendingContextBatch.DropStateKey(Key);
	if (!DynamicContextTracker.RemoveState(Key))
	{
		return;
	}

	// Batch like the other updates — rapid removes coalesce into one
	// canonical Replace on flush.
	PendingContextBatch.bForceReplace = true;
	ScheduleOrFlushDynamicContext(bFlushImmediately);

	if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
	{
		Dbg->OnStateDebug.Broadcast(this, Key, FString(), EC_RunLLMOption::Never, /*bRemoved*/ true);
	}
}

void UConvaiChatbotComponent::SetContextFact(const FString& Sentence, const FString& Key, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery, bool bFlushImmediately)
{
	// An empty Key means the caller wants a simple one-off fact: use the sentence
	// itself as the bookkeeping handle so identical facts dedup and the same
	// sentence can be removed later. Callers that pass an explicit Key get
	// in-place updates as the wording changes.
	const FString& EffectiveKey = Key.IsEmpty() ? Sentence : Key;

	if (ShouldHoldForConversation(ShouldRespond, Delivery))
	{
		PendingContextBatch.WithdrawDeclarativeForHold(
			DynamicContextTracker, EffectiveKey);
		HeldContext.HoldDeclarative(EffectiveKey, Sentence, ShouldRespond);
		HeldContext.bReleaseAsSoonAsIdle |= bFlushImmediately;
		return;
	}

	// A normal write supersedes an older held write for the same key.
	HeldContext.DropDeclarativeKey(EffectiveKey);
	PendingContextBatch.StageDeclarative(DynamicContextTracker, EffectiveKey, Sentence, ShouldRespond);
	ScheduleOrFlushDynamicContext(bFlushImmediately);
}

void UConvaiChatbotComponent::RemoveContextFact(const FString& KeyOrSentence, bool bFlushImmediately)
{
	// Drop any staged or held set for this key so neither the upcoming flush nor
	// a later held-lane release can resurrect it.
	HeldContext.DropDeclarativeKey(KeyOrSentence);
	PendingContextBatch.DropDeclarativeKey(KeyOrSentence);
	if (!DynamicContextTracker.RemoveDeclarative(KeyOrSentence))
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
	// Armed watches belonged to the pre-reset conversation. Clear them before
	// the spatial subsystem silently re-seeds synthetic Movement, otherwise a
	// changed baseline value could consume a watch and upgrade the reset batch.
	WatchedContextKeys.Empty();
	TargetWatchesWithObservedDeparture.Empty();

	if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UConvaiContextSubsystem* ContextSubsystem =
			GameInstance->GetSubsystem<UConvaiContextSubsystem>())
		{
			ContextSubsystem->ResetObserverSpatialContext(this);
		}
	}

	// Reset is terminal for queued triggers — they belonged to the pre-reset
	// conversation and shouldn't carry over.
	PendingTriggers.Empty();

	// Held (conversation-idle) work is CANCELLED, not forced out: it never
	// entered the canonical state, and triggering an LLM response from deferred
	// data only to immediately reset the remote context would be incoherent.
	// (Normal staged work below still drains first — it already mutated the
	// tracker, so it gets delivered before the reset.)
	HeldContext.Clear();

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
	return DynamicContextTracker.GetStateValue(ConvaiContextFormat::SanitizeKey(Name), OutValue);
}

bool UConvaiChatbotComponent::WouldWatchContextStateChange(
	const FString& Name, const FString& Value,
	const FString* PreviousValueOverride) const
{
	const FString Key = ConvaiContextFormat::SanitizeKey(Name);
	const FString* TargetValue = WatchedContextKeys.Find(Key);
	if (!TargetValue)
	{
		return false;
	}
	if (PreviousValueOverride)
	{
		return ConvaiContextFormat::WouldWatchMatch(*TargetValue,
			*PreviousValueOverride, Value,
			TargetWatchesWithObservedDeparture.Contains(Key));
	}
	FString ExistingValue;
	if (!DynamicContextTracker.GetStateValue(Key, ExistingValue))
	{
		return false;
	}
	return ConvaiContextFormat::WouldWatchMatch(*TargetValue, ExistingValue, Value,
		TargetWatchesWithObservedDeparture.Contains(Key));
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
		NextContextFlushTime    = -1.0;
		DebounceWindowStartTime = -1.0;
		return;
	}

	// ── Connection guard ─────────────────────────────────────────────
	// A force-flush (bFlushImmediately == true) can land while the session is down.
	// Sending now would no-op — UpdateContext drops the payload when the proxy isn't
	// connected — yet ClearStaged() below would still wipe the staged batch. For
	// persisted state/events that's merely wasteful (they're rebuilt from the tracker
	// on the next flush), but EPHEMERAL one-flush events are NEVER committed to the
	// tracker, so clearing them here would lose them permanently ("exactly once"
	// becomes "zero times"). Defer to the scheduled path — exactly what
	// TickDynamicContext does while disconnected — so the work survives and goes out
	// on the first flush after reconnect.
	if (!IsChatbotConnected())
	{
		ScheduleContextFlush();
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
			// A response requested by another coalesced item must not pull this
			// silent state into the high-attention delta tail. Never states stay
			// current in canonical context only.
			if (!PendingContextBatch.StateRequestsResponse(Key))
			{
				continue;
			}
			FString CurrentValue;
			DynamicContextTracker.GetStateValue(Key, CurrentValue);
			const FString* OldValue = PendingContextBatch.OldValues.Find(Key);
			if (OldValue)
			{
				// Lead with the CURRENT value — the delta block sits at the prompt
				// tail where the model attends hardest, and a stale value must
				// never be the last thing it reads there. The prior value rides
				// along parenthetically, and only when it's short (≤ 3 words): a
				// wordy stale value invites the model to attend to it as current.
				//
				// Manual early-exit counter — we only care whether the threshold
				// is exceeded, so there's no point allocating a TArray<FString>.
				int32 OldValueWords = 0;
				bool bInWord = false;
				for (const TCHAR Ch : *OldValue)
				{
					if (FChar::IsWhitespace(Ch))
					{
						bInWord = false;
					}
					else if (!bInWord)
					{
						bInWord = true;
						if (++OldValueWords > 3)
						{
							break;
						}
					}
				}

				// Values render through the same quoting rule as the canonical
				// rows, so a multi-word value reads identically in both places.
				if (PendingContextBatch.OmitPreviousValueKeys.Contains(Key)
					|| OldValueWords > 3)
				{
					DeltaLines.Add(FString::Printf(TEXT("%s is now %s"), *Key,
						*ConvaiContextFormat::FormatValueForPrompt(CurrentValue)));
				}
				else
				{
					DeltaLines.Add(FString::Printf(TEXT("%s is now %s (was %s)"), *Key,
						*ConvaiContextFormat::FormatValueForPrompt(CurrentValue),
						*ConvaiContextFormat::FormatValueForPrompt(*OldValue)));
				}
			}
			else
			{
				DeltaLines.Add(FString::Printf(TEXT("%s is %s"), *Key,
					*ConvaiContextFormat::FormatValueForPrompt(CurrentValue)));
			}
		}

		// Declarative facts get NO delta lines: they are persistent world facts
		// that read best grouped in their canonical slot (states → facts →
		// events). Emitting the changed sentence at the prompt tail — after a
		// long events list — made it read disconnected from its group.
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

		// Canonical vs. delta placement of brand-new state keys depends on each
		// state's own ShouldRespond. A response requested by a different state,
		// fact, event, or attention cue must not hide a silent key from canonical
		// or repeat it at the high-attention delta tail.
		//
		//   • This key > Never — defer its first appearance from canonical so it
		//     appears only in its delta line at the prompt tail on this flush.
		//   • This key == Never — keep it in canonical and emit no delta line.
		//
		// Changed-value keys stay in canonical regardless: their "is now Y (was X)"
		// delta line is dropped on the next flush, so canonical must keep the key.
		TSet<FString> FirstAppearanceKeys;
		if (PendingContextBatch.AggregateRunLLM != EC_RunLLMOption::Never)
		{
			FirstAppearanceKeys.Reserve(PendingContextBatch.StagedStateOrder.Num());
			for (const FString& Key : PendingContextBatch.StagedStateOrder)
			{
				if (PendingContextBatch.StateRequestsResponse(Key)
					&& !PendingContextBatch.InitialValues.Contains(Key))
				{
					FirstAppearanceKeys.Add(Key);
				}
			}
		}

		const FString Canonical = BuildCanonicalContextWithManagedActions(FirstAppearanceKeys);
		const bool bIncludeDeltas =
			PendingContextBatch.AggregateRunLLM != EC_RunLLMOption::Never &&
			DeltaLines.Num() > 0;
		FString Payload = bIncludeDeltas
			? Canonical + TEXT("\n") + FString::Join(DeltaLines, TEXT("\n"))
			: Canonical;

		// Ephemeral (one-flush) events: append to the OUTGOING text only. They are
		// deliberately NOT committed to the tracker (no AddEvent), so the NEXT flush —
		// a Replace built from canonical without these lines — makes them disappear, as
		// if never added. Appended unconditionally (even when AggregateRunLLM == Never,
		// where no delta block is emitted) so "one flush" is reliable. ClearStaged drops them.
		if (PendingContextBatch.EphemeralEventsOrdered.Num() > 0)
		{
			const FString EphemeralBlock = FString::Join(PendingContextBatch.EphemeralEventsOrdered, TEXT("\n"));
			Payload = Payload.IsEmpty() ? EphemeralBlock : Payload + TEXT("\n") + EphemeralBlock;
		}

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
		InvokeTrigger_Internal(Trigger.TriggerName, TEXT(""));
	}
	PendingTriggers.Empty();

	// ── Step 4: pending reset LAST so the batch/triggers above drain first ──
	if (bHasReset)
	{
		UpdateContext(TEXT(""), EC_ContextUpdateMode::Reset, EC_RunLLMOption::Never);
		DynamicContextTracker.Reset();
		PendingContextBatch.bPendingReset = false;

		// Reset clears user-authored world context, not the session contract. Put
		// the derived built-in instructions (and any still-live action status)
		// straight back so they cannot disappear until another queue transition.
		const FString PostResetContext = BuildCanonicalContextWithManagedActions();
		if (!PostResetContext.IsEmpty())
		{
			UpdateContext(PostResetContext, EC_ContextUpdateMode::Replace,
				EC_RunLLMOption::Never);
		}
	}

	NextContextFlushTime    = -1.0;
	DebounceWindowStartTime = -1.0;
}

void UConvaiChatbotComponent::TickDynamicContext()
{
	// Held-lane release is evaluated BEFORE the flush-timer early-return below:
	// held-only work has no NextContextFlushTime armed yet, and would otherwise
	// never wake up. The lane's idle clock counts only connected, genuinely
	// quiet time — any activity or disconnect restarts it — and held work
	// releases once the quiet stretch reaches Quiet Time Before Delivery
	// (immediately at first idle for a held bFlushImmediately). There is no
	// forced release: an update asked to wait for a pause waits for a pause.
	if (HeldContext.UpdateIdleAndCheckRelease(!IsInConversation(), IsChatbotConnected(),
		FPlatformTime::Seconds(), (double)ConversationIdleSettleSeconds))
	{
		ReleaseHeldContext();
	}

	if (NextContextFlushTime < 0.0)
	{
		return;
	}
	if (!PendingContextBatch.HasWork() && PendingTriggers.Num() == 0 && !PendingEnvironmentBatch.HasWork())
	{
		NextContextFlushTime    = -1.0;
		DebounceWindowStartTime = -1.0;
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
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
		const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
		AsyncTask(ENamedThreads::GameThread,
			[WeakSelf, ExpectedEpoch, NewActions = MoveTemp(NewActions)]() mutable
		{
			if (UConvaiChatbotComponent* Self = WeakSelf.Get();
				Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
			{
				Self->AppendActionsToQueue(MoveTemp(NewActions));
			}
		});
		return;
	}

	if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
	{
		for (const FConvaiResultAction& A : NewActions)
		{
			Dbg->OnActionDebug.Broadcast(this,
				A.ActionString.IsEmpty() ? A.Action : A.ActionString,
				EConvaiActionDebugPhase::Received, FString());
		}
	}

	if (ActionExecutionState == EConvaiActionExecutionState::Cancelling)
	{
		// While the old head is quiescing, the newest plan wins. It must remain
		// outside ActionsQueue so a late completion cannot consume it by mistake.
		PendingReplacementActions = MoveTemp(NewActions);
		MarkManagedActionContextDirty();
		return;
	}

	if (ActionExecutionState == EConvaiActionExecutionState::Running && ActionsQueue.Num() > 0)
	{
		// A new plan replaces the queued tail (element 0 keeps executing). Queued
		// built-in reminders/watches are silently superseded by design — but log
		// it, or "the character never spoke after arriving" is undebuggable.
		for (int32 i = 1; i < ActionsQueue.Num(); ++i)
		{
			const FString& Dropped = ActionsQueue[i].Action;
			if (Dropped.Equals(RemindSelfActionName, ESearchCase::IgnoreCase) ||
				Dropped.Equals(WatchPropertyActionName, ESearchCase::IgnoreCase))
			{
				CONVAI_LOG(ConvaiChatbotComponentLog, Log,
					TEXT("AppendActionsToQueue: new action sequence dropped queued '%s' (%s)"),
					*Dropped, *ActionsQueue[i].ActionString);
			}
		}

		FConvaiResultAction FirstAction = ActionsQueue[0];
		NewActions.Insert(FirstAction, 0);
		ActionsQueue = NewActions;
	}
	else
	{
		ActionsQueue = NewActions;
	}

	if (ActionExecutionState != EConvaiActionExecutionState::Running)
	{
		ActionExecutionState = ActionsQueue.Num() == 0
			? EConvaiActionExecutionState::Idle
			: EConvaiActionExecutionState::DispatchPending;
	}
	MarkManagedActionContextDirty();
}

void UConvaiChatbotComponent::HandleActionCompletion(bool IsSuccessful, bool bAutoReport,
	EC_RunLLMOption ShouldRespond, FString AdditionalNote, float Delay,
	EConvaiContextDelivery Delivery, bool bEphemeral, bool bFlushImmediately)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
		const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
		AsyncTask(ENamedThreads::GameThread,
			[WeakSelf, ExpectedEpoch, IsSuccessful, bAutoReport, ShouldRespond,
			 AdditionalNote = MoveTemp(AdditionalNote), Delay, Delivery, bEphemeral,
			 bFlushImmediately]() mutable
			{
				if (UConvaiChatbotComponent* Self = WeakSelf.Get();
					Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
				{
					Self->HandleActionCompletion(IsSuccessful, bAutoReport, ShouldRespond,
						MoveTemp(AdditionalNote), Delay, Delivery, bEphemeral,
						bFlushImmediately);
				}
			});
		return;
	}
	if (bActionLifecycleShuttingDown)
	{
		return;
	}
	if (ActionExecutionState != EConvaiActionExecutionState::Running &&
		ActionExecutionState != EConvaiActionExecutionState::Cancelling)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
			TEXT("HandleActionCompletion ignored because no dispatched action is active."));
		return;
	}

	const bool bCompletingCancellation =
		ActionExecutionState == EConvaiActionExecutionState::Cancelling;
	FConvaiResultAction CurrentAction;
	const bool bHasCurrentAction = FetchFirstAction(CurrentAction);
	const FString ActionLabel = bHasCurrentAction
		? (CurrentAction.ActionString.IsEmpty() ? CurrentAction.Action : CurrentAction.ActionString)
		: FString();

	if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
	{
		Dbg->OnActionDebug.Broadcast(this,
			ActionLabel,
			bCompletingCancellation && !IsSuccessful
				? EConvaiActionDebugPhase::Aborted
				: (IsSuccessful ? EConvaiActionDebugPhase::Succeeded : EConvaiActionDebugPhase::Failed),
			AdditionalNote);
	}

	// ── Synthesise the auto-report "base" message if requested. Stays empty when
	//    bAutoReport is false, when the queue has no head, or when the action
	//    label resolves to empty.
	FString ReportBase;
	if (bAutoReport)
	{
		// Prefer the raw ActionString (natural-language form) so the LLM sees
		// the same phrasing it produced; fall back to the canonical Action name.
		if (bHasCurrentAction)
		{
			if (!ActionLabel.IsEmpty())
			{
				if (IsSuccessful)
				{
					ReportBase = FString::Printf(
						TEXT("you were able to %s successfully"), *ActionLabel);
				}
				else
				{
					// Resolve a partner name to address. GetFirstConvaiPlayerComponent
					// is the convention used elsewhere in this file (cf. Partner.Name
					// resolution in StartSession). Fall back to "the user" when no
					// player component is registered or the name is unset.
					//
					// The "try something else or consult with X" nudge steers smaller
					// LLMs away from blindly retrying the same plan.
					FString PartnerName = TEXT("the user");
					AActor* PartnerOwner = nullptr;
					if (UConvaiPlayerComponent* PC = UConvaiUtils::GetFirstConvaiPlayerComponent(this, /*bOnlyConnected*/ false, PartnerOwner))
					{
						if (!PC->PlayerName.IsEmpty())
						{
							PartnerName = PC->PlayerName;
						}
					}

					ReportBase = FString::Printf(
						TEXT("failed to %s, try something else or consult with %s"),
						*ActionLabel, *PartnerName);
				}
			}
		}
	}

	if (bCompletingCancellation)
	{
		// A failure caused by cooperative cancellation is not a new world-state
		// fact and must not encourage a retry. A genuine success can carry a real
		// state change, so preserve it as silent context. Explicit notes are kept
		// in both cases; generated failure prose is deliberately discarded.
		FString CancellationText;
		if (IsSuccessful)
		{
			FString Preserved;
			if (!ReportBase.IsEmpty() && !AdditionalNote.IsEmpty())
			{
				Preserved = FString::Printf(TEXT("%s, note: %s"), *ReportBase, *AdditionalNote);
			}
			else
			{
				Preserved = ReportBase.IsEmpty() ? AdditionalNote : ReportBase;
			}
			if (!Preserved.IsEmpty())
			{
				CancellationText = FString::Printf(
					TEXT("Action completed before cancellation took effect: %s"), *Preserved);
			}
		}
		else if (!AdditionalNote.IsEmpty())
		{
			CancellationText = FString::Printf(TEXT("Cancelled action note: %s"), *AdditionalNote);
		}

		if (!CancellationText.IsEmpty())
		{
			AddContextEvent(CancellationText, EC_RunLLMOption::Never);
		}
		FinishActionCancellation();
		return;
	}

	// ── Combine base + optional AdditionalNote into the outgoing message.
	//    AdditionalNote reads as a ", note: ..." appendix to the synthesised
	//    base so the LLM gets the caller's extra detail without losing the
	//    standard framing.
	FString OutgoingText;
	if (ReportBase.IsEmpty())
	{
		OutgoingText = AdditionalNote; // bAutoReport=false (or no head) → AdditionalNote alone
	}
	else if (AdditionalNote.IsEmpty())
	{
		OutgoingText = ReportBase;
	}
	else
	{
		OutgoingText = FString::Printf(TEXT("%s, note: %s"), *ReportBase, *AdditionalNote);
	}

	// ── Success: advance the queue, then maybe start the next action after Delay.
	if (IsSuccessful)
	{
		// Make the queue transition authoritative before a caller-requested flush.
		// Otherwise that payload could report the completed action as still current.
		DequeueAction();
		ActiveActionHandlerTarget.Reset();
		bActiveCancelHookInvoked = false;
		ActionExecutionState = ActionsQueue.Num() == 0
			? EConvaiActionExecutionState::Idle
			: EConvaiActionExecutionState::DispatchPending;
		MarkManagedActionContextDirty();

		if (!OutgoingText.IsEmpty())
		{
			AddContextEvent(OutgoingText, ShouldRespond, Delivery, bEphemeral,
				bFlushImmediately);
		}

		if (IsActionsQueueEmpty())
			return;

		if (Delay > 0.0f)
		{
			const uint64 ExpectedGeneration = ++ActionDispatchGeneration;
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().ClearTimer(ActionCompletionDelayTimerHandle);
				World->GetTimerManager().SetTimer(ActionCompletionDelayTimerHandle,
					FTimerDelegate::CreateWeakLambda(this, [this, ExpectedGeneration]
					{
						if (ActionDispatchGeneration == ExpectedGeneration &&
							ActionExecutionState == EConvaiActionExecutionState::DispatchPending)
						{
							StartFirstAction();
						}
					}),
					Delay, false);
			}
			else
			{
				StartFirstAction();
			}
		}
		else
		{
			StartFirstAction();
		}
		return;
	}

	// ── Failure: drop the rest of the queue immediately; the outgoing event is
	//    what re-triggers the LLM, so that's what Delay defers.
	CancelPendingActionStart();
	ActionsQueue.Empty();
	PendingReplacementActions.Empty();
	ActiveActionHandlerTarget.Reset();
	bActiveCancelHookInvoked = false;
	ActionExecutionState = EConvaiActionExecutionState::Idle;
	MarkManagedActionContextDirty();

	if (!OutgoingText.IsEmpty())
	{
		if (Delay > 0.0f)
		{
			// CreateWeakLambda auto-guards against the chatbot being GC'd before
			// the timer fires; cheaper than a manual WeakObjectPtr check.
			const uint64 ExpectedGeneration = ActionDispatchGeneration;
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(
					ActionCompletionDelayTimerHandle,
					FTimerDelegate::CreateWeakLambda(this,
						[this, OutgoingText, ShouldRespond, Delivery, bEphemeral,
						 bFlushImmediately, ExpectedGeneration]
						{
							if (ActionDispatchGeneration == ExpectedGeneration)
							{
								AddContextEvent(OutgoingText, ShouldRespond,
									Delivery, bEphemeral, bFlushImmediately);
							}
						}),
					Delay, /*bLoop*/ false);
			}
		}
		else
		{
			AddContextEvent(OutgoingText, ShouldRespond, Delivery, bEphemeral,
				bFlushImmediately);
		}
	}
}

void UConvaiChatbotComponent::HandleActionCancellation()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
		const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, ExpectedEpoch]
		{
			if (UConvaiChatbotComponent* Self = WeakSelf.Get();
				Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
			{
				Self->HandleActionCancellation();
			}
		});
		return;
	}
	if (bActionLifecycleShuttingDown)
	{
		return;
	}
	if (ActionExecutionState != EConvaiActionExecutionState::Cancelling)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
			TEXT("HandleActionCancellation ignored because no action is cancelling."));
		return;
	}

	if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
	{
		FConvaiResultAction Head;
		Dbg->OnActionDebug.Broadcast(this,
			FetchFirstAction(Head) ? (Head.ActionString.IsEmpty() ? Head.Action : Head.ActionString) : FString(),
			EConvaiActionDebugPhase::Aborted, TEXT("Cancelled"));
	}
	FinishActionCancellation();
}

void UConvaiChatbotComponent::CancelCurrentActionPlan()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
		const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, ExpectedEpoch]
		{
			if (UConvaiChatbotComponent* Self = WeakSelf.Get();
				Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
			{
				Self->CancelCurrentActionPlan();
			}
		});
		return;
	}
	RequestActionPlanCancellation({});
}

void UConvaiChatbotComponent::RequestActionPlanCancellation(
	TArray<FConvaiResultAction> ReplacementActions)
{
	check(IsInGameThread());

	// A later redirect supersedes only the held replacement. The active action's
	// cancel hook is intentionally one-shot.
	if (ActionExecutionState == EConvaiActionExecutionState::Cancelling)
	{
		PendingReplacementActions = MoveTemp(ReplacementActions);
		MarkManagedActionContextDirty();
		return;
	}

	// Nothing has entered user code yet, so no acknowledgement is needed. Invalidate
	// every pending timer/lambda and start the replacement immediately.
	if (ActionExecutionState == EConvaiActionExecutionState::Idle ||
		ActionExecutionState == EConvaiActionExecutionState::DispatchPending ||
		ActionsQueue.Num() == 0)
	{
		CancelPendingActionStart();
		ActionsQueue = MoveTemp(ReplacementActions);
		PendingReplacementActions.Empty();
		ActiveActionHandlerTarget.Reset();
		bActiveCancelHookInvoked = false;
		ActionExecutionState = ActionsQueue.Num() == 0
			? EConvaiActionExecutionState::Idle
			: EConvaiActionExecutionState::DispatchPending;
		MarkManagedActionContextDirty();
		if (ActionsQueue.Num() > 0)
		{
			ScheduleFirstAction();
		}
		return;
	}

	// Keep exactly the active head as a tombstone. A normal completion callback
	// can now only consume this entry; replacement actions are held separately.
	if (ActionsQueue.Num() > 1)
	{
		ActionsQueue.SetNum(1);
	}
	PendingReplacementActions = MoveTemp(ReplacementActions);
	ActionExecutionState = EConvaiActionExecutionState::Cancelling;
	const uint64 ExpectedCancellationGeneration = ++ActionCancellationGeneration;
	MarkManagedActionContextDirty();
	InvokeActiveActionCancellationHandler();

	// A cancellation hook may acknowledge synchronously and even start a newer
	// plan before ProcessEvent returns. Never attach this attempt's diagnostic to
	// that newer lifecycle.
	if (ActionExecutionState != EConvaiActionExecutionState::Cancelling ||
		ActionCancellationGeneration != ExpectedCancellationGeneration)
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionCancellationDiagnosticTimerHandle);
		World->GetTimerManager().SetTimer(ActionCancellationDiagnosticTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, ExpectedCancellationGeneration]
			{
				if (ActionCancellationGeneration == ExpectedCancellationGeneration &&
					ActionExecutionState == EConvaiActionExecutionState::Cancelling)
				{
					CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
						TEXT("Action cancellation is still waiting for one terminal acknowledgement. Replacement actions remain safely held."));
				}
			}),
			CancellationDiagnosticDelaySeconds, false);
	}
}

void UConvaiChatbotComponent::InvokeActiveActionCancellationHandler()
{
	check(IsInGameThread());
	if (bActiveCancelHookInvoked ||
		ActionExecutionState != EConvaiActionExecutionState::Cancelling ||
		ActionsQueue.Num() == 0)
	{
		return;
	}
	bActiveCancelHookInvoked = true;

	UObject* Target = ActiveActionHandlerTarget.Get();
	if (!IsValid(Target))
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Log,
			TEXT("No live action handler target exists; waiting for the original action's terminal acknowledgement."));
		return;
	}

	FConvaiResultAction OriginalAction = ActionsQueue[0];
	const FString CancelHandlerName = FString(CancelHandlerPrefix) + OriginalAction.Action;
	if (!TryCallFunction(Target, CancelHandlerName, OriginalAction))
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Log,
			TEXT("Optional cancellation handler '%s' was not found on '%s'; waiting for natural completion."),
			*CancelHandlerName, *Target->GetName());
	}
}

void UConvaiChatbotComponent::FinishActionCancellation()
{
	check(IsInGameThread());
	if (ActionExecutionState != EConvaiActionExecutionState::Cancelling)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionCancellationDiagnosticTimerHandle);
	}
	++ActionDispatchGeneration;
	ActionsQueue = MoveTemp(PendingReplacementActions);
	PendingReplacementActions.Empty();
	ActiveActionHandlerTarget.Reset();
	bActiveCancelHookInvoked = false;
	ActionExecutionState = ActionsQueue.Num() == 0
		? EConvaiActionExecutionState::Idle
		: EConvaiActionExecutionState::DispatchPending;
	MarkManagedActionContextDirty();
	if (ActionsQueue.Num() > 0)
	{
		ScheduleFirstAction();
	}
}

void UConvaiChatbotComponent::CancelPendingActionStart()
{
	check(IsInGameThread());
	++ActionDispatchGeneration;
	PublishedActionWaitGeneration.Store(0);
	bWaitingForBotSpeechTrigger = false;
	PendingActionPostSpeechDelay = 0.0f;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionWaitTimerHandle);
		World->GetTimerManager().ClearTimer(PostSpeechDelayTimerHandle);
		World->GetTimerManager().ClearTimer(ActionCompletionDelayTimerHandle);
	}
}

void UConvaiChatbotComponent::ResetActionExecution(bool bLatchShutdown)
{
	if (!IsInGameThread())
	{
		return;
	}

	++ActionLifecycleEpoch;
	const bool bRemainShutDown = bActionLifecycleShuttingDown || bLatchShutdown;
	// Guard against the owned task's synchronous terminal callback while the
	// queue is being torn down. A transient disconnect restores the executor
	// afterward; terminal lifecycle paths keep it latched.
	bActionLifecycleShuttingDown = true;
	if ((ActionExecutionState == EConvaiActionExecutionState::Running ||
		 ActionExecutionState == EConvaiActionExecutionState::Cancelling) &&
		ActionsQueue.Num() > 0 && !bActiveCancelHookInvoked)
	{
		ActionExecutionState = EConvaiActionExecutionState::Cancelling;
		InvokeActiveActionCancellationHandler();
	}
	++ActionCancellationGeneration;
	CancelPendingActionStart();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionCancellationDiagnosticTimerHandle);
	}
	ActionsQueue.Empty();
	PendingReplacementActions.Empty();
	ActiveActionHandlerTarget.Reset();
	bActiveCancelHookInvoked = false;
	ActionExecutionState = EConvaiActionExecutionState::Idle;
	bActionLifecycleShuttingDown = bRemainShutDown;
	MarkManagedActionContextDirty();
}

void UConvaiChatbotComponent::AbortActionSequence(FString EventText, EC_RunLLMOption ShouldRespond)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
		const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
		AsyncTask(ENamedThreads::GameThread,
			[WeakSelf, ExpectedEpoch, EventText = MoveTemp(EventText), ShouldRespond]() mutable
			{
				if (UConvaiChatbotComponent* Self = WeakSelf.Get();
					Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
				{
					Self->AbortActionSequence(MoveTemp(EventText), ShouldRespond);
				}
			});
		return;
	}

	if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
	{
		FConvaiResultAction Head;
		Dbg->OnActionDebug.Broadcast(this,
			FetchFirstAction(Head) ? (Head.ActionString.IsEmpty() ? Head.Action : Head.ActionString) : FString(),
			EConvaiActionDebugPhase::Aborted, EventText);
	}

	// Abort is a hard local teardown, not a cooperative replacement barrier.
	// Reset invokes an active custom cancel hook once and stops plugin-owned
	// gameplay tasks while terminal callbacks are suppressed, then clears both
	// the current queue and any replacement held behind a cancel marker.
	ResetActionExecution(/*bLatchShutdown*/ false);

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
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
		const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, ExpectedEpoch]
		{
			if (UConvaiChatbotComponent* Self = WeakSelf.Get();
				Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
			{
				Self->ClearActionQueue();
			}
		});
		return;
	}

	PendingReplacementActions.Empty();
	if ((ActionExecutionState == EConvaiActionExecutionState::Running ||
		 ActionExecutionState == EConvaiActionExecutionState::Cancelling) &&
		ActionsQueue.Num() > 0)
	{
		ActionsQueue.SetNum(1);
	}
	else
	{
		CancelPendingActionStart();
		ActionsQueue.Empty();
		ActiveActionHandlerTarget.Reset();
		bActiveCancelHookInvoked = false;
		ActionExecutionState = EConvaiActionExecutionState::Idle;
	}
	MarkManagedActionContextDirty();
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
	if (!ensureMsgf(IsInGameThread(),
		TEXT("DequeueAction must only mutate the action queue on the game thread.")))
	{
		return false;
	}
	if (ActionsQueue.Num() > 0)
	{
		ActionsQueue.RemoveAt(0);
		return true;
	}
	return false;
}

bool UConvaiChatbotComponent::StartFirstAction()
{
	check(IsInGameThread());

	if (bActionLifecycleShuttingDown ||
		ActionExecutionState == EConvaiActionExecutionState::Running ||
		ActionExecutionState == EConvaiActionExecutionState::Cancelling)
	{
		return false;
	}

	FConvaiResultAction ConvaiResultAction;
	if (FetchFirstAction(ConvaiResultAction))
	{
		// Spatial polling may be disabled or stale. Resolve merged targets against
		// the bot's live position immediately before either native or Blueprint
		// dispatch, while the sequential lane still owns this head action.
		ConvaiMergedObjectNavigation::ResolveActionObjectReferences(
			*this, ConvaiResultAction);
		ActionsQueue[0] = ConvaiResultAction;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(ActionWaitTimerHandle);
			World->GetTimerManager().ClearTimer(PostSpeechDelayTimerHandle);
			World->GetTimerManager().ClearTimer(ActionCompletionDelayTimerHandle);
		}
		bWaitingForBotSpeechTrigger = false;
		PublishedActionWaitGeneration.Store(0);
		PendingActionPostSpeechDelay = 0.0f;
		ActiveActionHandlerTarget.Reset();
		bActiveCancelHookInvoked = false;
		ActionExecutionState = EConvaiActionExecutionState::Running;
		MarkManagedActionContextDirty();
		if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
		{
			Dbg->OnActionDebug.Broadcast(this,
				ConvaiResultAction.ActionString.IsEmpty()
					? ConvaiResultAction.Action : ConvaiResultAction.ActionString,
				EConvaiActionDebugPhase::Started, FString());
		}

		const bool bStarted = TriggerNamedBlueprintAction(
			ConvaiResultAction.Action, ConvaiResultAction);
		if (!bStarted && ActionExecutionState == EConvaiActionExecutionState::Running)
		{
			HandleActionCompletion(false, /*bAutoReport*/ true,
				EC_RunLLMOption::Never, TEXT("no matching action handler was found"));
		}
		return bStarted;
	}
	ActionExecutionState = EConvaiActionExecutionState::Idle;
	MarkManagedActionContextDirty();
	return false;
}

bool UConvaiChatbotComponent::TriggerNamedBlueprintAction(const FString& ActionName, FConvaiResultAction ConvaiActionStruct)
{
	if (!ActionName.IsEmpty())
	{
		// Check the owning actor first
		if (AActor* Owner = GetOwner())
		{
			ActiveActionHandlerTarget = Owner;
			if (TryCallFunction(Owner, ActionName, ConvaiActionStruct))
			{
				return true;
			}
			ActiveActionHandlerTarget.Reset();
		}

		// Fallback to self (BP_ConvaiChatbotComponent)
		ActiveActionHandlerTarget = this;
		if (TryCallFunction(this, ActionName, ConvaiActionStruct))
		{
			return true;
		}
		ActiveActionHandlerTarget.Reset();

		// Built-ins run last so a Blueprint handler with the action's name
		// (on the owner actor or a chatbot subclass) always overrides them.
		ActiveActionHandlerTarget = this;
		if (TryExecuteBuiltInAction(ActionName, ConvaiActionStruct))
		{
			return true;
		}
		ActiveActionHandlerTarget.Reset();

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

void UConvaiChatbotComponent::ExecuteNarrativeTrigger(const FString TriggerMessage, EConvaiContextDelivery Delivery)
{
	if (TriggerMessage.IsEmpty())
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning, TEXT("Invoke Speech: TriggerMessage is missing"));
		return;
	}
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Invoke Speech: Executed | Character ID : %s | Session ID : %s"),
		*CharacterID,
		*SessionID);

	// One-shot spoken cue: ephemeral so it never persists in the running context.
	AddContextEvent(TriggerMessage, EC_RunLLMOption::Always, Delivery, /*bEphemeral*/ true);
}

void UConvaiChatbotComponent::InvokeNarrativeDesignTrigger(const FString TriggerName)
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
		PendingTriggers.Add(MoveTemp(Pending));
		ScheduleContextFlush();
		return;
	}

	InvokeTrigger_Internal(TriggerName, "");
}

void UConvaiChatbotComponent::InvokeTrigger_Internal(const FString& TriggerName, const FString& TriggerMessage)
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
	// IsConnectionTalking covers the server-speaking → local-playback buffering
	// gap: OnStartedTalking sets it and clears thinking before audio is locally
	// audible, and an interrupt in that window must still broadcast.
	if (GetIsTalking() || IsConnectionTalking || IsProcessing())
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

void UConvaiChatbotComponent::ReleaseHeldContext()
{
	// Move the lane out first: the re-issued calls below run the normal-path
	// reconciliation (DropStateKey etc.) against the member, which must already
	// be empty so they can't eat the very items being released.
	FConvaiHeldContextLane Lane = MoveTemp(HeldContext);
	HeldContext.Clear();

	for (const FString& Key : Lane.StateOrder)
	{
		const FString* PreviousValueOverride =
			Lane.StatePreviousValueOverrides.Find(Key);
		SetContextStateInternal(Key, Lane.StateValues[Key], Lane.StateRespond[Key],
			EConvaiContextDelivery::SendNormally, /*bFlushImmediately*/ false,
			Lane.StateOmitPreviousValue.Contains(Key),
			/*bEvaluateWatches*/ true, PreviousValueOverride,
			Lane.StatePreserveTransitions.Contains(Key));
		if (Lane.WatchPromotedStateKeys.Contains(Key)
			&& PendingContextBatch.StagedStateOrder.Contains(Key))
		{
			PendingContextBatch.WatchPromotedStateKeys.Add(Key);
		}
	}
	for (const FString& Key : Lane.DeclarativeOrder)
	{
		SetContextFact(Lane.DeclarativeSentences[Key], Key, Lane.DeclarativeRespond[Key],
			EConvaiContextDelivery::SendNormally, /*bFlushImmediately*/ false);
	}
	for (const FConvaiHeldContextLane::FHeldEvent& Event : Lane.Events)
	{
		AddContextEvent(Event.Text, Event.ShouldRespond,
			EConvaiContextDelivery::SendNormally, Event.bEphemeral, /*bFlushImmediately*/ false);
	}
	if (Lane.bHasAttention)
	{
		// Local prep (mirror, registration, ownership stamp) already ran when the
		// call was held — only the remote staging releases here. A gaze-held cue
		// yields to an Explicit owner that appeared during the hold.
		if (!Lane.bAttentionFromGaze || AttentionSource != EConvaiAttentionSource::Explicit)
		{
			StageAttentionCue(Lane.AttentionObject, Lane.AttentionText, Lane.AttentionRespond, Lane.bAttentionAddEvent);
		}
	}

	// Flush NOW, always: the quiet period already proved the conversation is
	// idle, and routing through the debounce would let someone resume speaking
	// inside the window and receive the update mid-sentence — the exact failure
	// this feature exists to prevent. The lane already coalesced the updates,
	// so a second batching delay buys nothing.
	FlushDynamicContext();
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
				Existing.ObjectReference = Entry.ObjectReference;
				Existing.AcceptanceRadius = Entry.AcceptanceRadius;
				Existing.ComponentName = Entry.ComponentName;
				Existing.SocketOrBoneName = Entry.SocketOrBoneName;
				Existing.MovementPoints = Entry.MovementPoints;
				Existing.bFallbackToObjectWhenPointsUnreachable = Entry.bFallbackToObjectWhenPointsUnreachable;
				// Identity bookkeeping must follow the INCOMING entry: a real
				// object replacing a same-named destination entry clears the
				// sub-object flags — otherwise removing the destination's base
				// would cascade-delete the real object (fresh-codex P1).
				Existing.bIsMovementPointSubObject = Entry.bIsMovementPointSubObject;
				Existing.MovementPointSubObjectBaseName = Entry.MovementPointSubObjectBaseName;
				Existing.MovementPointSubObjectPointName = Entry.MovementPointSubObjectPointName;
				// Drop the cache — actor or filter may have changed, force lazy re-resolve.
				Existing.ResolvedComponent = nullptr;
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

	bool HasAutoSpawnCleanupOwner(
		const UObject* WorldContextObject,
		UConvaiObjectComponent* ObjectComponent)
	{
		if (!IsValid(ObjectComponent))
		{
			return false;
		}

		if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(WorldContextObject))
		{
			for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
			{
				if (IsValid(Chatbot) && Chatbot->OwnedAutoSpawnComponents.Contains(ObjectComponent))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool HasOtherAutoSpawnCleanupOwner(
		const UObject* WorldContextObject,
		UConvaiObjectComponent* ObjectComponent,
		const UConvaiChatbotComponent* ExcludedChatbot)
	{
		if (!IsValid(ObjectComponent))
		{
			return false;
		}

		if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(WorldContextObject))
		{
			for (UConvaiChatbotComponent* Chatbot : Subsystem->GetAllChatbotComponents())
			{
				if (IsValid(Chatbot)
					&& Chatbot != ExcludedChatbot
					&& Chatbot->OwnedAutoSpawnComponents.Contains(ObjectComponent))
				{
					return true;
				}
			}
		}
		return false;
	}
}

// ── Object mutation ───────────────────────────────────────────────────

void UConvaiChatbotComponent::AddOrUpdateObjectFromComponent(UConvaiObjectComponent* ObjectComponent)
{
	if (!IsValid(ObjectComponent) || !ObjectComponent->HasValidObjectName())
	{
		return;
	}

	AActor* Owner = ObjectComponent->GetOwner();

	// Copy the whole entry so navigation targeting fields (ObjectReference,
	// AcceptanceRadius, ComponentName, SocketOrBoneName, MovementPoints) flow
	// through to the chatbot. Then overlay the composed
	// description (which folds in the tracked-property descriptors).
	FConvaiObjectEntry Entry = ObjectComponent->ObjectEntry;
	Entry.Description = ObjectComponent->ComposeDescriptionForLLM();

	// Merged set: several components share this final name and must collapse into a
	// SINGLE object entry. Build it deterministically from the whole set (not from
	// whichever member triggered this update): the first registered member is the
	// representative for nav fields + Ref baseline (Phase 5b later repoints Ref at the
	// nearest reachable member), and the first member with a non-empty description
	// supplies the one shared description.
	TArray<UConvaiObjectComponent*> Members{ ObjectComponent };
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		Subsystem->GetObjectGroupMembers(ObjectComponent, Members);
		if (Members.Num() > 1)
		{
			UConvaiObjectComponent* Representative = Members[0];
			if (IsValid(Representative))
			{
				Entry = Representative->ObjectEntry; // nav fields + Ref baseline + shared name
				Owner = Representative->GetOwner();
			}
			UConvaiObjectComponent* DescSource = nullptr;
			for (UConvaiObjectComponent* Member : Members)
			{
				if (IsValid(Member) && !Member->ObjectEntry.Description.IsEmpty())
				{
					DescSource = Member;
					break;
				}
			}
			if (!IsValid(DescSource)) { DescSource = Representative; }
			if (IsValid(DescSource))
			{
				Entry.Description = DescSource->ComposeDescriptionForLLM();
			}
		}
	}

	if (!Entry.Ref.IsValid())
	{
		Entry.Ref = Owner;
	}

	// ── Duplicate-description dedup ──
	// Five identical coins each carrying the same long description would
	// repeat it five times in the prompt; later registrations point at the
	// first instead ("Same as coin."). In-channel on purpose — a declarative
	// fact would need per-chatbot refcounting and retirement. Known edge: if
	// the FIRST instance unregisters mid-session, the pointer dangles until
	// the next re-registration; acceptable for descriptive text.
	if (!Entry.Description.IsEmpty())
	{
		const FConvaiObjectEntry* Original = EnvironmentData.Objects.FindByPredicate(
			[&Entry](const FConvaiObjectEntry& E)
			{
				// Generated movement-point sub-entries never serve as the
				// canonical source — pointing at "Door Other Side" would break
				// when the destination retires with its point.
				return !E.bIsMovementPointSubObject
					&& E.Name != Entry.Name
					&& E.Description == Entry.Description;
			});
		if (Original)
		{
			const FString Pointer = FString::Printf(TEXT("Same as %s."), *Original->Name);
			if (Entry.Description.Len() > Pointer.Len())
			{
				Entry.Description = Pointer;
			}
		}
	}

	// ── Named movement points → sub-object entries ──
	// One clone per distinct point name across the member set: "Door" + point
	// "Other Side" becomes an addressable "Door Other Side" whose movement
	// resolves ONLY those points; the base entry keeps the unnamed points
	// (none left = body-fallback resolution, so "go to Door" still means the
	// door itself). Point names are authoring-time identity — runtime renames
	// take effect on re-registration, not mid-session.
	TArray<FConvaiObjectEntry> SubEntries;
	{
		TArray<const FConvaiObjectEntry*> MemberEntries;
		for (UConvaiObjectComponent* Member : Members)
		{
			if (IsValid(Member)) { MemberEntries.Add(&Member->ObjectEntry); }
		}
		TArray<FString> SubNames;
		FConvaiObjectEntry::CollectMovementPointSubNames(MemberEntries, SubNames);
		for (const FString& SubName : SubNames)
		{
			// Source = first member owning that name: its actor anchors the
			// relative points. The spatial pass repoints the entry at the
			// nearest reachable member later, exactly like the base entry.
			const FString Key = FConvaiObjectEntry::NormalizeMovementPointName(SubName).ToLower();
			UConvaiObjectComponent* Source = nullptr;
			for (UConvaiObjectComponent* Member : Members)
			{
				if (!IsValid(Member)) { continue; }
				const bool bHasName = Member->ObjectEntry.MovementPoints.ContainsByPredicate(
					[&Key](const FConvaiMovementPoint& P)
					{
						return P.bEnabled
							&& FConvaiObjectEntry::EffectiveMovementPointName(P).ToLower() == Key;
					});
				if (bHasName) { Source = Member; break; }
			}
			if (!Source) { continue; }
			FConvaiObjectEntry SubEntry = Source->ObjectEntry.MakeMovementPointSubEntry(SubName);
			if (!SubEntry.Ref.IsValid())
			{
				SubEntry.Ref = Source->GetOwner();
			}
			SubEntry.Description = FConvaiObjectEntry::MovementPointDestinationDescription(
				Entry.Name, MemberEntries, SubName);
			// A generated name can collide with a REAL object ("Door" + point
			// "Key" vs an object named "Door Key"). The designer must rename
			// one — silently shadowing either would misroute actions.
			const bool bClashes = EnvironmentData.Objects.ContainsByPredicate(
				[&](const FConvaiObjectEntry& E)
				{
					return E.Name.Equals(SubEntry.Name, ESearchCase::IgnoreCase)
						&& !(E.bIsMovementPointSubObject
							&& E.MovementPointSubObjectBaseName == Entry.Name);
				});
			if (bClashes)
			{
				CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
					TEXT("Movement point '%s' on object '%s' generates the name '%s', which collides with an existing object — sub-object skipped. Rename the point or the object."),
					*SubName, *Entry.Name, *SubEntry.Name);
				continue;
			}
			SubEntries.Add(MoveTemp(SubEntry));
		}
		if (SubEntries.Num() > 0)
		{
			Entry.FilterMovementPointsToObjectItself();
		}
	}

	const bool bAlreadyPresent = EnvironmentData.Objects.ContainsByPredicate(
		[&](const FConvaiObjectEntry& E){ return E.Name == Entry.Name; });

	AddOrReplaceEntry(EnvironmentData.Objects, Entry);
	if (bAlreadyPresent)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Log,
			TEXT("Replaced existing object entry '%s' from ConvaiObjectComponent on %s"),
			*Entry.Name, Owner ? *Owner->GetName() : TEXT("<no owner>"));
	}

	bool bDirty = !EnvironmentSnapshotAtConnect.Contains(Entry.Name);
	for (const FConvaiObjectEntry& SubEntry : SubEntries)
	{
		AddOrReplaceEntry(EnvironmentData.Objects, SubEntry);
		bDirty |= !EnvironmentSnapshotAtConnect.Contains(SubEntry.Name);
	}
	// Retire subs whose point name was removed/renamed since the last rebuild.
	bDirty |= EnvironmentData.Objects.RemoveAll(
		[&](const FConvaiObjectEntry& E)
		{
			return E.bIsMovementPointSubObject
				&& E.MovementPointSubObjectBaseName == Entry.Name
				&& !SubEntries.ContainsByPredicate(
					[&E](const FConvaiObjectEntry& S){ return S.Name == E.Name; });
		}) > 0;

	// If we are already past connect, queue a scene-metadata refresh so the LLM picks
	// up the new/updated entries.
	if (bDirty)
	{
		PendingEnvironmentBatch.bSceneMetadataDirty = true;
		ScheduleOrFlushDynamicContext(/*bFlushImmediately*/ false);
	}
}

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
	// Make sure the actor referenced by this entry has a UConvaiObjectComponent so the player's
	// gaze tracker can find it — idempotent if one is already present.
	EnsureObjectComponentsForEnvironmentObjects();
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
	// Same auto-spawn pass we run on AddObject — keeps the actor-side ObjectComponent state
	// in sync with EnvironmentData.Objects so gaze tracking sees every entry.
	EnsureObjectComponentsForEnvironmentObjects();
}

void UConvaiChatbotComponent::RemoveObject(FString ObjectName, bool bFlushImmediately)
{
	bool bRemoved = RemoveEntryByName(EnvironmentData.Objects, ObjectName);
	// A base object takes its movement-point sub-objects with it — they are
	// projections of its named points, meaningless without it.
	bRemoved |= EnvironmentData.Objects.RemoveAll(
		[&ObjectName](const FConvaiObjectEntry& E)
		{
			return E.bIsMovementPointSubObject
				&& E.MovementPointSubObjectBaseName == ObjectName;
		}) > 0;
	if (bRemoved)
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
		// Same cascade as RemoveObject: a base takes its movement-point
		// sub-objects with it.
		bAnyRemoved |= EnvironmentData.Objects.RemoveAll(
			[&Name](const FConvaiObjectEntry& E)
			{
				return E.bIsMovementPointSubObject
					&& E.MovementPointSubObjectBaseName == Name;
			}) > 0;
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
			if (List[i].Name.Equals(Name, ESearchCase::IgnoreCase)) return i;
		}
		return INDEX_NONE;
	}

	FString QuoteManagedActionLabel(FString Label)
	{
		// Keep a model-provided invocation on one bounded line. This is display
		// context, not a command language, so collapsing whitespace is lossless and
		// prevents an action argument from injecting another managed section.
		TArray<FString> Words;
		Label.ParseIntoArrayWS(Words);
		Label = FString::Join(Words, TEXT(" "));
		if (Label.IsEmpty())
		{
			Label = TEXT("Unknown action");
		}
		constexpr int32 MaxLabelCharacters = 240;
		if (Label.Len() > MaxLabelCharacters)
		{
			Label = Label.Left(MaxLabelCharacters - 3) + TEXT("...");
		}
		Label.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Label.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Label);
	}

	FString BuildManagedActionLabel(
		const FConvaiResultAction& Result,
		const TArray<FConvaiAction>& Templates)
	{
		const int32 TemplateIndex = IndexOfActionByName(Templates, Result.Action);
		if (Templates.IsValidIndex(TemplateIndex))
		{
			const FConvaiAction& ActionTemplate = Templates[TemplateIndex];
			FString Invocation = ActionTemplate.Name;
			for (const FConvaiActionParam& DeclaredParam : ActionTemplate.Parameters)
			{
				const FConvaiResultParam* Value = Result.Parameters.Find(DeclaredParam.Name);
				if (Value == nullptr)
				{
					continue;
				}

				FString Text = Value->StringValue.TrimStartAndEnd();
				if (Text.IsEmpty())
				{
					Text = Value->RefValue.Name.TrimStartAndEnd();
				}
				if (Text.IsEmpty())
				{
					continue;
				}
				if (!DeclaredParam.Connector.IsEmpty())
				{
					Invocation += TEXT(" ") + DeclaredParam.Connector;
				}
				Invocation += TEXT(" ") + Text;
			}

			// A matched template is authoritative even if the model omitted every
			// parameter. Present values use declaration order/connectors; missing
			// values (and their connectors) are simply absent.
			return QuoteManagedActionLabel(MoveTemp(Invocation));
		}

		return QuoteManagedActionLabel(Result.ActionString.IsEmpty()
			? Result.Action
			: Result.ActionString);
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

	// ── Built-in (experimental) action templates ──────────────────────
	// Descriptions are deliberately terse — every word here rides the connect
	// prompt of each session that enables the toggle. (Name constants live at
	// the top of the file.)

	FConvaiAction BuildRemindSelfAction()
	{
		return FConvaiAction(RemindSelfActionName,
			TEXT("Queue this after another action to remind yourself what to consider or say when it finishes."),
			{ FConvaiActionParam(RemindSelfParamName, TEXT("What to remember, consider, or say."), EConvaiActionParamType::String) });
	}

	FConvaiAction BuildWatchPropertyAction()
	{
		return FConvaiAction(WatchPropertyActionName,
			TEXT("Watch a tracked property once. Use its exact context key for the next change, or append ' = exact value' to wait until a later change reaches that value."),
			{ FConvaiActionParam(WatchPropertyParamName, TEXT("Exact context key, optionally followed by ' = exact value'."), EConvaiActionParamType::String) });
	}

	FConvaiAction BuildCancelActionPlanAction()
	{
		return FConvaiAction(CancelActionPlanActionName,
			TEXT("Cancel the current action plan before replacing it when the user redirects or abandons that plan."));
	}

	// Value of a built-in handler's parameter; falls back to the "target" slot
	// the parser uses when it couldn't split named params.
	FString GetActionStringParam(const FConvaiResultAction& Action, const TCHAR* ParamName)
	{
		if (const FConvaiResultParam* Param = Action.Parameters.Find(ParamName))
		{
			return Param->StringValue;
		}
		if (const FConvaiResultParam* Target = Action.Parameters.Find(TEXT("target")))
		{
			return Target->StringValue;
		}
		return FString();
	}

}

void UConvaiChatbotComponent::GetEffectiveActions(TArray<FConvaiAction>& OutActions) const
{
	OutActions.Reset();
	OutActions.Reserve(EnvironmentData.Actions.Num() + 3);
	for (const FConvaiAction& Action : EnvironmentData.Actions)
	{
		if (Action.bEnabled && !Action.Name.IsEmpty())
		{
			OutActions.Add(Action);
		}
	}
	// Every authored name reserves its identity, even while disabled. This keeps
	// disabling a project action from silently activating a native behavior with
	// the same case-insensitive name.
	if (bEnableRemindSelfAction &&
		IndexOfActionByName(EnvironmentData.Actions, RemindSelfActionName) == INDEX_NONE)
	{
		OutActions.Add(BuildRemindSelfAction());
	}
	if (bEnableWatchPropertyAction &&
		IndexOfActionByName(EnvironmentData.Actions, WatchPropertyActionName) == INDEX_NONE)
	{
		OutActions.Add(BuildWatchPropertyAction());
	}
	if (bEnableCancelActionPlanAction &&
		IndexOfActionByName(EnvironmentData.Actions, CancelActionPlanActionName) == INDEX_NONE)
	{
		OutActions.Add(BuildCancelActionPlanAction());
	}
}

void UConvaiChatbotComponent::GetActionsForParsing(TArray<FConvaiAction>& OutActions) const
{
	// The connect-time contract when one exists — the server can only send what
	// was advertised, and a valid contract may be EMPTY (actions disabled /
	// zero actions at connect). Live effective list only as a never-connected
	// fallback, which should be unreachable in practice (no contract → no
	// server-side actions to receive).
	if (bHasConnectedActionsSnapshot)
	{
		OutActions = ConnectedActionsSnapshot;
		return;
	}
	GetEffectiveActions(OutActions);
}

FString UConvaiChatbotComponent::GetActionConfigJson()
{
	// Freeze the effective action set at the moment the connect payload is
	// built. Incoming action responses parse against this snapshot and built-in
	// dispatch consults the frozen built-in names or connected action schema —
	// never the live toggles/rows — so a mid-session edit can't stall an
	// already-advertised action's queue (or activate an unadvertised one).
	// Contract edits apply on reconnect, like every other action mutation.
	ConnectedActionsSnapshot.Reset();
	ConnectedBuiltInNames.Reset();
	if (!EnvironmentData.bEnableActions)
	{
		// A connect with actions disabled is a valid EMPTY contract — parsing
		// must not fall back to the live list for this session.
		bHasConnectedActionsSnapshot = true;
		return FString();
	}
	GetEffectiveActions(ConnectedActionsSnapshot);
	if (bEnableRemindSelfAction &&
		IndexOfActionByName(EnvironmentData.Actions, RemindSelfActionName) == INDEX_NONE)
	{
		ConnectedBuiltInNames.Add(RemindSelfActionName);
	}
	if (bEnableWatchPropertyAction &&
		IndexOfActionByName(EnvironmentData.Actions, WatchPropertyActionName) == INDEX_NONE)
	{
		ConnectedBuiltInNames.Add(WatchPropertyActionName);
	}
	if (bEnableCancelActionPlanAction &&
		IndexOfActionByName(EnvironmentData.Actions, CancelActionPlanActionName) == INDEX_NONE)
	{
		ConnectedBuiltInNames.Add(CancelActionPlanActionName);
	}

	// Serialize a copy so the Details-panel array never shows the built-ins.
	FConvaiEnvironmentData Effective = EnvironmentData;
	Effective.Actions = ConnectedActionsSnapshot;
	const FString Json = Effective.ToActionConfigJson();
	bHasConnectedActionsSnapshot = true;
	return Json;
}

bool UConvaiChatbotComponent::WasBuiltInActionAdvertised(const TCHAR* ActionName) const
{
	return ConnectedBuiltInNames.ContainsByPredicate(
		[ActionName](const FString& Name)
		{
			return Name.Equals(ActionName, ESearchCase::IgnoreCase);
		});
}

FString UConvaiChatbotComponent::BuildManagedActionContext() const
{
	TArray<FString> InstructionLines;

	// Tell the model which frame each spatial clause belongs to — but only while
	// the published facts actually carry a player-perspective clause; the rule
	// must never advertise a "From <name>" clause that isn't in the context.
	if (const UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (const UConvaiContextSubsystem* Ctx = GI->GetSubsystem<UConvaiContextSubsystem>())
		{
			if (Ctx->HasPlayerPerspectiveFacts(this))
			{
				InstructionLines.Add(TEXT("- Phrases with \"you\"/\"your\" (\"in front of you\") locate things from YOUR own body and facing. When telling a player where something is, use the \"From <name>'s position\" part of that thing's description instead — that is how THEY see it."));
			}
		}
	}

	if (WasBuiltInActionAdvertised(RemindSelfActionName))
	{
		InstructionLines.Add(TEXT("- After an action that needs speech or follow-up, queue Remind Self immediately after it with what to consider or say."));
	}
	if (WasBuiltInActionAdvertised(WatchPropertyActionName))
	{
		InstructionLines.Add(TEXT("- Use Watch Property with an exact context key when you should react once to that property's next change."));
	}
	if (WasBuiltInActionAdvertised(CancelActionPlanActionName))
	{
		InstructionLines.Add(TEXT("- If the user redirects or abandons a plan, place Cancel Action Plan before its replacement actions and acknowledge the change in the same response."));
	}

	FString Result;
	if (InstructionLines.Num() > 0)
	{
		Result = TEXT("Instructions:\n") + FString::Join(InstructionLines, TEXT("\n"));
	}

	if (ActionExecutionState != EConvaiActionExecutionState::Idle && ActionsQueue.Num() > 0)
	{
		TArray<FConvaiAction> ActionTemplates;
		GetActionsForParsing(ActionTemplates);
		const FString CurrentLabel = BuildManagedActionLabel(ActionsQueue[0], ActionTemplates);

		const TArray<FConvaiResultAction>& Following =
			ActionExecutionState == EConvaiActionExecutionState::Cancelling
				? PendingReplacementActions
				: ActionsQueue;
		const int32 NextIndex = ActionExecutionState == EConvaiActionExecutionState::Cancelling ? 0 : 1;
		TArray<FString> PlanLines;
		switch (ActionExecutionState)
		{
			case EConvaiActionExecutionState::Running:
				PlanLines.Add(FString::Printf(
					TEXT("- Currently doing action %s."), *CurrentLabel));
				break;
			case EConvaiActionExecutionState::Cancelling:
				PlanLines.Add(FString::Printf(
					TEXT("- Currently cancelling action %s."), *CurrentLabel));
				break;
			case EConvaiActionExecutionState::DispatchPending:
				PlanLines.Add(FString::Printf(
					TEXT("- Waiting to start action %s."), *CurrentLabel));
				break;
			default:
				break;
		}
		if (Following.IsValidIndex(NextIndex))
		{
			const FConvaiResultAction& Next = Following[NextIndex];
			const FString NextLabel = BuildManagedActionLabel(Next, ActionTemplates);
			PlanLines.Add(FString::Printf(TEXT("- Next action: %s."), *NextLabel));
			const int32 Remaining = FMath::Max(0, Following.Num() - NextIndex - 1);
			if (Remaining > 0)
			{
				PlanLines.Add(FString::Printf(TEXT("- Additional queued actions: %d."), Remaining));
			}
		}

		const FString PlanBlock = TEXT("Current action plan:\n") + FString::Join(PlanLines, TEXT("\n"));
		Result = Result.IsEmpty() ? PlanBlock : Result + TEXT("\n") + PlanBlock;
	}
	return Result;
}

FString UConvaiChatbotComponent::BuildCanonicalContextWithManagedActions(
	const TSet<FString>& ExcludeStateKeys) const
{
	FString Result = DynamicContextTracker.BuildCanonicalContext(ExcludeStateKeys);
	const FString ManagedActionContext = BuildManagedActionContext();
	if (!ManagedActionContext.IsEmpty())
	{
		Result = Result.IsEmpty()
			? ManagedActionContext
			: Result + TEXT("\n") + ManagedActionContext;
	}
	return Result;
}

void UConvaiChatbotComponent::MarkManagedActionContextDirty()
{
	if (!IsInGameThread())
	{
		return;
	}
	PendingContextBatch.bForceReplace = true;
	ScheduleOrFlushDynamicContext(/*bFlushImmediately*/ false);
}

void UConvaiChatbotComponent::SyncManagedActionContext()
{
	// Remove the two stable facts emitted by pre-cancellation builds, then use one
	// generated block so live plan state replaces instead of accumulating events.
	RemoveContextFact(TEXT("BuiltInAction:RemindSelf"));
	RemoveContextFact(TEXT("BuiltInAction:WatchProperty"));
	MarkManagedActionContextDirty();
}

bool UConvaiChatbotComponent::TryExecuteBuiltInAction(const FString& ActionName, const FConvaiResultAction& Action)
{
	// Gate on what was ADVERTISED at connect, not the live toggles: the server
	// keeps sending what it was told about, and a designer action shadowing a
	// built-in name must never fall through to the built-in handler.
	const bool bConnected = ConnectedBuiltInNames.ContainsByPredicate(
		[&ActionName](const FString& Name) { return Name.Equals(ActionName, ESearchCase::IgnoreCase); });
	if (!bConnected)
	{
		return false;
	}
	// ActionName is the canonical template name (FindActionTemplate resolved it),
	// but stay case-insensitive for the no-template fallback path.
	if (ActionName.Equals(RemindSelfActionName, ESearchCase::IgnoreCase))
	{
		ExecuteRemindSelfAction(Action);
		return true;
	}
	if (ActionName.Equals(WatchPropertyActionName, ESearchCase::IgnoreCase))
	{
		ExecuteWatchPropertyAction(Action);
		return true;
	}
	if (ActionName.Equals(CancelActionPlanActionName, ESearchCase::IgnoreCase))
	{
		// This reserved control is normally consumed before queue insertion. Keep
		// the fallback terminal so a manually-started queue cannot stall.
		HandleActionCompletion(true, /*bAutoReport*/ false);
		return true;
	}
	return false;
}

void UConvaiChatbotComponent::ExecuteRemindSelfAction(const FConvaiResultAction& Action)
{
	const FString Reminder = GetActionStringParam(Action, RemindSelfParamName).TrimStartAndEnd();
	if (Reminder.IsEmpty())
	{
		// Always: a silent failure would leave the LLM believing the reminder is armed.
		HandleActionCompletion(false, /*bAutoReport*/ true, EC_RunLLMOption::Always,
			TEXT("the reminder text was missing"));
		return;
	}

	// Always + ephemeral + idle delivery: the reminder must produce a response,
	// is consumed exactly once, and waits for a genuine pause in the
	// conversation instead of talking over anyone — however long that takes.
	AddContextEvent(FString::Printf(TEXT("Reminder to yourself: %s"), *Reminder),
		EC_RunLLMOption::Always, EConvaiContextDelivery::WaitUntilConversationIsIdle,
		/*bEphemeral*/ true, /*bFlushImmediately*/ false);

	// The reminder event IS the outcome report — the default "you were able
	// to..." line would just repeat it and spend tokens twice.
	HandleActionCompletion(true, /*bAutoReport*/ false);
}

void UConvaiChatbotComponent::ExecuteWatchPropertyAction(const FConvaiResultAction& Action)
{
	FString Query = GetActionStringParam(Action, WatchPropertyParamName).TrimStartAndEnd();
	if (Query.IsEmpty())
	{
		HandleActionCompletion(false, /*bAutoReport*/ true, EC_RunLLMOption::Always,
			TEXT("no property name was given"));
		return;
	}

	// Keep the wire action compact (one parameter) while allowing a precise
	// target: "Platform.Movement = Stopped". Without '=', existing behaviour is
	// unchanged and the next genuine value change satisfies the watch.
	FString TargetValue;
	int32 EqualsIndex = INDEX_NONE;
	if (Query.FindChar(TEXT('='), EqualsIndex))
	{
		TargetValue = Query.Mid(EqualsIndex + 1).TrimStartAndEnd();
		Query = Query.Left(EqualsIndex).TrimStartAndEnd();
		if (Query.IsEmpty() || TargetValue.IsEmpty())
		{
			HandleActionCompletion(false, /*bAutoReport*/ true, EC_RunLLMOption::Always,
				TEXT("use 'Property.Key = exact value', or omit '= exact value' to watch any change"));
			return;
		}
	}

	// Collect every tracked-property context key in the world — the exact keys
	// the LLM already sees in its context states (e.g. "FrontDoor.DoorState").
	TArray<FString> Keys;
	if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		for (UConvaiObjectComponent* ObjectComponent : Subsystem->GetAllObjectComponents())
		{
			if (IsValid(ObjectComponent))
			{
				ObjectComponent->AppendTrackedPropertyContextKeys(Keys);
			}
		}
	}

	// Exact (case-insensitive) match first — on the raw query, then on its
	// sanitized form (the LLM may echo the spaced pre-sanitize wording).
	const FString SanitizedQuery = ConvaiContextFormat::SanitizeKey(Query);
	FString Resolved;
	for (const FString& Key : Keys)
	{
		if (Key.Equals(Query, ESearchCase::IgnoreCase) || Key.Equals(SanitizedQuery, ESearchCase::IgnoreCase))
		{
			Resolved = Key;
			break;
		}
	}

	// Fuzzy fallback: unique winner under ONE metric — lowercased bounded
	// Levenshtein (the same primitive and threshold family the action parser
	// uses for enum labels). No substring pass: property keys are identifiers,
	// and judging a substring hit by a different metric made acceptance
	// internally inconsistent. Ties reject — arming the wrong "Door.State" is
	// worse than asking the LLM to retry with the exact key.
	if (Resolved.IsEmpty() && Keys.Num() > 0)
	{
		const FString LowerQuery = SanitizedQuery.ToLower();
		const int32 Threshold = FMath::Clamp(LowerQuery.Len() / 2, 2, 4);
		int32 BestDistance = MAX_int32;
		int32 BestIndex = INDEX_NONE;
		bool bTie = false;
		for (int32 i = 0; i < Keys.Num(); ++i)
		{
			const int32 Distance = UConvaiUtils::LevenshteinDistance(LowerQuery, Keys[i].ToLower());
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestIndex = i;
				bTie = false;
			}
			else if (Distance == BestDistance)
			{
				bTie = true;
			}
		}
		if (BestIndex != INDEX_NONE && !bTie && BestDistance <= Threshold)
		{
			Resolved = Keys[BestIndex];
		}
	}

	if (Resolved.IsEmpty())
	{
		// Always: the LLM needs to know the watch is NOT armed so it can replan
		// (retry with the exact key it sees in its context states).
		HandleActionCompletion(false, /*bAutoReport*/ true, EC_RunLLMOption::Always,
			FString::Printf(TEXT("no tracked property matching '%s' was found — use the exact property name from your context"), *Query));
		return;
	}

	// Idempotent re-arm; consumed (one-shot) in SetContextState.
	WatchedContextKeys.Add(Resolved, TargetValue);
	TargetWatchesWithObservedDeparture.Remove(Resolved);

	// No outcome event: the LLM just issued the action and the armed watch IS
	// the state — a persistent "you were able to..." line would spend tokens on
	// every later prompt for nothing.
	HandleActionCompletion(true, /*bAutoReport*/ false);
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
	FString Text, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery, bool bAddAttentionEvent, bool bFlushImmediately)
{
	if (!EnvironmentData.bEnableActions)
	{
		// Server resolves attention only when action_config was sent at /connect, which only
		// happens when actions are enabled. Surface this as a Warning (not Verbose): a silent
		// skip here is one of the most common reasons a SetObjectInAttention call "does nothing".
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
			TEXT("SetObjectInAttention('%s') had no effect: this chatbot's Environment has bEnableActions = false. ")
			TEXT("Attention is resolved server-side from action_config, which is only sent when actions are enabled — ")
			TEXT("enable Actions on the chatbot for attention to take effect."),
			*AttentionObject.Name);
		return;
	}

	// 1. Update local mirror — required for next reconnect's action_config.current_attention_object.
	//    Runs even when delivery is deferred below: local preparation (mirror,
	//    object auto-registration, ownership) cannot safely wait — a pre-connect
	//    held call must still get its object into the connect-time action_config.
	EnvironmentData.CurrentAttentionObject = AttentionObject;

	// 2. Auto-register in EnvironmentData.Objects if missing, and surface the two reasons an
	//    otherwise-valid attention set silently fails to resolve. Server resolves attention only
	//    against action_config.objects (sent at /connect), even if the entry is also in Characters.
	if (!AttentionObject.Name.IsEmpty())
	{
		// (a) A name this chatbot has never heard of (typo, or AddObject was never called).
		//     Auto-add it so the local mirror and the next reconnect's action_config are correct.
		const bool bMissingFromList = EnvironmentData.FindObject(AttentionObject.Name) == nullptr;
		if (bMissingFromList)
		{
			EnvironmentData.Objects.Add(AttentionObject);
			if (!EnvironmentSnapshotAtConnect.Contains(AttentionObject.Name))
			{
				PendingEnvironmentBatch.bSceneMetadataDirty = true;
			}
		}

		// (b) Even a known object can't be an attention target this session unless it was among
		//     the objects sent at /connect — mid-session additions reach the server only via
		//     update-scene-metadata (descriptive-only). It becomes resolvable after the next
		//     reconnect folds it into action_config.
		const bool bNotResolvableLive =
			IsChatbotConnected() && !EnvironmentSnapshotAtConnect.Contains(AttentionObject.Name);

		if (bMissingFromList || bNotResolvableLive)
		{
			CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
				TEXT("SetObjectInAttention('%s'): %s%s"),
				*AttentionObject.Name,
				bMissingFromList
					? TEXT("object was not in this chatbot's environment object list — auto-registered it now; verify the name matches an object you added via AddObject / the Environment. ")
					: TEXT(""),
				bNotResolvableLive
					? TEXT("object was not among the objects sent to the server at connect time, so the server cannot resolve attention to it this session — it takes effect after the next reconnect (register the object before StartSession for it to apply immediately).")
					: TEXT(""));
		}
	}

	// 3. Deliver — now, or once the conversation is idle. Only the REMOTE half
	//    (the staged slot + response event) defers; steps 1-2 above and the
	//    ownership stamp below already happened, so a held explicit set blocks
	//    gaze for the whole hold and a pre-connect hold still reaches the
	//    connect-time action_config. A newer attention call of either kind
	//    supersedes an older held one (last wins).
	if (ShouldHoldForConversation(ShouldRespond, Delivery))
	{
		HeldContext.HoldAttention(AttentionObject, Text, ShouldRespond, bAddAttentionEvent, /*bFromGaze*/ false);
		HeldContext.bReleaseAsSoonAsIdle |= bFlushImmediately;
	}
	else
	{
		HeldContext.DropAttention();
		StageAttentionCue(AttentionObject, Text, ShouldRespond, bAddAttentionEvent);
		ScheduleOrFlushDynamicContext(bFlushImmediately);
	}

	// 4. Mark explicit ownership so gaze-driven sets can't trample this until BP clears.
	//    An empty-name AttentionObject is treated as "clear" — release ownership back to None
	//    so gaze can take over again.
	AttentionSource = AttentionObject.Name.IsEmpty()
		? EConvaiAttentionSource::None
		: EConvaiAttentionSource::Explicit;
}

void UConvaiChatbotComponent::StageAttentionCue(const FConvaiObjectEntry& AttentionObject,
	const FString& Text, EC_RunLLMOption ShouldRespond, bool bAddAttentionEvent)
{
	// Stage the attention slot, and (by default) a ONE-FLUSH event announcing the focus.
	// The ephemeral event lets the AI react once to the new focus without the cue piling up
	// as attention changes; it never enters the persisted context. Emitted only when the
	// caller wants it AND ShouldRespond is Auto/Always (Never sets the slot silently). The
	// optional Text is appended to the sentence. When the event is NOT emitted, the optional
	// Text falls back to a normal (persistent) event via the slot's text promotion.
	const bool bEmitAttentionEvent =
		bAddAttentionEvent && ShouldRespond != EC_RunLLMOption::Never && !AttentionObject.Name.IsEmpty();
	if (bEmitAttentionEvent)
	{
		const FString Observer = ConversationPartner.Name.IsEmpty() ? TEXT("The user") : ConversationPartner.Name;
		FString AttnEvent = FString::Printf(TEXT("%s is paying attention to %s"), *Observer, *AttentionObject.Name);
		if (!Text.IsEmpty())
		{
			AttnEvent += FString::Printf(TEXT(". %s"), *Text);
		}
		PendingContextBatch.StageEvent(AttnEvent, ShouldRespond, /*bEphemeral*/ true);
		// Slot only — the optional Text is already folded into the ephemeral event above.
		PendingContextBatch.StageAttention(AttentionObject, TEXT(""), ShouldRespond);
	}
	else
	{
		// Silent / Never path: stage the slot and let any optional Text become a normal
		// (persistent) event via the flush's attention-text promotion (legacy behaviour).
		PendingContextBatch.StageAttention(AttentionObject, Text, ShouldRespond);
	}
}

bool UConvaiChatbotComponent::TrySetObjectInAttentionFromGaze(FConvaiObjectEntry AttentionObject,
	FString Text, EC_RunLLMOption ShouldRespond, EConvaiContextDelivery Delivery, bool bAddAttentionEvent, bool bFlushImmediately)
{
	if (AttentionSource == EConvaiAttentionSource::Explicit)
	{
		// [GAZE-DEBUG] Verbose; bump to Log when debugging gaze-vs-explicit attention conflicts.
		CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
			TEXT("[Gaze] TrySetObjectInAttentionFromGaze rejected on '%s': slot owned by Explicit (BP/C++)."),
			*GetName());
		return false;
	}

	// Don't claim ownership of a slot the chatbot won't actually update — SetObjectInAttention
	// early-returns when bEnableActions is false, and stamping AttentionSource = Gaze in that
	// case would make a later real Explicit set look like a gaze-trampled state.
	if (!EnvironmentData.bEnableActions)
	{
		// [GAZE-DEBUG] Visible warning so this misconfiguration shows up in logs.
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
			TEXT("[Gaze] TrySetObjectInAttentionFromGaze rejected on '%s': EnvironmentData.bEnableActions is FALSE — enable it on the chatbot for gaze attention to take effect."),
			*GetName());
		return false;
	}

	// [GAZE-DEBUG] Verbose; bump to Log when debugging gaze-attention acceptance.
	CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
		TEXT("[Gaze] TrySetObjectInAttentionFromGaze accepted on '%s' for object='%s' source=%s"),
		*GetName(), *AttentionObject.Name,
		AttentionSource == EConvaiAttentionSource::None ? TEXT("None") :
		AttentionSource == EConvaiAttentionSource::Gaze ? TEXT("Gaze") : TEXT("Explicit"));

	// Forward through the canonical path so all the side-effects (mirror update,
	// auto-add to Objects, hold-or-stage, schedule flush) happen exactly once and
	// stay in sync. Local preparation runs immediately even when delivery defers.
	SetObjectInAttention(AttentionObject, Text, ShouldRespond, Delivery, bAddAttentionEvent, bFlushImmediately);

	// If the cue got held, remember it came from gaze: the release re-checks
	// ownership (an Explicit owner appearing during the hold wins) and a
	// look-away can cancel it.
	if (ShouldHoldForConversation(ShouldRespond, Delivery))
	{
		HeldContext.bAttentionFromGaze = true;
	}

	// SetObjectInAttention just stamped Explicit; overwrite to Gaze so subsequent gaze
	// calls on this chatbot can continue to take/release the slot.
	AttentionSource = AttentionObject.Name.IsEmpty()
		? EConvaiAttentionSource::None
		: EConvaiAttentionSource::Gaze;
	return true;
}

bool UConvaiChatbotComponent::TryClearObjectInAttentionFromGaze(FConvaiObjectEntry ExpectedObject)
{
	// A held (not-yet-delivered) gaze attention is cancelled when the gaze moves
	// away — attention describes the present, and the player already stopped
	// looking before the cue landed. Fall through (no early return): the normal
	// clear below still needs to reset the mirror and release ownership that the
	// hold's immediate local-prep already applied.
	if (HeldContext.bHasAttention && HeldContext.bAttentionFromGaze &&
		HeldContext.AttentionObject.Name == ExpectedObject.Name)
	{
		HeldContext.DropAttention();
	}

	if (AttentionSource != EConvaiAttentionSource::Gaze)
	{
		return false;
	}

	// Defend against the "late clear" race: if attention has already moved to a
	// different object (either via another gaze target or an Explicit set), this clear
	// is stale and must be ignored. Match by Name since FConvaiObjectEntry.Ref may be
	// stale by the time the clear arrives.
	if (!ExpectedObject.Name.IsEmpty() &&
		EnvironmentData.CurrentAttentionObject.Name != ExpectedObject.Name)
	{
		return false;
	}

	// Route through SetObjectInAttention with an empty entry — same path BP uses to clear,
	// keeps debounce/flush semantics consistent. Note this would stamp source = None;
	// we leave it as that since the slot is genuinely unowned now.
	SetObjectInAttention(FConvaiObjectEntry(), TEXT(""), EC_RunLLMOption::Never, EConvaiContextDelivery::SendNormally, /*bAddAttentionEvent=*/ false, /*bFlushImmediately=*/ false);
	return true;
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
	
	// Auto-spawn UConvaiObjectComponents on the actors referenced by Environment.Objects that
	// don't already carry one. Has to happen after BP construction (so EnvironmentData.Objects
	// is populated) but before the player can possibly gaze, hence here in BeginPlay.
	EnsureObjectComponentsForEnvironmentObjects();

	// Initialize session if auto-initialize is enabled — but DEFER it one frame. Actor and
	// component BeginPlay order within a frame is not deterministic, so a UConvaiObjectComponent
	// placed in the level may not have registered with the subsystem yet when this chatbot's
	// BeginPlay runs. Connecting synchronously here would build the connect action_config before
	// those objects exist, and they'd miss the initial object list (only arriving afterwards via
	// update-scene-metadata, never as action targets). Waiting until next tick lets every
	// level-placed object finish BeginPlay/registration first, so StartSession's object gather
	// sees the full set. (A manual StartSession() call is unaffected — that's the caller's timing.)
	if (bAutoInitializeSession)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				// Re-check at fire time: the flag may have been cleared, or a session may have
				// been started manually in the intervening frame — don't stomp it.
				if (bAutoInitializeSession && !IsValid(SessionProxyInstance))
				{
					CONVAI_LOG(ConvaiChatbotComponentLog, Log,
						TEXT("Auto Initializing Session (deferred one frame so level objects register first)"));
					StartSession();
				}
			}));
		}
	}
}

int32 UConvaiChatbotComponent::EnsureObjectComponentsForEnvironmentObjects()
{
	int32 SpawnedCount = 0;
	for (const FConvaiObjectEntry& Entry : EnvironmentData.Objects)
	{
		// Movement-point sub-objects are projections of their base object.
		// Materializing one as a real component would re-register it as an
		// independent object — colliding with its own generated name.
		if (Entry.bIsMovementPointSubObject)
		{
			continue;
		}
		AActor* RefActor = Entry.Ref.Get();
		if (!IsValid(RefActor))
		{
			// Skip entries that aren't bound to a world actor yet — the user may not have
			// wired up Ref, or the actor was destroyed. Nothing useful we can do here.
			continue;
		}

		// Skip only when the actor already carries an ObjComp that matches this entry's scope.
		// Multiple entries pointing at the same actor with different ComponentName filters each
		// get their own ObjComp. If the match is another chatbot's auto-spawned ObjComp, share
		// cleanup ownership so the first chatbot to end play does not destroy a component that
		// later chatbots still rely on.
		if (UConvaiObjectComponent* CoveringObjComp = FindCoveringObjectComponentOnActor(Entry, RefActor))
		{
			if (HasAutoSpawnCleanupOwner(this, CoveringObjComp))
			{
				OwnedAutoSpawnComponents.AddUnique(CoveringObjComp);
			}
			continue;
		}

		// Spawn an ObjComp with default subsystem registration so the shared poll clock picks
		// it up (proximity, tracked-property change detection). Every chatbot in the scene
		// sees every ObjComp through the subsystem pool — simpler than per-chatbot subscriptions.
		// We still track our own spawns in OwnedAutoSpawnComponents so EndPlay can
		// destroy them and avoid leaking auto-spawned components onto actors we don't own.
		UConvaiObjectComponent* NewObjComp = NewObject<UConvaiObjectComponent>(RefActor);
		if (!NewObjComp)
		{
			continue;
		}
		NewObjComp->ObjectEntry = Entry;
		NewObjComp->ObjectEntry.Ref = RefActor;  // make sure Ref is bound even if the source had a stale weak ptr
		// Defensive clear of the copied ResolvedComponent cache. ResolveComponent
		// revalidates owner + name on every call, so a carried-over wrong-actor cache
		// could never short-circuit — this just keeps the fresh ObjComp from holding a
		// pointer it has no claim to until its first resolution.
		NewObjComp->ObjectEntry.ResolvedComponent.Reset();
		NewObjComp->RegisterComponent();
		OwnedAutoSpawnComponents.Add(NewObjComp);
		++SpawnedCount;
	}
	return SpawnedCount;
}

UConvaiObjectComponent* UConvaiChatbotComponent::FindCoveringObjectComponentOnActor(
	const FConvaiObjectEntry& Entry,
	AActor* Actor) const
{
	if (!Actor)
	{
		return nullptr;
	}

	// Resolve our entry's targeted component on a *copy* so the const reference's cache stays
	// untouched. ResolveComponent revalidates the copied cache against Actor (owner + name
	// filter), so a cache carried over from a different actor or a stale prior call rescans.
	FConvaiObjectEntry EntryCopy = Entry;
	EntryCopy.Ref = Actor;
	USceneComponent* OurResolved = EntryCopy.ResolveComponent();

	TArray<UConvaiObjectComponent*> Existing;
	Actor->GetComponents<UConvaiObjectComponent>(Existing);

	for (UConvaiObjectComponent* X : Existing)
	{
		if (!IsValid(X))
		{
			continue;
		}

		// Scope match: both resolved to the same UPrimitiveComponent (or both unresolved,
		// i.e. both whole-actor scope). Pointer equality is fine since both call into
		// FConvaiObjectEntry::ResolveComponent on the same actor. ComponentName drives
		// the resolution, so this is implicitly a ComponentName-equivalence check. Any
		// registered ObjComp is reachable to every chatbot via the subsystem pool, so
		// scope match alone determines coverage.
		USceneComponent* TheirResolved = X->GetResolvedComponent();
		if (OurResolved == TheirResolved)
		{
			return X;
		}
	}

	return nullptr;
}

void UConvaiChatbotComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetActionExecution(/*bLatchShutdown*/ true);

	// Tear down any UConvaiObjectComponents we auto-spawned via
	// EnsureObjectComponentsForEnvironmentObjects — they live on OTHER actors (the Ref
	// actors from EnvironmentData.Objects) so they'd otherwise outlive the chatbot.
	// IsValid guards the case where the Ref actor died first and the component was
	// already destroyed with it.
	for (UConvaiObjectComponent* OwnedObj : OwnedAutoSpawnComponents)
	{
		if (IsValid(OwnedObj))
		{
			if (!HasOtherAutoSpawnCleanupOwner(this, OwnedObj, this))
			{
				OwnedObj->DestroyComponent();
			}
		}
	}
	OwnedAutoSpawnComponents.Reset();

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
	ResetActionExecution(/*bLatchShutdown*/ true);

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
	{
		FScopeLock AudioLock(&AudioIngressLock);
		ResetSpeechOnsetDetectionLocked();
		bForceFlushPending = false;
		IsConnectionTalking = false;
		FScopeLock FinishedLock(&FinishedTalkingLock);
		FinishedTalkingTimestamp = -1.0;
		LastRealAudioTimestamp = -1.0;
	}
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		ActiveResponseId.Reset();
		LastCompletedResponseId.Reset();
		bLegacyTurnCompleted = false;
	}
	StopVoice();
	IsConnectionTalking = false;
	bIsInterrupted = false;
	ClearThinking();
	EnvironmentSnapshotAtConnect.Reset();
	if (IsInGameThread())
	{
		ResetActionExecution(/*bLatchShutdown*/ false);
		return;
	}

	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
	AsyncTask(ENamedThreads::GameThread, [WeakSelf, ExpectedEpoch]
	{
		if (UConvaiChatbotComponent* Self = WeakSelf.Get();
			Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
		{
			Self->ResetActionExecution(/*bLatchShutdown*/ false);
		}
	});
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
	if (EnvironmentData.bEnableActions)
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
	TArray<FConvaiResultAction> Copy = ReceivedSequenceOfActions;
	if (IsInGameThread())
	{
		ProcessActionSequenceOnGameThread(MoveTemp(Copy));
		return;
	}

	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	const uint64 ExpectedEpoch = ActionLifecycleEpoch.Load();
	AsyncTask(ENamedThreads::GameThread,
		[WeakSelf, ExpectedEpoch, Copy = MoveTemp(Copy)]() mutable
	{
		if (UConvaiChatbotComponent* Self = WeakSelf.Get();
			Self && Self->ActionLifecycleEpoch.Load() == ExpectedEpoch)
		{
			Self->ProcessActionSequenceOnGameThread(MoveTemp(Copy));
		}
	});
}

void UConvaiChatbotComponent::ProcessActionSequenceOnGameThread(
	TArray<FConvaiResultAction> ReceivedSequenceOfActions)
{
	check(IsInGameThread());
	if (bActionLifecycleShuttingDown)
	{
		return;
	}

	TArray<FConvaiResultAction> BroadcastCopy;
	int32 LastCancelMarker = INDEX_NONE;
	if (WasBuiltInActionAdvertised(CancelActionPlanActionName))
	{
		for (int32 i = 0; i < ReceivedSequenceOfActions.Num(); ++i)
		{
			if (ReceivedSequenceOfActions[i].Action.Equals(
				CancelActionPlanActionName, ESearchCase::IgnoreCase))
			{
				LastCancelMarker = i;
			}
		}
	}

	if (LastCancelMarker != INDEX_NONE)
	{
		if (UConvaiDebugSubsystem* Dbg = UConvaiDebugSubsystem::GetActive(this))
		{
			for (const FConvaiResultAction& Action : ReceivedSequenceOfActions)
			{
				Dbg->OnActionDebug.Broadcast(this,
					Action.ActionString.IsEmpty() ? Action.Action : Action.ActionString,
					EConvaiActionDebugPhase::Received, FString());
			}
		}
		TArray<FConvaiResultAction> Replacement;
		for (int32 i = LastCancelMarker + 1; i < ReceivedSequenceOfActions.Num(); ++i)
		{
			Replacement.Add(MoveTemp(ReceivedSequenceOfActions[i]));
		}
		// The aggregate delegate is observe-only, but it should still reflect the
		// plan the executor accepted—not the discarded prefix or reserved control.
		BroadcastCopy = Replacement;
		RequestActionPlanCancellation(MoveTemp(Replacement));
	}
	else
	{
		BroadcastCopy = ReceivedSequenceOfActions;
		const bool bNeedsInitialDispatch =
			ActionExecutionState == EConvaiActionExecutionState::Idle ||
			ActionExecutionState == EConvaiActionExecutionState::DispatchPending;
		if (ActionExecutionState == EConvaiActionExecutionState::DispatchPending)
		{
			CancelPendingActionStart();
		}
		AppendActionsToQueue(MoveTemp(ReceivedSequenceOfActions));
		if (bNeedsInitialDispatch && ActionsQueue.Num() > 0 &&
			ActionExecutionState == EConvaiActionExecutionState::DispatchPending)
		{
			ScheduleFirstAction();
		}
	}

	// The public aggregate event observes only the accepted plan. Resolve the
	// same concrete nearest merged-object references that named action dispatch
	// receives, but re-resolve again at execution time so a moving chatbot does
	// not inherit a stale nearest representative.
	TArray<FConvaiResultAction> ResolvedBroadcastCopy = BroadcastCopy;
	for (FConvaiResultAction& Action : ResolvedBroadcastCopy)
	{
		ConvaiMergedObjectNavigation::ResolveActionObjectReferences(*this, Action);
	}
	OnActionReceivedEvent_V2.Broadcast(this, nullptr, ResolvedBroadcastCopy);
}

void UConvaiChatbotComponent::ScheduleFirstAction()
{
	check(IsInGameThread());
	if (bActionLifecycleShuttingDown || ActionsQueue.Num() == 0 ||
		ActionExecutionState == EConvaiActionExecutionState::Running ||
		ActionExecutionState == EConvaiActionExecutionState::Cancelling)
	{
		return;
	}

	CancelPendingActionStart();
	ActionExecutionState = EConvaiActionExecutionState::DispatchPending;
	const uint64 ExpectedGeneration = ActionDispatchGeneration;
	const FConvaiResultAction& First = ActionsQueue[0];
	if (!First.bWaitForBotSpeech)
	{
		StartFirstAction();
		return;
	}

	bWaitingForBotSpeechTrigger = true;
	PendingActionPostSpeechDelay = First.DelayAfterBotSpeechSec;
	PublishedActionWaitGeneration.Store(ExpectedGeneration);
	if (UWorld* World = GetWorld())
	{
		const float TimeoutSec = FMath::Max(0.0f, ActionWaitForBotSpeechTimeoutSec);
		World->GetTimerManager().SetTimer(ActionWaitTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, ExpectedGeneration]
			{
				if (ActionDispatchGeneration == ExpectedGeneration)
				{
					FlushPendingActionWait();
				}
			}),
			TimeoutSec, false);
	}
	if (IsConnectionTalking)
	{
		FlushPendingActionWait();
	}
}
void UConvaiChatbotComponent::FlushPendingActionWait()
{
	check(IsInGameThread());
	if (!bWaitingForBotSpeechTrigger ||
		ActionExecutionState != EConvaiActionExecutionState::DispatchPending)
	{
		return;
	}
	bWaitingForBotSpeechTrigger = false;
	PublishedActionWaitGeneration.Store(0);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionWaitTimerHandle);
	}

	const float Delay = FMath::Max(0.0f, PendingActionPostSpeechDelay);
	PendingActionPostSpeechDelay = 0.0f;
	const uint64 ExpectedGeneration = ActionDispatchGeneration;

	if (Delay > 0.0f && IsValid(GetWorld()))
	{
		GetWorld()->GetTimerManager().ClearTimer(PostSpeechDelayTimerHandle);
		GetWorld()->GetTimerManager().SetTimer(PostSpeechDelayTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, ExpectedGeneration]
			{
				if (ActionDispatchGeneration == ExpectedGeneration &&
					ActionExecutionState == EConvaiActionExecutionState::DispatchPending)
				{
					StartFirstAction();
				}
			}),
			Delay, false);
	}
	else
	{
		StartFirstAction();
	}
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

void UConvaiChatbotComponent::ForwardIncomingAudioChunk(const uint8* AudioData, const size_t NumFrames,
	const uint32 SampleRate, const uint32 BitsPerSample, const uint32 NumChannels)
{
	if (AudioData == nullptr || NumFrames == 0 || SampleRate == 0 || NumChannels == 0
		|| BitsPerSample == 0 || BitsPerSample % 8 != 0)
	{
		return;
	}

	const uint64 BytesPerSample = BitsPerSample / 8;
	const uint64 TotalBytes = static_cast<uint64>(NumFrames) * NumChannels * BytesPerSample;
	if (TotalBytes == 0 || TotalBytes > MAX_uint32)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Warning,
			TEXT("Ignoring invalid incoming audio chunk (%llu bytes)"), TotalBytes);
		return;
	}

	TotalAudioFramesReceived += static_cast<uint32>(NumFrames);
	HandleAudioReceived(const_cast<uint8*>(AudioData), static_cast<uint32>(TotalBytes), false,
		SampleRate, NumChannels);
}

void UConvaiChatbotComponent::ResetSpeechOnsetDetectionLocked()
{
	bAwaitingBotSpeechStart = false;
	PreStartLastAudioFrameStats = FConvaiAudioFrameStats();
}

void UConvaiChatbotComponent::HandleSpeechStartedSideEffects()
{
	ClearThinking();
	const uint64 ExpectedWaitGeneration = PublishedActionWaitGeneration.Load();
	if (ExpectedWaitGeneration == 0)
	{
		return;
	}
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf, ExpectedWaitGeneration]
	{
		if (UConvaiChatbotComponent* Self = WeakSelf.Get())
		{
			if (Self->PublishedActionWaitGeneration.Load() == ExpectedWaitGeneration &&
				Self->ActionDispatchGeneration == ExpectedWaitGeneration)
			{
				Self->FlushPendingActionWait();
			}
		}
	});
}

void UConvaiChatbotComponent::OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels)
{
	if (AudioData == nullptr || NumFrames == 0 || SampleRate == 0 || NumChannels == 0
		|| BitsPerSample == 0 || BitsPerSample % 8 != 0)
		return;

	FScopeLock AudioLock(&AudioIngressLock);

	if (bIsInterrupted)
		return;

	if (!IsConnectionTalking)
	{
		double FT_Local;
		double LRA_Local;
		{
			FScopeLock Lock(&FinishedTalkingLock);
			FT_Local  = FinishedTalkingTimestamp;
			LRA_Local = LastRealAudioTimestamp;
		}

		if (FT_Local < 0.0)
		{
			// PCM callbacks continue forever, including idle silence. Only analyze
			// and retain an onset while bot-llm-started has armed a response.
			if (!bAwaitingBotSpeechStart)
			{
				return;
			}

			FConvaiAudioFrameStats CurrentStats;
			const bool bContainsActualAudio = UConvaiUtils::ContainsActualAudio(
				AudioData, NumFrames, NumChannels, PreStartLastAudioFrameStats, CurrentStats);
			PreStartLastAudioFrameStats = CurrentStats;

			if (bContainsActualAudio)
			{
				CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
					TEXT("Actual PCM preceded bot-started-speaking; opening local speech gate"));
				IsConnectionTalking = true;
				ResetSpeechOnsetDetectionLocked();
				{
					FScopeLock FinishedLock(&FinishedTalkingLock);
					FinishedTalkingTimestamp = -1.0;
					LastRealAudioTimestamp = -1.0;
				}
				ForwardIncomingAudioChunk(reinterpret_cast<const uint8*>(AudioData), NumFrames,
					SampleRate, BitsPerSample, NumChannels);
				HandleSpeechStartedSideEffects();
			}
			return;
		}

		const double Now = FPlatformTime::Seconds();

		// All destructive writes below guard on `FinishedTalkingTimestamp ==
		// FT_Local`. If the game thread fired a new OnStartedTalking /
		// OnFinishedTalking between our snapshot and the write, FT_Local is
		// stale and a blind write would clobber the new tail's freshly-set
		// timestamp. The TOCTOU guard makes the writes conditional on the
		// snapshot still being current.

		// Hard cap (when enabled): once this many seconds have elapsed since
		// OnFinishedTalking, flush regardless of analyser verdict. Disabled
		// when MaxTailExtension < 0.
		if (MaxTailExtension > 0.0 && (Now - FT_Local) >= MaxTailExtension)
		{
			bool bGuardMatched = false;
			{
				FScopeLock Lock(&FinishedTalkingLock);
				if (FinishedTalkingTimestamp == FT_Local)
				{
					FinishedTalkingTimestamp  = -1.0;
					LastRealAudioTimestamp    = -1.0;
					bGuardMatched = true;
				}
			}
			// Only queue the force-flush when our snapshot was still current.
			// If the era changed between snapshot and write, a stale Force=true
			// can drain new-era audio out of the ring buffer below the
			// MinBufferDuration floor — let the new tail manage its own pacing.
			if (bGuardMatched)
			{
				bForceFlushPending = true;
			}
			return;
		}

		// Analyse every tail-frame so the cross-frame smoothing in
		// ContainsActualAudio sees true neighbouring frames. Per-frame cost
		// is trivial (~256-512 int16 samples per 16 ms tick).
		FConvaiAudioFrameStats CurrentStats;
		const bool bRealAudioInTail = UConvaiUtils::ContainsActualAudio(
			AudioData, NumFrames, NumChannels,
			LastAudioFrameStats, CurrentStats);
		LastAudioFrameStats = CurrentStats;

		if (bRealAudioInTail)
		{
			// Real audio extends the tail: debounce by stamping "last real
			// audio = now". Skip the stamp if FT changed under our feet —
			// the new tail's epoch is already setting its own LRA.
			FScopeLock Lock(&FinishedTalkingLock);
			if (FinishedTalkingTimestamp == FT_Local)
			{
				LastRealAudioTimestamp = Now;
			}
			// Frame passes through to normal processing regardless.
		}
		else
		{
			// Silence frame. Measure silence-since-real-audio (or
			// silence-since-OnFinishedTalking when no real audio has been
			// observed yet — LRA defaults to -1 so the max() falls back to
			// FT). Flush once it crosses StutterGraceWindow.
			const double SilenceAnchor = FMath::Max(LRA_Local, FT_Local);
			if ((Now - SilenceAnchor) >= StutterGraceWindow)
			{
				bool bGuardMatched = false;
				{
					FScopeLock Lock(&FinishedTalkingLock);
					if (FinishedTalkingTimestamp == FT_Local)
					{
						FinishedTalkingTimestamp  = -1.0;
						LastRealAudioTimestamp    = -1.0;
						bGuardMatched = true;
					}
				}
				// See hard-cap branch — only force-flush when the snapshot is
				// still current, otherwise a stale Force=true could starve the
				// new tail's MinBufferDuration window.
				if (bGuardMatched)
				{
					bForceFlushPending = true;
				}
				return;
			}
			// Within grace — fall through.
		}
	}

	ForwardIncomingAudioChunk(reinterpret_cast<const uint8*>(AudioData), NumFrames,
		SampleRate, BitsPerSample, NumChannels);
}

void UConvaiChatbotComponent::OnStartedTalking()
{
	{
		FScopeLock AudioLock(&AudioIngressLock);
		IsConnectionTalking = true;
		ResetSpeechOnsetDetectionLocked();
		{
			FScopeLock Lock(&FinishedTalkingLock);
			FinishedTalkingTimestamp  = -1.0;
			LastRealAudioTimestamp    = -1.0;
		}
	}
	HandleSpeechStartedSideEffects();
}

void UConvaiChatbotComponent::OnStartedTalkingForResponse(const FString& ResponseId)
{
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		if (!ResponseId.IsEmpty() && LastCompletedResponseId == ResponseId)
		{
			return;
		}
		if (!ResponseId.IsEmpty() && !ActiveResponseId.IsEmpty() && ResponseId != ActiveResponseId)
		{
			CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
				TEXT("Ignoring bot-started-speaking for stale response %s (active: %s)"),
				*ResponseId, *ActiveResponseId);
			return;
		}
		// A legacy marker without response_id must not erase the owner learned
		// from bot-llm-started.
		if (!ResponseId.IsEmpty())
		{
			ActiveResponseId = ResponseId;
		}
		bLegacyTurnCompleted = false;
	}
	OnStartedTalking();
}

void UConvaiChatbotComponent::OnFinishedTalking()
{
	{
		FScopeLock AudioLock(&AudioIngressLock);
		ResetSpeechOnsetDetectionLocked();
		IsConnectionTalking = false;
		{
			FScopeLock Lock(&FinishedTalkingLock);
			FinishedTalkingTimestamp  = FPlatformTime::Seconds();
			LastRealAudioTimestamp    = -1.0;
		}
	}
	// Also ends any thinking window: llm-no-response ("the server declined to
	// answer") is routed here by the subsystem, and a normal speech end must not
	// leave a stale window open either.
	ClearThinking();
	const uint64 ExpectedWaitGeneration = PublishedActionWaitGeneration.Load();
	if (ExpectedWaitGeneration == 0)
	{
		return;
	}
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread, [WeakSelf, ExpectedWaitGeneration]
	{
		if (UConvaiChatbotComponent* Self = WeakSelf.Get())
		{
			if (Self->PublishedActionWaitGeneration.Load() == ExpectedWaitGeneration &&
				Self->ActionDispatchGeneration == ExpectedWaitGeneration)
			{
				Self->FlushPendingActionWait();
			}
		}
	});
}

void UConvaiChatbotComponent::OnFinishedTalkingForResponse(const FString& ResponseId)
{
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		if (!ResponseId.IsEmpty() && LastCompletedResponseId == ResponseId)
		{
			return;
		}
		if (!ResponseId.IsEmpty() && !ActiveResponseId.IsEmpty() && ResponseId != ActiveResponseId)
		{
			CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
				TEXT("Ignoring bot-stopped-speaking for stale response %s (active: %s)"),
				*ResponseId, *ActiveResponseId);
			return;
		}
		if (ActiveResponseId.IsEmpty() && !ResponseId.IsEmpty())
		{
			ActiveResponseId = ResponseId;
		}
	}
	OnFinishedTalking();
}

void UConvaiChatbotComponent::OnBotTurnCompleted(const FString& ResponseId,
	const bool bWasInterrupted, const bool bWasAborted, const FString& ErrorReason)
{
	const bool bCancellationTerminal = bWasInterrupted || bWasAborted;
	bool bApplyToCurrentResponse = true;
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		if ((!ResponseId.IsEmpty() && LastCompletedResponseId == ResponseId)
			|| (ResponseId.IsEmpty() && bLegacyTurnCompleted))
		{
			return;
		}

		if (ResponseId.IsEmpty())
		{
			bLegacyTurnCompleted = true;
			// Without an owner, an interruption must not stop identified media.
			bApplyToCurrentResponse = !bCancellationTerminal || ActiveResponseId.IsEmpty();
		}
		else
		{
			LastCompletedResponseId = ResponseId;
			bApplyToCurrentResponse = bCancellationTerminal
				? (!ActiveResponseId.IsEmpty() && ResponseId == ActiveResponseId)
				: (ActiveResponseId.IsEmpty() || ResponseId == ActiveResponseId);
		}
	}

	if (!bApplyToCurrentResponse)
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Verbose,
			TEXT("Received terminal for a response that does not own current media: %s"),
			*ResponseId);
	}
	else if (bCancellationTerminal)
	{
		{
			FScopeLock AudioLock(&AudioIngressLock);
			ResetSpeechOnsetDetectionLocked();
			bForceFlushPending = false;
			IsConnectionTalking = false;
			FScopeLock FinishedLock(&FinishedTalkingLock);
			FinishedTalkingTimestamp = -1.0;
			LastRealAudioTimestamp = -1.0;
		}
		StopVoice();
	}
	else
	{
		FScopeLock AudioLock(&AudioIngressLock);
		ResetSpeechOnsetDetectionLocked();
		bForceFlushPending = true;

		bool bSpeechStopWasSeen = false;
		{
			FScopeLock FinishedLock(&FinishedTalkingLock);
			bSpeechStopWasSeen = FinishedTalkingTimestamp >= 0.0;
		}

		if (!bSpeechStopWasSeen)
		{
			// Turn completion is a fallback for a missing speaking-state event,
			// not a local-playback boundary. Arm the analyser-driven tail grace
			// for any RTP callbacks already in flight, rather than accepting the
			// transport's endless idle silence.
			IsConnectionTalking = false;
			FScopeLock FinishedLock(&FinishedTalkingLock);
			FinishedTalkingTimestamp = FPlatformTime::Seconds();
			LastRealAudioTimestamp = -1.0;
		}
	}

	if (bApplyToCurrentResponse)
	{
		ClearThinking();
	}
	const uint64 ExpectedWaitGeneration = PublishedActionWaitGeneration.Load();
	TWeakObjectPtr<UConvaiChatbotComponent> WeakSelf(this);
	AsyncTask(ENamedThreads::GameThread,
		[WeakSelf, ResponseId, bWasInterrupted, bWasAborted, ErrorReason,
			bApplyToCurrentResponse, ExpectedWaitGeneration]
		{
			if (UConvaiChatbotComponent* Self = WeakSelf.Get())
			{
				if (bApplyToCurrentResponse && ExpectedWaitGeneration != 0 &&
					Self->PublishedActionWaitGeneration.Load() == ExpectedWaitGeneration &&
					Self->ActionDispatchGeneration == ExpectedWaitGeneration)
				{
					Self->FlushPendingActionWait();
				}
				Self->OnBotTurnCompletedEvent.Broadcast(Self, ResponseId,
					bWasInterrupted, bWasAborted, ErrorReason);
			}
		});
}

void UConvaiChatbotComponent::OnLLMStarted()
{
	// THE thinking signal: bot-llm-started means the server is actually
	// generating a response for this session — the only ground truth for
	// "Is Thinking". Closed by bot speech, llm-no-response, or staleness.
	MarkThinkingStarted();
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		bLegacyTurnCompleted = false;
	}
	{
		FScopeLock AudioLock(&AudioIngressLock);
		ResetSpeechOnsetDetectionLocked();
		bForceFlushPending = false;
		MarkEndOfAudio();
		StopVoice();
		IsConnectionTalking = false;
		bAwaitingBotSpeechStart = true;
		FScopeLock Lock(&FinishedTalkingLock);
		FinishedTalkingTimestamp  = -1.0;
		LastRealAudioTimestamp    = -1.0;
	}
}

void UConvaiChatbotComponent::OnLLMStartedForResponse(const FString& ResponseId)
{
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		ActiveResponseId = ResponseId;
		bLegacyTurnCompleted = false;
	}
	OnLLMStarted();
}

void UConvaiChatbotComponent::OnLLMStopped()
{
}

void UConvaiChatbotComponent::OnInterrupt()
{
	bIsInterrupted = true;
	{
		FScopeLock AudioLock(&AudioIngressLock);
		ResetSpeechOnsetDetectionLocked();
		bForceFlushPending = false;
		FScopeLock FinishedLock(&FinishedTalkingLock);
		FinishedTalkingTimestamp = -1.0;
		LastRealAudioTimestamp = -1.0;
	}
	InterruptSpeech(0);
	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Character interrupted by user speaking"));
}

void UConvaiChatbotComponent::OnInterruptEnd()
{
	bIsInterrupted = false;
	// Deliberately does NOT open the thinking window: a user turn ending is no
	// promise that THIS character (or any character) will respond — in
	// multiplayer / multi-character scenes the user may be talking to someone
	// else entirely. Thinking starts only on the true signal, bot-llm-started
	// (OnLLMStarted). The brief quiet between turns is covered, for held
	// delivery, by the Quiet Time Before Delivery gate.
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

	CONVAI_LOG(ConvaiAudioStreamerLog, Log, TEXT("MinBufferDuration = %f"), MinBufferDuration);

	if (IsValid(SessionProxyInstance))
	{
		StopSession();
	}
	++ActionLifecycleEpoch;
	bActionLifecycleShuttingDown = false;
	if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UConvaiContextSubsystem* ContextSubsystem =
			GameInstance->GetSubsystem<UConvaiContextSubsystem>())
		{
			ContextSubsystem->ResetObserverSpatialContext(this);
		}
	}

	// Auto-fill conversation partner before the BP virtual fires so designers can inspect
	// or override the auto-filled value from GatherEnvironmentExtras.
	if (bAutoFillConversationPartnerFromPlayer)
	{
		FConvaiObjectEntry Partner;
		AActor* PartnerOwner = nullptr;
		if (UConvaiPlayerComponent* PC = UConvaiUtils::GetFirstConvaiPlayerComponent(this, /*bOnlyConnected*/ false, PartnerOwner))
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

	// Auto-gather every UConvaiObjectComponent currently in the world. This is the canonical
	// path for "drop a ConvaiObjectComponent on a door, the chatbot just knows about it"
	// — no per-chatbot wiring needed. Initial tracked-property values are seeded with
	// EC_RunLLMOption::Never so session boot doesn't pile RunLLM calls.
	if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
	{
		for (UConvaiObjectComponent* ObjectComponent : ConvaiSubsystem->GetAllObjectComponents())
		{
			if (!IsValid(ObjectComponent) || !ObjectComponent->HasValidObjectName())
			{
				continue;
			}
			AddOrUpdateObjectFromComponent(ObjectComponent);
			ObjectComponent->SeedInitialStateOntoChatbot(this);
		}
	}

	SessionProxyInstance = UConvaiConnectionSessionProxy::RequestSessionProxy(this, CharacterID, this);

	if (!IsValid(SessionProxyInstance))
	{
		CONVAI_LOG(ConvaiChatbotComponentLog, Error, TEXT("Failed to acquire connection for character ID: %s"), *CharacterID);
		BroadcastConnectionStateChanged("", EC_ConnectionState::Disconnected);
		return;
	}

	// Pull the advertised-action contract from the proxy — the contract belongs
	// to the CONNECTION (published by the subsystem when the /connect payload
	// was built, before the connection thread started) and survives orphan
	// rebinds. For a FRESH connect this re-reads what GetActionConfigJson just
	// froze; for a WARM (orphan-rebound) proxy it adopts the PREVIOUS owner's
	// contract so built-in dispatch honors what the server actually knows.
	// Packet parsing doesn't depend on this pull — it reads the proxy contract
	// directly — and dispatch can't run before StartSession returns (both are
	// game-thread), so there's no window where either sees stale state.
	if (SessionProxyInstance->HasAdvertisedActions())
	{
		SessionProxyInstance->GetAdvertisedActions(ConnectedActionsSnapshot, ConnectedBuiltInNames);
		bHasConnectedActionsSnapshot = true;
	}

	// Tell the model when to use only the built-ins this connection actually
	// advertised. The derived Instructions block joins a silent canonical
	// Replace and supersedes the legacy stable-fact hints from older builds.
	SyncManagedActionContext();

	CONVAI_LOG(ConvaiChatbotComponentLog, Log, TEXT("Session started for character ID: %s"), *CharacterID);
}

void UConvaiChatbotComponent::StopSession()
{
	ResetActionExecution(/*bLatchShutdown*/ true);
	{
		FScopeLock AudioLock(&AudioIngressLock);
		ResetSpeechOnsetDetectionLocked();
		bForceFlushPending = false;
		IsConnectionTalking = false;
		FScopeLock FinishedLock(&FinishedTalkingLock);
		FinishedTalkingTimestamp = -1.0;
		LastRealAudioTimestamp = -1.0;
	}
	{
		FScopeLock LifecycleLock(&ResponseLifecycleLock);
		ActiveResponseId.Reset();
		LastCompletedResponseId.Reset();
		bLegacyTurnCompleted = false;
	}

	if (IsValid(SessionProxyInstance))
	{
		UConvaiConnectionSessionProxy::ReturnSessionProxy(this, CharacterID, SessionProxyInstance);
		SessionProxyInstance = nullptr;
		// The proxy may linger in an orphan grace period, so OnDisconnectedFromServer
		// isn't guaranteed to fire promptly — clear conversation state here too.
		IsConnectionTalking = false;
		bIsInterrupted = false;
		ClearThinking();
		// Explicit session stop retires armed watches; transient reconnects
		// (which don't pass through here) deliberately preserve them.
		WatchedContextKeys.Empty();
		TargetWatchesWithObservedDeparture.Empty();
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
