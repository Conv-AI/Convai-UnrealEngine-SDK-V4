// Copyright 2022 Convai Inc. All Rights Reserved.

#include "DynamicContext/ConvaiDynamicContextTracker.h"
#include "Utility/ConvaiContextFormat.h"

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

bool FConvaiDynamicContextTracker::SetDeclarative(const FString& Key, const FString& Sentence, FString* OutOldValue)
{
	if (FString* Existing = DeclarativeValues.Find(Key))
	{
		if (OutOldValue)
		{
			*OutOldValue = *Existing;
		}
		*Existing = Sentence;
		return true; // existed before
	}

	// New entry
	DeclarativeKeyOrder.Add(Key);
	DeclarativeValues.Add(Key, Sentence);
	return false;
}

bool FConvaiDynamicContextTracker::RemoveDeclarative(const FString& Key)
{
	if (DeclarativeValues.Remove(Key) > 0)
	{
		DeclarativeKeyOrder.Remove(Key);
		return true;
	}
	return false;
}

bool FConvaiDynamicContextTracker::GetDeclarativeValue(const FString& Key, FString& OutValue) const
{
	if (const FString* Found = DeclarativeValues.Find(Key))
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

void FConvaiDynamicContextTracker::GetStatesSnapshot(TArray<TPair<FString, FString>>& Out) const
{
	Out.Reset();
	Out.Reserve(StateKeyOrder.Num());
	for (const FString& Key : StateKeyOrder)
	{
		if (const FString* Val = StateValues.Find(Key))
		{
			Out.Emplace(Key, *Val);
		}
	}
}

void FConvaiDynamicContextTracker::GetDeclarativesSnapshot(TArray<TPair<FString, FString>>& Out) const
{
	Out.Reset();
	Out.Reserve(DeclarativeKeyOrder.Num());
	for (const FString& Key : DeclarativeKeyOrder)
	{
		if (const FString* Sentence = DeclarativeValues.Find(Key))
		{
			Out.Emplace(Key, *Sentence);
		}
	}
}

FString FConvaiDynamicContextTracker::BuildCanonicalContext(const TSet<FString>& ExcludeStateKeys) const
{
	TArray<FString> Lines;
	Lines.Reserve(StateKeyOrder.Num() + DeclarativeKeyOrder.Num() + Events.Num());

	for (const FString& Key : StateKeyOrder)
	{
		if (ExcludeStateKeys.Contains(Key))
		{
			continue;
		}
		if (const FString* Val = StateValues.Find(Key))
		{
			// Multi-word values are quoted at render time (storage stays raw) so
			// the value's boundary is unambiguous in the prompt line.
			Lines.Add(FString::Printf(TEXT("%s is %s"), *Key, *ConvaiContextFormat::FormatValueForPrompt(*Val)));
		}
	}

	// Declarative facts render as the bare sentence — no "Key is" prefix.
	for (const FString& Key : DeclarativeKeyOrder)
	{
		if (const FString* Val = DeclarativeValues.Find(Key))
		{
			Lines.Add(*Val);
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
	DeclarativeKeyOrder.Empty();
	DeclarativeValues.Empty();
	Events.Empty();
}
