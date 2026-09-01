// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiTestEventRecorder.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

void FConvaiTestEventRecorder::Record(const FString& Kind, const FString& Detail,
                                      const FString& SessionProxyId)
{
    FConvaiTestEvent Event;
    Event.Kind = Kind;
    Event.Detail = Detail;
    Event.SessionProxyId = SessionProxyId;
    Event.TimeSeconds = FPlatformTime::Seconds();
    Event.ThreadId = FPlatformTLS::GetCurrentThreadId();

    FScopeLock ScopeLock(&Lock);
    Events.Add(MoveTemp(Event));
}

int32 FConvaiTestEventRecorder::CountOfKind(const FString& Kind) const
{
    FScopeLock ScopeLock(&Lock);
    int32 Count = 0;
    for (const FConvaiTestEvent& Event : Events)
    {
        Count += Event.Kind == Kind ? 1 : 0;
    }
    return Count;
}

bool FConvaiTestEventRecorder::HasNoneOfKind(const FString& Kind) const
{
    return CountOfKind(Kind) == 0;
}

bool FConvaiTestEventRecorder::Matches(
    TFunctionRef<bool(const FConvaiTestEvent&)> Predicate) const
{
    FScopeLock ScopeLock(&Lock);
    for (const FConvaiTestEvent& Event : Events)
    {
        if (Predicate(Event))
        {
            return true;
        }
    }
    return false;
}

TArray<FConvaiTestEvent> FConvaiTestEventRecorder::Snapshot() const
{
    FScopeLock ScopeLock(&Lock);
    return Events;
}

void FConvaiTestEventRecorder::Drain()
{
    FScopeLock ScopeLock(&Lock);
    Events.Reset();
}

bool FConvaiTestEventRecorder::DumpToFile(const FString& Path) const
{
    const TArray<FConvaiTestEvent> Copy = Snapshot();

    TArray<FString> Lines;
    Lines.Reserve(Copy.Num());
    for (const FConvaiTestEvent& Event : Copy)
    {
        // Condensed, not the default pretty policy: JSONL means one object per
        // line, and a pretty writer spreads each event across several.
        FString Line;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("t"), Event.TimeSeconds);
        Writer->WriteValue(TEXT("kind"), Event.Kind);
        Writer->WriteValue(TEXT("detail"), Event.Detail);
        Writer->WriteValue(TEXT("thread"), static_cast<int32>(Event.ThreadId));
        if (!Event.SessionProxyId.IsEmpty())
        {
            Writer->WriteValue(TEXT("proxy"), Event.SessionProxyId);
        }
        Writer->WriteObjectEnd();
        Writer->Close();
        Lines.Add(Line);
    }

    // UTF-8, not UE's default UTF-16: the trace's only consumer is a fix agent
    // and its tooling, and a UTF-16 .jsonl fails to parse in every standard
    // JSON reader.
    return FFileHelper::SaveStringArrayToFile(Lines, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FConvaiTestLatencyTracker::Begin()
{
    FScopeLock ScopeLock(&Lock);
    StartSeconds = FPlatformTime::Seconds();
    Milestones.Reset();
}

void FConvaiTestLatencyTracker::Mark(const FString& Label)
{
    FScopeLock ScopeLock(&Lock);
    if (StartSeconds <= 0.0 || Milestones.Contains(Label))
    {
        return;
    }
    Milestones.Add(Label, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

TMap<FString, double> FConvaiTestLatencyTracker::Snapshot() const
{
    FScopeLock ScopeLock(&Lock);
    return Milestones;
}
