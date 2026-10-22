// Copyright 2022 Convai Inc. All Rights Reserved.

#include "DynamicContext/ConvaiDynamicContextTracker.h"
#include "Utility/ConvaiContextFormat.h"
#include "Utility/ConvaiUtf8String.h"
#include "Utility/Log/ConvaiLogger.h"

DEFINE_LOG_CATEGORY_STATIC(ConvaiDynamicContextLog, Log, All);

namespace
{
	// Every Replace re-sends the whole history, and the transport refuses a
	// message over 64,000 bytes (DEFAULT_MAX_MESSAGE_SIZE in the LiveKit Rust
	// SDK's rtc_session.rs, convai-livekit-cpp 8796e7b; negotiation only lowers
	// it). With the chatbot's 16,000-byte Update Context cap this leaves about
	// 16 KB for states, facts, the action block, delta lines and the envelope.
	constexpr int32 MaxEventBytes = 32000;

	int32 EventLineBytes(const FString& Event)
	{
		return FTCHARToUTF8(*Event).Length() + 1;
	}
}

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
	// Cut, not dropped: an event alone over the budget would otherwise take the
	// whole history with it, itself included. Each cut is its own loss, so each
	// is reported; the trim below warns once, as it repeats once the history is full.
	FString Event = ConvaiUtf8::TruncateToBytes(Text, MaxEventBytes - 1);
	if (Event.Len() < Text.Len())
	{
		CONVAI_LOG(ConvaiDynamicContextLog, Warning,
			TEXT("A context event is longer than %d bytes: only its start is kept in the character's context"),
			MaxEventBytes - 1);
	}
	EventBytes += EventLineBytes(Event);
	Events.Add(MoveTemp(Event));

	int32 Dropped = 0;
	while (EventBytes > MaxEventBytes)
	{
		EventBytes -= EventLineBytes(Events[Dropped]);
		++Dropped;
	}
	if (Dropped == 0)
	{
		return;
	}
	Events.RemoveAt(0, Dropped);
	if (!bWarnedEventTrim)
	{
		bWarnedEventTrim = true;
		CONVAI_LOG(ConvaiDynamicContextLog, Warning,
			TEXT("Context event history passed %d bytes: the oldest events are dropped from the character's context"),
			MaxEventBytes);
	}
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
	return BuildCanonicalContextFrom(ExcludeStateKeys, 0);
}

FString FConvaiDynamicContextTracker::BuildCanonicalContextFrom(const TSet<FString>& ExcludeStateKeys,
	int32 FirstEvent) const
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

	for (int32 Index = FMath::Max(FirstEvent, 0); Index < Events.Num(); ++Index)
	{
		Lines.Add(Events[Index]);
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
	EventBytes = 0;
}
