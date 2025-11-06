/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiDIContainer.h
 *
 * Dependency injection container for ConvaiEditor services.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "ConvaiEditor.h"

// Forward declarations
namespace ConvaiEditor
{
    class FServiceScope;
    class FScopeManager;
}

/** Result type for error handling without exceptions. */
template <typename T>
class CONVAIEDITOR_API TConvaiResult
{
public:
    TConvaiResult() : bSuccess(false), ErrorCode(0) {}

    bool IsSuccess() const { return bSuccess; }
    bool IsFailure() const { return !bSuccess; }

    const T &GetValue() const
    {
        checkf(bSuccess, TEXT("Attempted to get value from failed result: %s"), *ErrorMessage);
        checkf(Value.IsSet(), TEXT("Value is not set in successful result"));
        return Value.GetValue();
    }

    T &GetValue()
    {
        checkf(bSuccess, TEXT("Attempted to get value from failed result: %s"), *ErrorMessage);
        checkf(Value.IsSet(), TEXT("Value is not set in successful result"));
        return Value.GetValue();
    }

    const FString &GetError() const
    {
        checkf(!bSuccess, TEXT("Attempted to get error from successful result"));
        return ErrorMessage;
    }

    FString GetFullError() const
    {
        checkf(!bSuccess, TEXT("Attempted to get error from successful result"));

        if (ErrorContext.Num() == 0)
        {
            return ErrorMessage;
        }

        FString FullError = ErrorMessage;
        for (int32 i = ErrorContext.Num() - 1; i >= 0; --i)
        {
            FullError = FString::Printf(TEXT("%s → %s"), *ErrorContext[i], *FullError);
        }

        return FullError;
    }

    int32 GetErrorCode() const
    {
        checkf(!bSuccess, TEXT("Attempted to get error code from successful result"));
        return ErrorCode;
    }

    const TMap<FString, FString> &GetMetadata() const
    {
        return ErrorMetadata;
    }

    TConvaiResult<T> WithContext(const FString &Context) const
    {
        checkf(!bSuccess, TEXT("Cannot add context to successful result"));

        TConvaiResult<T> NewResult;
        NewResult.Value = Value;
        NewResult.ErrorMessage = ErrorMessage;
        NewResult.bSuccess = false;
        NewResult.ErrorCode = ErrorCode;
        NewResult.ErrorContext = ErrorContext;
        NewResult.ErrorContext.Add(Context);
        NewResult.ErrorMetadata = ErrorMetadata;

        return NewResult;
    }

    TConvaiResult<T> WithCode(int32 Code) const
    {
        checkf(!bSuccess, TEXT("Cannot set error code on successful result"));

        TConvaiResult<T> NewResult;
        NewResult.Value = Value;
        NewResult.ErrorMessage = ErrorMessage;
        NewResult.bSuccess = false;
        NewResult.ErrorCode = Code;
        NewResult.ErrorContext = ErrorContext;
        NewResult.ErrorMetadata = ErrorMetadata;

        return NewResult;
    }

    /** Add metadata entry */
    TConvaiResult<T> WithMetadata(const FString &Key, const FString &Value) const
    {
        checkf(!bSuccess, TEXT("Cannot add metadata to successful result"));

        TConvaiResult<T> NewResult;
        NewResult.Value = Value;
        NewResult.ErrorMessage = ErrorMessage;
        NewResult.bSuccess = false;
        NewResult.ErrorCode = ErrorCode;
        NewResult.ErrorContext = ErrorContext;
        NewResult.ErrorMetadata = ErrorMetadata;
        NewResult.ErrorMetadata.Add(Key, Value);

        return NewResult;
    }

    static TConvaiResult<T> Success(const T &InValue)
    {
        TConvaiResult<T> Result;
        Result.Value = InValue;
        Result.bSuccess = true;
        return Result;
    }

    static TConvaiResult<T> Success(T &&InValue)
    {
        TConvaiResult<T> Result;
        Result.Value = MoveTemp(InValue);
        Result.bSuccess = true;
        return Result;
    }

    static TConvaiResult<T> Failure(const FString &InError)
    {
        TConvaiResult<T> Result;
        Result.ErrorMessage = InError;
        Result.bSuccess = false;
        Result.ErrorCode = 0;
        return Result;
    }

