// Copyright 2022 Convai Inc. All Rights Reserved.

#include "DynamicContext/ConvaiPendingContextBatch.h"
#include "DynamicContext/ConvaiDynamicContextTracker.h"

namespace
{
	int32 RunLLMRank(EC_RunLLMOption Option)
	{
		switch (Option)
		{
			case EC_RunLLMOption::Always: return 2;
			case EC_RunLLMOption::Auto:   return 1;
			default:                      return 0; // Never
		}
	}
}

bool FConvaiPendingContextBatch::IsEmpty() const
{
	return StagedStateOrder.IsEmpty() && StagedDeclarativeOrder.IsEmpty() && EventsOrdered.IsEmpty()
		&& EphemeralEventsOrdered.IsEmpty() && !bHasPendingAttention;
}

bool FConvaiPendingContextBatch::HasWork() const
{
	return !IsEmpty() || bForceReplace || bPendingReset;
}

bool FConvaiPendingContextBatch::HasNonNever() const
{
	return AggregateRunLLM != EC_RunLLMOption::Never;
}

bool FConvaiPendingContextBatch::HasStagedWork() const
{
	return !IsEmpty() || bForceReplace;
}

void FConvaiPendingContextBatch::ClearStaged()
{
	StagedStateOrder.Empty();
	OldValues.Empty();
	InitialValues.Empty();
	PreserveTransitionKeys.Empty();
	OmitPreviousValueKeys.Empty();
	StateRespond.Empty();
	WatchPromotedStateKeys.Empty();
	StagedDeclarativeOrder.Empty();
	InitialDeclarativeValues.Empty();
	DeclarativeRespond.Empty();
	EventsOrdered.Empty();
	EventsSet.Empty();
	EventRespond.Empty();
	EphemeralEventsOrdered.Empty();
	EphemeralEventsSet.Empty();
	EphemeralEventRespond.Empty();
	AggregateRunLLM = EC_RunLLMOption::Never;
	bForceReplace = false;
	bHasPendingAttention = false;
	PendingAttentionObject = FConvaiObjectEntry();
	PendingAttentionText.Reset();
	PendingAttentionRunLLM = EC_RunLLMOption::Auto;
}

void FConvaiPendingContextBatch::Clear()
{
	ClearStaged();
	bPendingReset = false;
}

void FConvaiPendingContextBatch::MergeRunLLM(EC_RunLLMOption In)
{
	if (RunLLMRank(In) > RunLLMRank(AggregateRunLLM))
	{
		AggregateRunLLM = In;
	}
}

void FConvaiPendingContextBatch::RecomputeAggregateRunLLM()
{
	AggregateRunLLM = EC_RunLLMOption::Never;
	for (const TPair<FString, EC_RunLLMOption>& Pair : StateRespond)
	{
		MergeRunLLM(Pair.Value);
	}
	for (const TPair<FString, EC_RunLLMOption>& Pair : DeclarativeRespond)
	{
		MergeRunLLM(Pair.Value);
	}
	for (const TPair<FString, EC_RunLLMOption>& Pair : EventRespond)
	{
		MergeRunLLM(Pair.Value);
	}
	for (const TPair<FString, EC_RunLLMOption>& Pair : EphemeralEventRespond)
	{
		MergeRunLLM(Pair.Value);
	}
	if (bHasPendingAttention)
	{
		MergeRunLLM(PendingAttentionRunLLM);
	}
}

bool FConvaiPendingContextBatch::StageState(FConvaiDynamicContextTracker& Tracker,
	const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond,
	bool bOmitPreviousValue, const FString* PreviousValueOverride,
	bool bPreserveTransition)
{
	FString ImmediatePreviousValue;
	const bool bHadImmediatePrevious =
		Tracker.GetStateValue(Name, ImmediatePreviousValue);
	if (!StagedStateOrder.Contains(Name))
	{
		StagedStateOrder.Add(Name);
		if (bHadImmediatePrevious)
		{
			InitialValues.Add(Name, ImmediatePreviousValue);
			OldValues.Add(Name, ImmediatePreviousValue);
		}
		// Key absent from InitialValues means it's new this batch.
	}
	if (bPreserveTransition || PreviousValueOverride)
	{
		PreserveTransitionKeys.Add(Name);
	}
	const bool bKeepTransition = PreserveTransitionKeys.Contains(Name);
	if (PreviousValueOverride)
	{
		// A detector edge is stronger evidence than the pre-batch snapshot. In a
		// rapid Stopped -> Moving -> Stopped round trip, the final transition must
		// still read "was Moving", not the batch's original "was Stopped".
		OldValues.Add(Name, *PreviousValueOverride);
	}
	else if (bKeepTransition && bHadImmediatePrevious
		&& ImmediatePreviousValue != Value)
	{
		// Preservation is sticky. If a later ordinary write completes the
		// excursion, describe that final edge rather than reverting to the
		// original batch baseline.
		OldValues.Add(Name, ImmediatePreviousValue);
	}
	Tracker.SetState(Name, Value);
	if (!bKeepTransition)
	{
		if (const FString* OriginalValue = InitialValues.Find(Name);
			OriginalValue && *OriginalValue == Value)
		{
			// Ordinary A -> B -> A churn has no net information. Remove the staged
			// key and its response rank; the tracker already contains the final A.
			DropStateKey(Name);
			return false;
		}
	}
	if (bOmitPreviousValue)
	{
		OmitPreviousValueKeys.Add(Name);
	}
	else
	{
		OmitPreviousValueKeys.Remove(Name);
	}
	EC_RunLLMOption& Response = StateRespond.FindOrAdd(Name, EC_RunLLMOption::Never);
	if (RunLLMRank(ShouldRespond) > RunLLMRank(Response))
	{
		Response = ShouldRespond;
	}
	MergeRunLLM(ShouldRespond);
	return true;
}

