/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IServiceContainer.h
 *
 * Interface for service container with lifecycle management.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "Services/ConvaiDIContainer.h"

// Forward declarations
class IConvaiService;

/** Service lifecycle phases. */
enum class EServiceLifecycle : uint8
{
    None,
    Registered,
    Initializing,
    Active,
    Shutting,
    Shutdown
};

/** Service registration options. */
struct CONVAIEDITOR_API FServiceRegistrationOptions
{
    bool bInitializeImmediately = true;
    bool bSingleton = true;
    int32 InitializationPriority = 0;
    TArray<FName> Dependencies;
    FString Description;
};

/** Service diagnostic information. */
struct CONVAIEDITOR_API FServiceDiagnostics
{
    int32 TotalServices = 0;
    int32 ActiveServices = 0;
    int32 FailedServices = 0;
    int64 MemoryUsage = 0;
    TMap<FName, float> InitializationTimes;
    TMap<FName, FString> FailedServicesErrors;
    TMap<FName, TArray<FName>> DependencyGraph;
};

/** Service validation result. */
struct CONVAIEDITOR_API FServiceValidationResult
{
    bool bIsValid = true;
    TArray<FString> Errors;
    TArray<FString> Warnings;
    TArray<TArray<FName>> CircularDependencies;
};

/** Professional service container interface */
class CONVAIEDITOR_API IServiceContainer
{
public:
    virtual ~IServiceContainer() = default;

    /** Register a service implementation as a singleton */
    template <typename TInterface, typename TImplementation>
    TConvaiResult<void> RegisterSingleton(const FServiceRegistrationOptions &Options = FServiceRegistrationOptions())
    {
        static_assert(std::is_base_of<IConvaiService, TInterface>::value,
                      "TInterface must derive from IConvaiService");
        static_assert(std::is_base_of<TInterface, TImplementation>::value,
                      "TImplementation must derive from TInterface");

        return RegisterSingletonInternal(
            TInterface::StaticType(),
            []()
            { return MakeShared<TImplementation>(); },
            Options);
    }

    /** Register a service implementation as transient */
    template <typename TInterface, typename TImplementation>
    TConvaiResult<void> RegisterTransient(const FServiceRegistrationOptions &Options = FServiceRegistrationOptions())
    {
        static_assert(std::is_base_of<IConvaiService, TInterface>::value,
                      "TInterface must derive from IConvaiService");
        static_assert(std::is_base_of<TInterface, TImplementation>::value,
                      "TImplementation must derive from TInterface");

        FServiceRegistrationOptions TransientOptions = Options;
        TransientOptions.bSingleton = false;

        return RegisterTransientInternal(
            TInterface::StaticType(),
            []()
            { return MakeShared<TImplementation>(); },
            TransientOptions);
    }

    /** Register a service instance directly */
    template <typename TInterface>
    TConvaiResult<void> RegisterInstance(TSharedPtr<TInterface> Instance, const FServiceRegistrationOptions &Options = FServiceRegistrationOptions())
    {
        static_assert(std::is_base_of<IConvaiService, TInterface>::value,
                      "TInterface must derive from IConvaiService");

        return RegisterInstanceInternal(
            TInterface::StaticType(),
            StaticCastSharedPtr<IConvaiService>(Instance),
            Options);
    }

    /** Resolve a service by interface type */
    template <typename TInterface>
    TConvaiResult<TSharedPtr<TInterface>> Resolve()
    {
        static_assert(std::is_base_of<IConvaiService, TInterface>::value,
                      "TInterface must derive from IConvaiService");

        auto Result = ResolveInternal(TInterface::StaticType());
        if (Result.IsFailure())
        {
            return TConvaiResult<TSharedPtr<TInterface>>::Failure(Result.GetError());
        }

        TSharedPtr<TInterface> CastedService = StaticCastSharedPtr<TInterface>(Result.GetValue());
        if (!CastedService.IsValid())
        {
            return TConvaiResult<TSharedPtr<TInterface>>::Failure(
                FString::Printf(TEXT("Failed to cast service to interface type: %s"), *TInterface::StaticType().ToString()));
        }

        return TConvaiResult<TSharedPtr<TInterface>>::Success(CastedService);
    }

    /** Check if a service is registered */
    template <typename TInterface>
    bool IsRegistered() const
    {
        return IsRegisteredInternal(TInterface::StaticType());
    }

    /** Get the lifecycle state of a service */
    template <typename TInterface>
    EServiceLifecycle GetLifecycleState() const
    {
        return GetLifecycleStateInternal(TInterface::StaticType());
    }

    /** Unregister a service */
    template <typename TInterface>
    TConvaiResult<void> Unregister()
    {
        return UnregisterInternal(TInterface::StaticType());
    }

    /** Initialize all registered services in dependency order */
    virtual TConvaiResult<FServiceValidationResult> InitializeAll() = 0;

    /** Shutdown all services in reverse dependency order */
    virtual TConvaiResult<void> ShutdownAll() = 0;

    /** Validate all service dependencies */
    virtual FServiceValidationResult ValidateServices() const = 0;

    /** Get diagnostic information about all services */
    virtual FServiceDiagnostics GetDiagnostics() const = 0;

    /** Clear all registered services */
    virtual TConvaiResult<void> Clear() = 0;

protected:
    /** Internal singleton registration implementation */
    virtual TConvaiResult<void> RegisterSingletonInternal(
        const FName &ServiceType,
        TFunction<TSharedPtr<IConvaiService>()> Factory,
        const FServiceRegistrationOptions &Options) = 0;

    /** Internal transient registration implementation */
    virtual TConvaiResult<void> RegisterTransientInternal(
        const FName &ServiceType,
        TFunction<TSharedPtr<IConvaiService>()> Factory,
        const FServiceRegistrationOptions &Options) = 0;

    /** Internal instance registration implementation */
    virtual TConvaiResult<void> RegisterInstanceInternal(
        const FName &ServiceType,
        TSharedPtr<IConvaiService> Instance,
        const FServiceRegistrationOptions &Options) = 0;

    /** Internal service resolution implementation */
    virtual TConvaiResult<TSharedPtr<IConvaiService>> ResolveInternal(const FName &ServiceType) = 0;

    /** Internal registration check implementation */
    virtual bool IsRegisteredInternal(const FName &ServiceType) const = 0;

    /** Internal lifecycle state check implementation */
    virtual EServiceLifecycle GetLifecycleStateInternal(const FName &ServiceType) const = 0;

    /** Internal unregistration implementation */
    virtual TConvaiResult<void> UnregisterInternal(const FName &ServiceType) = 0;
};
