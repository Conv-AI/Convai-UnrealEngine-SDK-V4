// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

// Ported from convai_harness's event_recorder: one sink every observation goes
// through, queried by predicate rather than by sleeping.
//
// One deliberate difference from that harness. Its waitFor() blocks the calling
// thread on a condition variable, which works because scenarios there run on
// their own thread. Here the game thread must keep ticking or the engine stops
// rendering audio, so the equivalent is Matches() polled from the runner's
// ticker. Blocking would measure the block.
struct CONVAITESTS_API FConvaiTestEvent
{
    FString Kind;
    FString Detail;
    double TimeSeconds = 0.0;
    uint32 ThreadId = 0;

    // Which Session Proxy the event belongs to, empty when it belongs to none.
    // Multi-character scenarios (issue 09) assert on this; it is recorded from
    // the start so those scenarios do not need the format to change.
    FString SessionProxyId;
};

class CONVAITESTS_API FConvaiTestEventRecorder
{
public:
    void Record(const FString& Kind, const FString& Detail = FString(),
                const FString& SessionProxyId = FString());

    int32 CountOfKind(const FString& Kind) const;
    bool HasNoneOfKind(const FString& Kind) const;

    // First event matching the predicate, or false. The runner polls this; see
    // the note above on why it does not block.
    bool Matches(TFunctionRef<bool(const FConvaiTestEvent&)> Predicate) const;

    TArray<FConvaiTestEvent> Snapshot() const;
    void Drain();

    // Every recorded event as JSONL, one object per line — the trace format
    // salvaged from the dead in-plugin harness. Written on failure and on
    // watchdog abort, because a failure with no trace is un-triageable.
    bool DumpToFile(const FString& Path) const;

private:
    mutable FCriticalSection Lock;
    TArray<FConvaiTestEvent> Events;
};

// Named milestones as offsets from a start point. First mark wins, so a
// callback that fires repeatedly cannot move a latency figure, and marking is
// safe from any thread because delegates do not all arrive on the game thread.
//
// Milestones are milliseconds. Label them `*_ms`: every report field these
// reach is read by someone deciding whether a number is healthy, and a
// `*_seconds` field holding milliseconds reads as healthy at 1000x too slow.
class CONVAITESTS_API FConvaiTestLatencyTracker
{
public:
    void Begin();
    void Mark(const FString& Label);
    TMap<FString, double> Snapshot() const;

private:
    mutable FCriticalSection Lock;
    double StartSeconds = 0.0;
    TMap<FString, double> Milestones;
};