    /**
     * Create failure with error code
     */
    static TConvaiResult<T> Failure(const FString &InError, int32 InErrorCode)
    {
        TConvaiResult<T> Result;
        Result.ErrorMessage = InError;
        Result.ErrorCode = InErrorCode;
        Result.bSuccess = false;
        return Result;
    }

    /** Map - Transform the success value if present */
    template <typename TFunc>
    auto Map(TFunc Func) const -> TConvaiResult<decltype(Func(DeclVal<T>()))>
    {
        using TResult = decltype(Func(DeclVal<T>()));

        if (IsFailure())
        {
            // Propagate error with context
            TConvaiResult<TResult> FailureResult;
            FailureResult.ErrorMessage = ErrorMessage;
            FailureResult.bSuccess = false;
            FailureResult.ErrorCode = ErrorCode;
            FailureResult.ErrorContext = ErrorContext;
            FailureResult.ErrorMetadata = ErrorMetadata;
            return FailureResult;
        }

        // Apply transformation
        TResult TransformedValue = Func(GetValue());
        return TConvaiResult<TResult>::Success(MoveTemp(TransformedValue));
    }

    /** Bind - Chain operations that return Result types (flatMap/andThen) */
    template <typename TFunc>
    auto Bind(TFunc Func) const -> decltype(Func(DeclVal<T>()))
    {
        using TResult = decltype(Func(DeclVal<T>()));

        if (IsFailure())
        {
            // Propagate error
            TResult FailureResult;
            FailureResult.ErrorMessage = ErrorMessage;
            FailureResult.bSuccess = false;
            FailureResult.ErrorCode = ErrorCode;
            FailureResult.ErrorContext = ErrorContext;
            FailureResult.ErrorMetadata = ErrorMetadata;
            return FailureResult;
        }

        // Chain the operation
        return Func(GetValue());
    }

    /** OrElse - Provide a fallback if the result is a failure */
    template <typename TFunc>
    TConvaiResult<T> OrElse(TFunc Func) const
    {
        if (IsSuccess())
        {
            return *this;
        }

        // Try fallback
        return Func();
    }

    /** LogOnFailure - Automatically log errors */
    TConvaiResult<T> LogOnFailure(const FLogCategoryBase &LogCategory, const TCHAR *Message) const
    {
        if (IsFailure())
        {
            FString FullError = GetFullError();
            UE_LOG_REF(LogCategory, Error, TEXT("%s: %s"), Message, *FullError);
        }
        return *this;
    }

    /** LogOnFailureWithCode - Conditionally log based on error code */
    TConvaiResult<T> LogOnFailureWithCode(const FLogCategoryBase &LogCategory, int32 Code, const TCHAR *Message) const
    {
        if (IsFailure() && GetErrorCode() == Code)
        {
            FString FullError = GetFullError();
            UE_LOG_REF(LogCategory, Error, TEXT("%s (Code: %d): %s"), Message, Code, *FullError);
        }
        return *this;
    }

    /** LogOnSuccess - Log successful operations */
    TConvaiResult<T> LogOnSuccess(const FLogCategoryBase &LogCategory, const TCHAR *Message) const
    {
        if (IsSuccess())
        {
            UE_LOG_REF(LogCategory, Log, TEXT("%s"), Message);
        }
        return *this;
    }

    /** Tap - Execute a side effect without modifying the result */
    template <typename TFunc>
    TConvaiResult<T> Tap(TFunc Func) const
    {
        if (IsSuccess())
        {
            Func(GetValue());
        }
        return *this;
    }

    /** TapError - Execute a side effect on error without modifying the result */
    template <typename TFunc>
    TConvaiResult<T> TapError(TFunc Func) const
    {
        if (IsFailure())
        {
            Func(GetFullError());
        }
        return *this;
    }

private:
    TOptional<T> Value;
    FString ErrorMessage;
    bool bSuccess;
    int32 ErrorCode;
    TArray<FString> ErrorContext;
    TMap<FString, FString> ErrorMetadata;
};

/**
 * Specialization for void type
 */
template <>
class CONVAIEDITOR_API TConvaiResult<void>
{
public:
    TConvaiResult() : bSuccess(false), ErrorCode(0) {}

    bool IsSuccess() const { return bSuccess; }
    bool IsFailure() const { return !bSuccess; }

