#pragma once

#include <cstdint>
#include <string>

// Owned by the game thread. Transport callbacks must dispatch before using it.
class FConvaiPttReleaseTracker
{
public:
    enum class EState { Idle, Draining, AwaitingCommit, RetryScheduled, Failed };
    bool CanStart(uint64_t Epoch) const { return Epoch != Generation || State == EState::Idle; }
    bool Begin(const std::string& Id, uint64_t Epoch, double Now)
    {
        if (!CanStart(Epoch) || Id.empty()) return false;
        Generation = Epoch;
        StopId = Id;
        State = EState::Draining;
        Deadline = Now + 12.0;
        RetryCount = 0;
        return true;
    }
    bool Matches(const std::string& Id, uint64_t Epoch) const
    {
        return Epoch == Generation && Id == StopId &&
            (State == EState::Draining || State == EState::AwaitingCommit || State == EState::RetryScheduled);
    }
    bool Submitted(const std::string& Id, uint64_t Epoch)
    {
        if (!Matches(Id, Epoch)) return false;
        State = EState::AwaitingCommit;
        return true;
    }
    bool Complete(const std::string& Id, uint64_t Epoch, bool Success)
    {
        if (!Matches(Id, Epoch)) return false;
        State = Success ? EState::Idle : EState::Failed;
        return true;
    }
    bool ScheduleRetry(const std::string& Id, uint64_t Epoch, double Now)
    {
        if (!Matches(Id, Epoch) || State != EState::AwaitingCommit ||
            RetryCount >= 2 || Now >= Deadline) return false;
        RetryAt = Now + (RetryCount == 0 ? 0.25 : 0.5);
        ++RetryCount;
        State = EState::RetryScheduled;
        return true;
    }
    bool TakeRetry(uint64_t Epoch, double Now)
    {
        if (Epoch != Generation || State != EState::RetryScheduled ||
            Now < RetryAt || Now >= Deadline) return false;
        State = EState::AwaitingCommit;
        return true;
    }
    // Legacy servers acknowledge queueing, not final STT commitment. Do not
    // expose this compatibility acknowledgement as a committed transcript.
    bool AcceptLegacy(uint64_t Epoch)
    {
        if (Epoch != Generation || State != EState::AwaitingCommit) return false;
        State = EState::Idle;
        return true;
    }
    bool Expire(uint64_t Epoch, double Now)
    {
        if (!Matches(StopId, Epoch) || Now < Deadline) return false;
        State = EState::Failed;
        return true;
    }
    const std::string& Id() const { return StopId; }
private:
    EState State = EState::Idle;
    uint64_t Generation = 0;
    std::string StopId;
    double Deadline = 0;
    double RetryAt = 0;
    unsigned RetryCount = 0;
};
