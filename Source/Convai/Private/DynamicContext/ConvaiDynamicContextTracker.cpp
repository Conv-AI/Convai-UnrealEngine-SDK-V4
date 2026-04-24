// Copyright 2022 Convai Inc. All Rights Reserved.

#include "DynamicContext/ConvaiDynamicContextTracker.h"

bool FConvaiDynamicContextTracker::SetState(const FString& Name, const FString& Value, FString* OutOldValue)
{
	if (FString* Existing = StateValues.Find(Name))
	{
		if (OutOldValue)
		{
			*OutOldValue = *Existing;
		}
		*Existing = Value;
		return true; // existed before
	}

	// New entry
	StateKeyOrder.Add(Name);
	StateValues.Add(Name, Value);
	return false;
}

bool FConvaiDynamicContextTracker::RemoveState(const FString& Name)
{
	if (StateValues.Remove(Name) > 0)
	{
		StateKeyOrder.Remove(Name);
		return true;
	}
	return false;
}

bool FConvaiDynamicContextTracker::GetStateValue(const FString& Name, FString& OutValue) const
{
	if (const FString* Found = StateValues.Find(Name))
	{
		OutValue = *Found;
		return true;
	}
	return false;
}

void FConvaiDynamicContextTracker::AddEvent(const FString& Text)
{
	Events.Add(Text);
}

FString FConvaiDynamicContextTracker::BuildCanonicalContext() const
{
	TArray<FString> Lines;
	Lines.Reserve(StateKeyOrder.Num() + Events.Num());

	for (const FString& Key : StateKeyOrder)
	{
		if (const FString* Val = StateValues.Find(Key))
		{
			Lines.Add(FString::Printf(TEXT("%s is %s"), *Key, **Val));
		}
	}

	for (const FString& Event : Events)
	{
		Lines.Add(Event);
	}

	return FString::Join(Lines, TEXT("\n"));
}

void FConvaiDynamicContextTracker::Reset()
{
	StateKeyOrder.Empty();
	StateValues.Empty();
	Events.Empty();
}