    void GetValue() const
    {
        checkf(bSuccess, TEXT("Attempted to get value from failed result: %s"), *ErrorMessage);
    }

    const FString &GetError() const
    {
        checkf(!bSuccess, TEXT("Attempted to get error from successful result"));
        return ErrorMessage;
    }

    /**
     * Get full error message including all context chain
     */
    FString GetFullError() const
    {
        checkf(!bSuccess, TEXT("Attempted to get error from successful result"));

        if (ErrorContext.Num() == 0)
        {
            return ErrorMessage;
        }

        FString FullError = ErrorMessage;
        for (int32 i = ErrorContext.Num() - 1; i >= 0; --i)
        {
            FullError = FString::Printf(TEXT("%s → %s"), *ErrorContext[i], *FullError);
        }

        return FullError;
    }

    /**
     * Get error code (0 if not set)
     */
    int32 GetErrorCode() const
    {
        checkf(!bSuccess, TEXT("Attempted to get error code from successful result"));
        return ErrorCode;
    }

    /**
     * Get error metadata
     */
    const TMap<FString, FString> &GetMetadata() const
    {
        return ErrorMetadata;
    }

    /**
     * Add context to an existing error result
     */
    TConvaiResult<void> WithContext(const FString &Context) const
    {
        checkf(!bSuccess, TEXT("Cannot add context to successful result"));

        TConvaiResult<void> NewResult;
        NewResult.ErrorMessage = ErrorMessage;
        NewResult.bSuccess = false;
        NewResult.ErrorCode = ErrorCode;
        NewResult.ErrorContext = ErrorContext;
        NewResult.ErrorContext.Add(Context);
        NewResult.ErrorMetadata = ErrorMetadata;

        return NewResult;
    }

    /**
     * Set error code for categorization
     */
    TConvaiResult<void> WithCode(int32 Code) const
    {
        checkf(!bSuccess, TEXT("Cannot set error code on successful result"));

        TConvaiResult<void> NewResult;
        NewResult.ErrorMessage = ErrorMessage;
        NewResult.bSuccess = false;
        NewResult.ErrorCode = Code;
        NewResult.ErrorContext = ErrorContext;
        NewResult.ErrorMetadata = ErrorMetadata;

        return NewResult;
    }

    /**
     * Add metadata entry
     */
    TConvaiResult<void> WithMetadata(const FString &Key, const FString &Value) const
    {
        checkf(!bSuccess, TEXT("Cannot add metadata to successful result"));

        TConvaiResult<void> NewResult;
        NewResult.ErrorMessage = ErrorMessage;
        NewResult.bSuccess = false;
        NewResult.ErrorCode = ErrorCode;
        NewResult.ErrorContext = ErrorContext;
        NewResult.ErrorMetadata = ErrorMetadata;
        NewResult.ErrorMetadata.Add(Key, Value);

        return NewResult;
    }

    static TConvaiResult<void> Success()
    {
        TConvaiResult<void> Result;
        Result.bSuccess = true;
        return Result;
    }

    static TConvaiResult<void> Failure(const FString &InError)
    {
        TConvaiResult<void> Result;
        Result.ErrorMessage = InError;
        Result.bSuccess = false;
        Result.ErrorCode = 0;
        return Result;
    }

    /**
     * Create failure with error code
     */
    static TConvaiResult<void> Failure(const FString &InError, int32 InErrorCode)
    {
        TConvaiResult<void> Result;
        Result.ErrorMessage = InError;
        Result.ErrorCode = InErrorCode;
        Result.bSuccess = false;
        return Result;
    }

    //----------------------------------------
    // Monadic Operations (Functional Programming) - void specialization
    //----------------------------------------

    /**
     * Bind - Chain operations that return Result types
     *
     * For void results, this allows chaining operations that don't return values.
     *
     * Example:
     * @code
     * auto Result = Service->Initialize()
     *     .Bind([]() { return Service->Connect(); })
     *     .Bind([]() { return Service->Authenticate(); });
     * @endcode
     *
     * @tparam TFunc Function type (void -> TConvaiResult<U>)
     * @param Func Function that returns a Result
     * @return Chained result
     */
    template <typename TFunc>
    auto Bind(TFunc Func) const -> decltype(Func())
    {
        using TResult = decltype(Func());

        if (IsFailure())
        {
            // Propagate error
            TResult FailureResult;
            FailureResult.ErrorMessage = ErrorMessage;
            FailureResult.bSuccess = false;
            FailureResult.ErrorCode = ErrorCode;
            FailureResult.ErrorContext = ErrorContext;
            FailureResult.ErrorMetadata = ErrorMetadata;
            return FailureResult;
        }

        // Chain the operation
        return Func();
    }

