/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiServiceRegistrationHelpers.h
 *
 * Helper utilities for registering services with the DI container.
 * Reduces code duplication and provides consistent error handling.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"

namespace ConvaiEditor
{
    /** Helper utilities for service registration. */
    namespace ServiceHelpers
    {
        /** Registers service with automatic logging. */
        template <typename TInterface, typename TConcrete>
        bool RegisterServiceWithLogging(
            IConvaiDIContainer &Container,
            const TCHAR *ServiceName,
            EConvaiServiceLifetime Lifetime = EConvaiServiceLifetime::Singleton)
        {
            auto Result = Container.RegisterService<TInterface, TConcrete>(Lifetime);

            if (Result.IsFailure())
            {
                UE_LOG(LogConvaiEditor, Error,
                       TEXT("Failed to register %s: %s"), ServiceName, *Result.GetError());
                return false;
            }

            return true;
        }

        /** Service Registration Batch for fluent API style. */
        struct CONVAIEDITOR_API FServiceRegistrationBatch
        {
            /** List of successfully registered services */
            TArray<FString> SuccessfulRegistrations;

            /** List of failed service registrations */
            TArray<FString> FailedRegistrations;

            /** Register a service and add to the batch tracking */
            template <typename TInterface, typename TConcrete>
            FServiceRegistrationBatch &Register(
                IConvaiDIContainer &Container,
                const TCHAR *ServiceName,
                EConvaiServiceLifetime Lifetime = EConvaiServiceLifetime::Singleton)
            {
                if (RegisterServiceWithLogging<TInterface, TConcrete>(
                        Container, ServiceName, Lifetime))
                {
                    SuccessfulRegistrations.Add(ServiceName);
                }
                else
                {
                    FailedRegistrations.Add(ServiceName);
                }
                return *this;
            }

            /** Log a summary of all registration attempts */
            void LogSummary() const
            {

                if (FailedRegistrations.Num() > 0)
                {
                    UE_LOG(LogConvaiEditor, Warning,
                           TEXT("Failed to register the following services:"));
                    for (const FString &ServiceName : FailedRegistrations)
                    {
                        UE_LOG(LogConvaiEditor, Warning, TEXT("  - %s"), *ServiceName);
                    }
                }
            }

            /** Check if all registrations were successful */
            bool AllSucceeded() const
            {
                return FailedRegistrations.Num() == 0;
            }

            /** Get the number of successful registrations */
            int32 GetSuccessCount() const
            {
                return SuccessfulRegistrations.Num();
            }

            /** Get the number of failed registrations */
            int32 GetFailureCount() const
            {
                return FailedRegistrations.Num();
            }
        };

    } // namespace ServiceHelpers
} // namespace ConvaiEditor