void FConvaiPendingContextBatch::StageDeclarative(FConvaiDynamicContextTracker& Tracker, const FString& Key, const FString& Sentence, EC_RunLLMOption ShouldRespond)
{
	if (!StagedDeclarativeOrder.Contains(Key))
	{
		StagedDeclarativeOrder.Add(Key);
		FString InitialSentence;
		if (Tracker.GetDeclarativeValue(Key, InitialSentence))
		{
			InitialDeclarativeValues.Add(Key, InitialSentence);
		}
		// Key absent from InitialDeclarativeValues means it is new this batch.
	}
	Tracker.SetDeclarative(Key, Sentence);
	EC_RunLLMOption& Response = DeclarativeRespond.FindOrAdd(Key, EC_RunLLMOption::Never);
	if (RunLLMRank(ShouldRespond) > RunLLMRank(Response))
	{
		Response = ShouldRespond;
	}
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::StageEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bEphemeral)
{
	// Ephemeral events stage into a parallel set that the flush emits ONCE and never
	// commits to the canonical tracker; normal events stage as before.
	TArray<FString>& Ordered = bEphemeral ? EphemeralEventsOrdered : EventsOrdered;
	TSet<FString>&   Seen    = bEphemeral ? EphemeralEventsSet     : EventsSet;
	TMap<FString, EC_RunLLMOption>& Responses =
		bEphemeral ? EphemeralEventRespond : EventRespond;
	if (!Seen.Contains(Text))
	{
		Seen.Add(Text);
		Ordered.Add(Text);
	}
	EC_RunLLMOption& Response = Responses.FindOrAdd(Text, EC_RunLLMOption::Never);
	if (RunLLMRank(ShouldRespond) > RunLLMRank(Response))
	{
		Response = ShouldRespond;
	}
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::DropEvent(const FString& Text, bool bEphemeral)
{
	TArray<FString>& Ordered = bEphemeral ? EphemeralEventsOrdered : EventsOrdered;
	TSet<FString>& Seen = bEphemeral ? EphemeralEventsSet : EventsSet;
	if (Seen.Remove(Text) > 0)
	{
		Ordered.Remove(Text);
	}

	// AggregateRunLLM is intentionally monotonic inside a batch. If this was
	// the batch's final payload, however, no removed task cue may leave a
	// response request armed on an otherwise empty debounce flush.
	if (IsEmpty())
	{
		AggregateRunLLM = EC_RunLLMOption::Never;
	}
}

void FConvaiPendingContextBatch::StageAttention(const FConvaiObjectEntry& Object, const FString& Text, EC_RunLLMOption ShouldRespond)
{
	// Last-wins within the window: every call overwrites the slot.
	const bool bHadPendingAttention = bHasPendingAttention;
	bHasPendingAttention = true;
	PendingAttentionObject = Object;
	PendingAttentionText = Text;
	if (RunLLMRank(ShouldRespond) > RunLLMRank(PendingAttentionRunLLM)
		|| !bHadPendingAttention)
	{
		PendingAttentionRunLLM = ShouldRespond;
	}
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::DropStateKey(const FString& Name)
{
	StagedStateOrder.Remove(Name);
	OldValues.Remove(Name);
	InitialValues.Remove(Name);
	PreserveTransitionKeys.Remove(Name);
	OmitPreviousValueKeys.Remove(Name);
	StateRespond.Remove(Name);
	WatchPromotedStateKeys.Remove(Name);
	RecomputeAggregateRunLLM();
}

void FConvaiPendingContextBatch::DropDeclarativeKey(const FString& Name)
{
	StagedDeclarativeOrder.Remove(Name);
	InitialDeclarativeValues.Remove(Name);
	DeclarativeRespond.Remove(Name);
	RecomputeAggregateRunLLM();
}

bool FConvaiPendingContextBatch::WithdrawStateForHold(
	FConvaiDynamicContextTracker& Tracker, const FString& Name,
	bool* bOutWatchPromoted)
{
	if (bOutWatchPromoted)
	{
		*bOutWatchPromoted = false;
	}
	if (!StagedStateOrder.Contains(Name))
	{
		return false;
	}

	if (bOutWatchPromoted)
	{
		*bOutWatchPromoted = WatchPromotedStateKeys.Contains(Name);
	}
	if (const FString* InitialValue = InitialValues.Find(Name))
	{
		Tracker.SetState(Name, *InitialValue);
	}
	else
	{
		Tracker.RemoveState(Name);
	}
	DropStateKey(Name);
	return true;
}

bool FConvaiPendingContextBatch::WithdrawDeclarativeForHold(
	FConvaiDynamicContextTracker& Tracker, const FString& Name)
{
	if (!StagedDeclarativeOrder.Contains(Name))
	{
		return false;
	}

	if (const FString* InitialSentence = InitialDeclarativeValues.Find(Name))
	{
		Tracker.SetDeclarative(Name, *InitialSentence);
	}
	else
	{
		Tracker.RemoveDeclarative(Name);
	}
	DropDeclarativeKey(Name);
	return true;
}

bool FConvaiPendingContextBatch::StateRequestsResponse(const FString& Name) const
{
	const EC_RunLLMOption* Response = StateRespond.Find(Name);
	return Response && *Response != EC_RunLLMOption::Never;
}

// ── FConvaiHeldContextLane ────────────────────────────────────────────

namespace
{
	EC_RunLLMOption MaxRankRespond(EC_RunLLMOption A, EC_RunLLMOption B)
	{
		return RunLLMRank(A) >= RunLLMRank(B) ? A : B;
	}
}

bool FConvaiHeldContextLane::HasWork() const
{
	return !StateOrder.IsEmpty() || !DeclarativeOrder.IsEmpty() || !Events.IsEmpty() || bHasAttention;
}

void FConvaiHeldContextLane::Clear()
{
	StateOrder.Empty();
	StateValues.Empty();
	StateRespond.Empty();
	StateOmitPreviousValue.Empty();
	StatePreviousValueOverrides.Empty();
	StatePreserveTransitions.Empty();
	WatchPromotedStateKeys.Empty();
	DeclarativeOrder.Empty();
	DeclarativeSentences.Empty();
	DeclarativeRespond.Empty();
	Events.Empty();
	bHasAttention = false;
	AttentionObject = FConvaiObjectEntry();
	AttentionText.Reset();
	AttentionRespond = EC_RunLLMOption::Auto;
	bAttentionAddEvent = true;
	bAttentionFromGaze = false;
	bReleaseAsSoonAsIdle = false;
	IdleSinceTime = -1.0;
}

bool FConvaiHeldContextLane::UpdateIdleAndCheckRelease(bool bConversationIdle, bool bConnected, double Now, double QuietSeconds)
{
	if (!HasWork() || !bConnected || !bConversationIdle)
	{
		// Activity, a disconnect, or an empty lane all invalidate any quiet
		// stretch observed so far — the clock restarts on the next real idle.
		IdleSinceTime = -1.0;
		return false;
	}
	if (bReleaseAsSoonAsIdle)
	{
		// Urgent (a held bFlushImmediately): the first idle instant wins.
		return true;
	}
	if (IdleSinceTime < 0.0)
	{
		IdleSinceTime = Now;
	}
	return (Now - IdleSinceTime) >= FMath::Max(0.0, QuietSeconds);
}

void FConvaiHeldContextLane::HoldState(const FString& Key, const FString& Value,
	EC_RunLLMOption ShouldRespond, bool bOmitPreviousValue,
	const FString* PreviousValueOverride, bool bPreserveTransition,
	bool bWatchPromoted)
{
	FString ExistingHeldValue;
	const bool bHadExistingHeldValue =
		StateValues.RemoveAndCopyValue(Key, ExistingHeldValue);
	const bool bWasPreserved = StatePreserveTransitions.Contains(Key);
	const bool bWasWatchPromoted = WatchPromotedStateKeys.Contains(Key);
	if (const EC_RunLLMOption* Existing = StateRespond.Find(Key))
	{
		// Last value wins; response keeps the strongest rank seen — matching
		// what the ordinary batch's aggregate would have done.
		ShouldRespond = MaxRankRespond(*Existing, ShouldRespond);
	}
	else
	{
		StateOrder.Add(Key);
	}
	StateValues.Add(Key, Value);
	StateRespond.Add(Key, ShouldRespond);
	if (bOmitPreviousValue)
	{
		StateOmitPreviousValue.Add(Key);
	}
	else
	{
		StateOmitPreviousValue.Remove(Key);
	}
	if (PreviousValueOverride)
	{
		StatePreviousValueOverrides.Add(Key, *PreviousValueOverride);
	}
	else if ((bWasPreserved || bPreserveTransition) && bHadExistingHeldValue
		&& ExistingHeldValue != Value)
	{
		StatePreviousValueOverrides.Add(Key, ExistingHeldValue);
	}
	else if (!bWasPreserved && !bPreserveTransition)
	{
		StatePreviousValueOverrides.Remove(Key);
	}
	if (bWasPreserved || bPreserveTransition || PreviousValueOverride)
	{
		StatePreserveTransitions.Add(Key);
	}
	if (bWasWatchPromoted || bWatchPromoted)
	{
		WatchPromotedStateKeys.Add(Key);
		StatePreserveTransitions.Add(Key);
	}
}

void FConvaiHeldContextLane::HoldDeclarative(const FString& Key, const FString& Sentence, EC_RunLLMOption ShouldRespond)
{
	if (const EC_RunLLMOption* Existing = DeclarativeRespond.Find(Key))
	{
		ShouldRespond = MaxRankRespond(*Existing, ShouldRespond);
	}
	else
	{
		DeclarativeOrder.Add(Key);
	}
	DeclarativeSentences.Add(Key, Sentence);
	DeclarativeRespond.Add(Key, ShouldRespond);
}

int32 FConvaiHeldContextLane::IndexOfHeldEvent(const FString& Text, bool bEphemeral) const
{
	for (int32 i = 0; i < Events.Num(); ++i)
	{
		if (Events[i].bEphemeral == bEphemeral && Events[i].Text == Text)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void FConvaiHeldContextLane::HoldEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bEphemeral)
{
	// Identity is (text, ephemerality) — a persistent "X" and an ephemeral "X"
	// are distinct, as in the ordinary batch. Duplicates rank-merge the response.
	const int32 i = IndexOfHeldEvent(Text, bEphemeral);
	if (i != INDEX_NONE)
	{
		Events[i].ShouldRespond = MaxRankRespond(Events[i].ShouldRespond, ShouldRespond);
		return;
	}
	FHeldEvent Held;
	Held.Text = Text;
	Held.ShouldRespond = ShouldRespond;
	Held.bEphemeral = bEphemeral;
	Events.Add(MoveTemp(Held));
}

void FConvaiHeldContextLane::HoldAttention(const FConvaiObjectEntry& Object, const FString& Text, EC_RunLLMOption ShouldRespond, bool bAddAttentionEvent, bool bFromGaze)
{
	// Last object/text wins; response keeps the strongest rank (the ordinary
	// batch's slot is last-wins but its AGGREGATE — which drives run_llm on the
	// wire — is max-rank, and one released call carries both roles).
	AttentionRespond = bHasAttention ? MaxRankRespond(AttentionRespond, ShouldRespond) : ShouldRespond;
	bHasAttention = true;
	AttentionObject = Object;
	AttentionText = Text;
	bAttentionAddEvent = bAddAttentionEvent;
	bAttentionFromGaze = bFromGaze;
}

void FConvaiHeldContextLane::DropStateKey(
	const FString& Key, bool* bOutWatchPromoted)
{
	if (bOutWatchPromoted)
	{
		*bOutWatchPromoted = WatchPromotedStateKeys.Contains(Key);
	}
	StateOrder.Remove(Key);
	StateValues.Remove(Key);
	StateRespond.Remove(Key);
	StateOmitPreviousValue.Remove(Key);
	StatePreviousValueOverrides.Remove(Key);
	StatePreserveTransitions.Remove(Key);
	WatchPromotedStateKeys.Remove(Key);
	ResetReleaseControlsIfEmpty();
}

void FConvaiHeldContextLane::DropDeclarativeKey(const FString& Key)
{
	DeclarativeOrder.Remove(Key);
	DeclarativeSentences.Remove(Key);
	DeclarativeRespond.Remove(Key);
	ResetReleaseControlsIfEmpty();
}

void FConvaiHeldContextLane::DropEvent(const FString& Text, bool bEphemeral)
{
	const int32 i = IndexOfHeldEvent(Text, bEphemeral);
	if (i != INDEX_NONE)
	{
		Events.RemoveAt(i);
		ResetReleaseControlsIfEmpty();
	}
}

void FConvaiHeldContextLane::DropAttention()
{
	bHasAttention = false;
	AttentionObject = FConvaiObjectEntry();
	AttentionText.Reset();
	AttentionRespond = EC_RunLLMOption::Auto;
	bAttentionAddEvent = true;
	bAttentionFromGaze = false;
	ResetReleaseControlsIfEmpty();
}