    /**
     * OrElse - Provide a fallback if the result is a failure
     *
     * Example:
     * @code
     * auto Result = Service->TryPrimaryOperation()
     *     .OrElse([]() { return Service->TryFallbackOperation(); });
     * @endcode
     *
     * @tparam TFunc Function type (void -> TConvaiResult<void>)
     * @param Func Fallback function
     * @return Original result if successful, fallback result otherwise
     */
    template <typename TFunc>
    TConvaiResult<void> OrElse(TFunc Func) const
    {
        if (IsSuccess())
        {
            return *this;
        }

        // Try fallback
        return Func();
    }

    //----------------------------------------
    // Automatic Logging - void specialization
    //----------------------------------------

    /**
     * LogOnFailure - Automatically log errors
     *
     * Example:
     * @code
     * auto Result = Service->Initialize()
     *     .LogOnFailure(LogConvaiEditor, TEXT("Initialization failed"));
     * @endcode
     */
    TConvaiResult<void> LogOnFailure(const FLogCategoryBase &LogCategory, const TCHAR *Message) const
    {
        if (IsFailure())
        {
            FString FullError = GetFullError();
            UE_LOG_REF(LogCategory, Error, TEXT("%s: %s"), Message, *FullError);
        }
        return *this;
    }

    /**
     * LogOnFailureWithCode - Conditionally log based on error code
     */
    TConvaiResult<void> LogOnFailureWithCode(const FLogCategoryBase &LogCategory, int32 Code, const TCHAR *Message) const
    {
        if (IsFailure() && GetErrorCode() == Code)
        {
            FString FullError = GetFullError();
            UE_LOG_REF(LogCategory, Error, TEXT("%s (Code: %d): %s"), Message, Code, *FullError);
        }
        return *this;
    }

    /**
     * LogOnSuccess - Log successful operations
     */
    TConvaiResult<void> LogOnSuccess(const FLogCategoryBase &LogCategory, const TCHAR *Message) const
    {
        if (IsSuccess())
        {
            UE_LOG_REF(LogCategory, Log, TEXT("%s"), Message);
        }
        return *this;
    }

    /**
     * Tap - Execute a side effect without modifying the result
     *
     * Example:
     * @code
     * auto Result = Service->Initialize()
     *     .Tap([]() { UE_LOG(LogTemp, Log, TEXT("Initialization completed")); });
     * @endcode
     */
    template <typename TFunc>
    TConvaiResult<void> Tap(TFunc Func) const
    {
        if (IsSuccess())
        {
            Func();
        }
        return *this;
    }

    /**
     * TapError - Execute a side effect on error without modifying the result
     */
    template <typename TFunc>
    TConvaiResult<void> TapError(TFunc Func) const
    {
        if (IsFailure())
        {
            Func(GetFullError());
        }
        return *this;
    }

private:
    FString ErrorMessage;
    bool bSuccess;
    int32 ErrorCode;
    TArray<FString> ErrorContext;
    TMap<FString, FString> ErrorMetadata;
};

/**
 * Service Lifetime Scopes
 */
enum class EConvaiServiceLifetime : uint8
{
    /** New instance created every time service is resolved */
    Transient,
    /** Single instance shared across entire application lifetime */
    Singleton,
    /** Single instance per resolution scope (future extension) */
    Scoped
};

/**
 * Service Factory Function Type
 * Used for creating service instances with dependency injection
 */
template <typename T>
using TConvaiServiceFactory = TFunction<TSharedPtr<T>(class IConvaiDIContainer *)>;

/**
 * Professional Dependency Injection Container Interface
 *
 * Note: Cannot use virtual template functions in C++, so we use a type-erased approach
 */
class CONVAIEDITOR_API IConvaiDIContainer
{
public:
    /**
     * Type-erased factory function type
     */
    using FServiceFactory = TFunction<TSharedPtr<IConvaiService>(IConvaiDIContainer *)>;

