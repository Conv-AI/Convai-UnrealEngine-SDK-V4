/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CircuitBreakerRegistry.cpp
 *
 * Implementation of the circuit breaker registry.
 */

#include "Utility/CircuitBreakerRegistry.h"
#include "ConvaiEditor.h"

namespace ConvaiEditor
{
    FCircuitBreakerRegistry &FCircuitBreakerRegistry::Get()
    {
        static FCircuitBreakerRegistry Instance;
        return Instance;
    }

    void FCircuitBreakerRegistry::Register(const FString &Name, FCircuitBreaker *CircuitBreaker)
    {
        if (!CircuitBreaker)
        {
            return;
        }

        FScopeLock Lock(&Mutex);

        if (Registry.Contains(Name))
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("Replacing existing circuit breaker: %s"), *Name);
        }

        Registry.Add(Name, CircuitBreaker);
    }

    void FCircuitBreakerRegistry::Unregister(const FString &Name)
    {
        FScopeLock Lock(&Mutex);

        Registry.Remove(Name);
    }

    int32 FCircuitBreakerRegistry::ForceAllHalfOpen()
    {
        FScopeLock Lock(&Mutex);

        int32 ResetCount = 0;

        for (const auto &Pair : Registry)
        {
            FCircuitBreaker *CB = Pair.Value;
            if (CB && CB->IsOpen())
            {
                CB->ForceHalfOpen();
                ResetCount++;
            }
        }

        return ResetCount;
    }

    int32 FCircuitBreakerRegistry::ForceAllClosed()
    {
        FScopeLock Lock(&Mutex);

        int32 ResetCount = 0;

        for (const auto &Pair : Registry)
        {
            FCircuitBreaker *CB = Pair.Value;
            if (CB && (CB->IsOpen() || CB->IsHalfOpen()))
            {
                CB->Close();
                ResetCount++;
            }
        }

        return ResetCount;
    }

    TArray<FString> FCircuitBreakerRegistry::GetRegisteredNames() const
    {
        FScopeLock Lock(&Mutex);

        TArray<FString> Names;
        Registry.GetKeys(Names);
        return Names;
    }

    int32 FCircuitBreakerRegistry::GetOpenCount() const
    {
        FScopeLock Lock(&Mutex);

        int32 Count = 0;
        for (const auto &Pair : Registry)
        {
            if (Pair.Value && Pair.Value->IsOpen())
            {
                Count++;
            }
        }
        return Count;
    }

} // namespace ConvaiEditor
