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
	return StagedStateOrder.IsEmpty() && EventsOrdered.IsEmpty();
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
	EventsOrdered.Empty();
	EventsSet.Empty();
	AggregateRunLLM = EC_RunLLMOption::Never;
	bForceReplace = false;
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

void FConvaiPendingContextBatch::StageEvent(const FString& Text, EC_RunLLMOption ShouldRespond)
{
	if (!EventsSet.Contains(Text))
	{
		EventsSet.Add(Text);
		EventsOrdered.Add(Text);
	}
	MergeRunLLM(ShouldRespond);
}

void FConvaiPendingContextBatch::DropStateKey(const FString& Name)
{
	StagedStateOrder.Remove(Name);
	OldValues.Remove(Name);
}