    virtual ~IConvaiDIContainer() = default;

    /**
     * Register a service with automatic factory creation
     *
     * @param Lifetime Service lifetime management
     * @return Result indicating success or failure
     */
    template <typename TInterface, typename TConcrete>
    TConvaiResult<void> RegisterService(EConvaiServiceLifetime Lifetime = EConvaiServiceLifetime::Singleton)
    {
        static_assert(std::is_base_of_v<TInterface, TConcrete>,
                      "TConcrete must derive from TInterface");
        static_assert(std::is_base_of_v<IConvaiService, TInterface>,
                      "TInterface must derive from IConvaiService");

        // Create factory function that handles dependency injection
        auto Factory = [](IConvaiDIContainer *Container) -> TSharedPtr<TInterface>
        {
            return CreateServiceInstance<TInterface, TConcrete>(Container);
        };

        return RegisterServiceWithFactory<TInterface>(
            MoveTemp(Factory),
            Lifetime,
            TInterface::StaticType());
    }

    /**
     * Register a service with custom factory function
     *
     * @param Factory Custom factory function
     * @param Lifetime Service lifetime management
     * @param ServiceTypeName Name for debugging
     * @return Result indicating success or failure
     */
    template <typename TInterface>
    TConvaiResult<void> RegisterServiceWithFactory(
        TConvaiServiceFactory<TInterface> Factory,
        EConvaiServiceLifetime Lifetime,
        FName ServiceTypeName)
    {
        // Type-erased registration - delegate to concrete implementation
        return RegisterServiceInternal(
            TInterface::StaticType(),
            [Factory](IConvaiDIContainer *Container) -> TSharedPtr<IConvaiService>
            {
                auto Service = Factory(Container);
                return StaticCastSharedPtr<IConvaiService>(Service);
            },
            Lifetime,
            ServiceTypeName);
    }

    /**
     * Register a pre-created service instance
     *
     * @param Instance Pre-created service instance
     * @return Result indicating success or failure
     */
    template <typename TInterface>
    TConvaiResult<void> RegisterInstance(TSharedPtr<TInterface> Instance)
    {
        if (!Instance.IsValid())
        {
            return TConvaiResult<void>::Failure(
                FString::Printf(TEXT("Cannot register null instance for: %s"), *TInterface::StaticType().ToString()));
        }

        // Create factory that returns the pre-created instance
        auto Factory = [Instance](IConvaiDIContainer *Container) -> TSharedPtr<TInterface>
        {
            return Instance;
        };

        return RegisterServiceWithFactory<TInterface>(
            MoveTemp(Factory),
            EConvaiServiceLifetime::Singleton,
            TInterface::StaticType());
    }

    /**
     * Resolve a service by interface type
     *
     * @return Result containing the service instance or error
     */
    template <typename TInterface>
    TConvaiResult<TSharedPtr<TInterface>> Resolve()
    {
        auto Result = ResolveServiceInternal(TInterface::StaticType());
        if (Result.IsFailure())
        {
            return TConvaiResult<TSharedPtr<TInterface>>::Failure(Result.GetError());
        }

        auto TypedService = StaticCastSharedPtr<TInterface>(Result.GetValue());
        if (!TypedService.IsValid())
        {
            return TConvaiResult<TSharedPtr<TInterface>>::Failure(
                FString::Printf(TEXT("Service type cast failed for: %s"), *TInterface::StaticType().ToString()));
        }

        return TConvaiResult<TSharedPtr<TInterface>>::Success(TypedService);
    }

    /**
     * Resolve a service by interface type (required - throws on failure)
     *
     * @return Service instance (never null)
     */
    template <typename TInterface>
    TSharedPtr<TInterface> ResolveRequired()
    {
        auto Result = Resolve<TInterface>();
        checkf(Result.IsSuccess(), TEXT("Required service not found: %s - %s"),
               *TInterface::StaticType().ToString(), *Result.GetError());
        return Result.GetValue();
    }

    /**
     * Check if a service is registered
     *
     * @return True if service is registered
     */
    template <typename TInterface>
    bool IsRegistered() const
    {
        return IsServiceRegisteredInternal(TInterface::StaticType());
    }

    /**
     * Unregister a service
     *
     * @return Result indicating success or failure
     */
    template <typename TInterface>
    TConvaiResult<void> Unregister()
    {
        return UnregisterServiceInternal(TInterface::StaticType());
    }

