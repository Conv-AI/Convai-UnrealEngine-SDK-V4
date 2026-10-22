/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * CircuitBreakerRegistry.h
 *
 * Global registry for all circuit breakers in the system.
 */

#pragma once

#include "CoreMinimal.h"
#include "Utility/CircuitBreaker.h"

namespace ConvaiEditor
{
    /** Global registry for all circuit breakers */
    class CONVAIEDITOR_API FCircuitBreakerRegistry
    {
    public:
        /** Get singleton instance */
        static FCircuitBreakerRegistry &Get();

        /** Register a circuit breaker */
        void Register(const FString &Name, FCircuitBreaker *CircuitBreaker);

        /** Unregister a circuit breaker */
        void Unregister(const FString &Name);

        /** Force all OPEN circuit breakers to HALF-OPEN state */
        int32 ForceAllHalfOpen();

        /** Force all circuit breakers to CLOSED state */
        int32 ForceAllClosed();

        /** Get all registered circuit breaker names */
        TArray<FString> GetRegisteredNames() const;

        /** Get count of OPEN circuit breakers */
        int32 GetOpenCount() const;

    private:
        FCircuitBreakerRegistry() = default;

        TMap<FString, FCircuitBreaker *> Registry;
        mutable FCriticalSection Mutex;
    };

} // namespace ConvaiEditor
