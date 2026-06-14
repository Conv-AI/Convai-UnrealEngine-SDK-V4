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
	StagedDeclarativeOrder.Empty();
	DeclarativeOldValues.Empty();
	EventsOrdered.Empty();
	EventsSet.Empty();
	EphemeralEventsOrdered.Empty();
	EphemeralEventsSet.Empty();
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

void FConvaiPendingContextBatch::StageState(FConvaiDynamicContextTracker& Tracker, const FString& Name, const FString& Value, EC_RunLLMOption ShouldRespond)
{
	if (!StagedStateOrder.Contains(Name))
	{
		StagedStateOrder.Add(Name);
		FString ExistingValue;
		if (Tracker.GetStateValue(Name, ExistingValue))
		{
			OldValues.Add(Name, ExistingValue);
		}
		// Key absent from OldValues means it's new this batch.
	}
	Tracker.SetState(Name, Value);
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::StageDeclarative(FConvaiDynamicContextTracker& Tracker, const FString& Key, const FString& Sentence, EC_RunLLMOption ShouldRespond)
{
	if (!StagedDeclarativeOrder.Contains(Key))
	{
		StagedDeclarativeOrder.Add(Key);
		FString ExistingValue;
		if (Tracker.GetDeclarativeValue(Key, ExistingValue))
		{
			DeclarativeOldValues.Add(Key, ExistingValue);
		}
		// Key absent from DeclarativeOldValues means it's new this batch.
	}
	Tracker.SetDeclarative(Key, Sentence);
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::StageEvent(const FString& Text, EC_RunLLMOption ShouldRespond, bool bEphemeral)
{
	// Ephemeral events stage into a parallel set that the flush emits ONCE and never
	// commits to the canonical tracker; normal events stage as before.
	TArray<FString>& Ordered = bEphemeral ? EphemeralEventsOrdered : EventsOrdered;
	TSet<FString>&   Seen    = bEphemeral ? EphemeralEventsSet     : EventsSet;
	if (!Seen.Contains(Text))
	{
		Seen.Add(Text);
		Ordered.Add(Text);
	}
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::StageAttention(const FConvaiObjectEntry& Object, const FString& Text, EC_RunLLMOption ShouldRespond)
{
	// Last-wins within the window: every call overwrites the slot.
	bHasPendingAttention = true;
	PendingAttentionObject = Object;
	PendingAttentionText = Text;
	PendingAttentionRunLLM = ShouldRespond;
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::DropStateKey(const FString& Name)
{
	StagedStateOrder.Remove(Name);
	OldValues.Remove(Name);
}

void FConvaiPendingContextBatch::DropDeclarativeKey(const FString& Name)
{
	StagedDeclarativeOrder.Remove(Name);
	DeclarativeOldValues.Remove(Name);
}