    /**
     * Clear all registered services
     */
    virtual void Clear() = 0;

    /**
     * Get container statistics
     */
    struct FContainerStats
    {
        int32 RegisteredServices = 0;
        int32 SingletonInstances = 0;
        int32 TransientServices = 0;
        TArray<FName> ServiceTypes;
    };

    virtual FContainerStats GetStats() const = 0;

protected:
    /**
     * Type-erased service registration
     */
    virtual TConvaiResult<void> RegisterServiceInternal(
        FName ServiceType,
        FServiceFactory Factory,
        EConvaiServiceLifetime Lifetime,
        FName ServiceTypeName) = 0;

    /**
     * Type-erased service resolution
     */
    virtual TConvaiResult<TSharedPtr<IConvaiService>> ResolveServiceInternal(FName ServiceType) = 0;

    /**
     * Type-erased service registration check
     */
    virtual bool IsServiceRegisteredInternal(FName ServiceType) const = 0;

    /**
     * Type-erased service unregistration
     */
    virtual TConvaiResult<void> UnregisterServiceInternal(FName ServiceType) = 0;

private:
    /**
     * Create service instance with dependency injection
     * No UObject support - pure C++ services only
     */
    template <typename TInterface, typename TConcrete>
    static TSharedPtr<TInterface> CreateServiceInstance(IConvaiDIContainer *Container)
    {
        // Only support pure C++ classes - no UObject derivatives
        static_assert(!std::is_base_of_v<UObject, TConcrete>,
                      "UObject-derived services are not supported. Use pure C++ classes only.");

        return MakeShared<TConcrete>();
    }
};

/**
 * Service Descriptor for type-erased storage
 */
struct CONVAIEDITOR_API FConvaiServiceDescriptor
{
    /** Type-erased factory function */
    IConvaiDIContainer::FServiceFactory Factory;

    /** Service lifetime management */
    EConvaiServiceLifetime Lifetime;

    /** Service type name for debugging */
    FName ServiceTypeName;

    /** Whether this service has been initialized */
    bool bIsInitialized;

    /** Cached singleton instance (if lifetime is Singleton) */
    TSharedPtr<IConvaiService> SingletonInstance;

    FConvaiServiceDescriptor()
        : Lifetime(EConvaiServiceLifetime::Transient), bIsInitialized(false)
    {
    }

    FConvaiServiceDescriptor(
        IConvaiDIContainer::FServiceFactory InFactory,
        EConvaiServiceLifetime InLifetime,
        FName InServiceTypeName)
        : Factory(MoveTemp(InFactory)), Lifetime(InLifetime), ServiceTypeName(InServiceTypeName), bIsInitialized(false)
    {
    }

    /** Safe destructor - prevents crashes during shutdown */
    ~FConvaiServiceDescriptor()
    {
        // Safely cleanup singleton instance
        if (SingletonInstance.IsValid())
        {
            // Safe shutdown with exception handling
            try
            {
                SingletonInstance->Shutdown();
            }
            catch (...)
            {
                UE_LOG(LogConvaiEditor, Error, TEXT("Exception during service shutdown: %s"),
                       ServiceTypeName.IsValid() ? *ServiceTypeName.ToString() : TEXT("Unknown"));
            }

            SingletonInstance.Reset();
        }

        // Clear factory function
        Factory = nullptr;
    }
};

/**
 * Concrete Implementation of Professional DI Container
 */
class CONVAIEDITOR_API FConvaiDIContainer : public IConvaiDIContainer
{
    // Friend class to allow scope management access
    friend class FConvaiDIContainerManager;

public:
    FConvaiDIContainer();
    virtual ~FConvaiDIContainer();

    // IConvaiDIContainer Interface
    virtual void Clear() override;
    virtual FContainerStats GetStats() const override;

protected:
    // Type-erased implementations
    virtual TConvaiResult<void> RegisterServiceInternal(
        FName ServiceType,
        FServiceFactory Factory,
        EConvaiServiceLifetime Lifetime,
        FName ServiceTypeName) override;

    virtual TConvaiResult<TSharedPtr<IConvaiService>> ResolveServiceInternal(FName ServiceType) override;
    virtual bool IsServiceRegisteredInternal(FName ServiceType) const override;
    virtual TConvaiResult<void> UnregisterServiceInternal(FName ServiceType) override;

private:
    /**
     * Thread-local context for dependency resolution tracking.
     *
     * This structure is thread-local to ensure that each thread has its own
     * independent resolution context, preventing race conditions and ensuring
     * correct circular dependency detection in multi-threaded environments.
     *
     * Architecture Benefits:
     * - Thread-safe resolution tracking without locks
     * - Accurate circular dependency detection per thread
     * - No performance overhead from synchronization
     * - Stack trace for better debugging
     */
    struct FThreadResolutionContext
    {
        /** Current resolution depth for this thread */
        int32 Depth = 0;

        /** Stack of currently resolving service types for circular dependency detection */
        TArray<FName> ResolutionStack;

        /** Reset the context to initial state */
        void Reset()
        {
            Depth = 0;
            ResolutionStack.Empty();
        }

        /** Check if we're currently resolving a specific service type */
        bool IsResolving(FName ServiceType) const
        {
            return ResolutionStack.Contains(ServiceType);
        }

        /** Get a string representation of the resolution stack for debugging */
        FString GetStackTrace() const
        {
            if (ResolutionStack.Num() == 0)
            {
                return TEXT("(empty)");
            }

            FString Trace;
            for (int32 i = 0; i < ResolutionStack.Num(); ++i)
            {
                if (i > 0)
                {
                    Trace += TEXT(" -> ");
                }
                Trace += ResolutionStack[i].ToString();
            }
            return Trace;
        }
    };

    /**
     * Get thread-local resolution context.
     *
     * We use a static function returning a reference to a function-local
     * static variable instead of a static member variable to avoid DLL
     * export issues with thread_local storage on Windows.
     *
     * This pattern is safe and ensures each thread gets its own context
     * without crossing DLL boundaries.
     */
    static FThreadResolutionContext &GetThreadContext()
    {
        static thread_local FThreadResolutionContext ThreadContext;
        return ThreadContext;
    }

    /** Maximum allowed resolution depth to prevent infinite recursion */
    static constexpr int32 MaxResolutionDepth = 32;

    /** Thread-safe access to service descriptors */
    mutable FRWLock ServicesLock;

    /** Map of service type to service descriptor */
    TMap<FName, TSharedPtr<FConvaiServiceDescriptor>> ServiceDescriptors;

    /** Scope manager for scoped lifetime services */
    TUniquePtr<ConvaiEditor::FScopeManager> ScopeManager;
};

/**
 * Global DI Container Singleton (Managed properly)
 */
class CONVAIEDITOR_API FConvaiDIContainerManager
{
public:
    /** Initialize the global container */
    static void Initialize();

    /** Shutdown and cleanup the global container */
    static void Shutdown();

    /** Get the global container instance */
    static IConvaiDIContainer &Get();

    /** Check if container is initialized */
    static bool IsInitialized();

    //========================================
    // Scope Management Methods
    //========================================

    /**
     * Create a new service scope
     *
     * @param ScopeName Human-readable name for the scope (e.g., "MainWindow")
     * @return Shared pointer to the created scope
     */
    static TSharedPtr<ConvaiEditor::FServiceScope> CreateScope(const FString &ScopeName);

    /**
     * Get the current active scope
     *
     * @return The current scope, or nullptr if no scope is active
     */
    static TSharedPtr<ConvaiEditor::FServiceScope> GetCurrentScope();

    /**
     * Push a scope onto the stack
     *
     * @param Scope The scope to make current
     */
    static void PushScope(TSharedPtr<ConvaiEditor::FServiceScope> Scope);

    /**
     * Pop the current scope from the stack
     *
     * @return The popped scope
     */
    static TSharedPtr<ConvaiEditor::FServiceScope> PopScope();

    /**
     * Destroy a scope and all its scoped services
     *
     * @param Scope The scope to destroy
     */
    static void DestroyScope(TSharedPtr<ConvaiEditor::FServiceScope> Scope);

    /**
     * Get the number of active scopes
     *
     * @return Number of scopes in the stack
     */
    static int32 GetActiveScopeCount();

private:
    static TUniquePtr<IConvaiDIContainer> Instance;
    static bool bIsInitialized;
};

// No template implementations in header anymore - all moved to type-erased methods
