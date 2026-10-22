/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NavigationMiddlewareManager.h
 *
 * Manager for navigation middleware registration and execution.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Navigation/INavigationMiddleware.h"
#include "HAL/CriticalSection.h"

namespace ConvaiEditor
{
    /** Manages navigation middleware registration and execution. */
    class CONVAIEDITOR_API FNavigationMiddlewareManager
    {
    public:
        static FNavigationMiddlewareManager &Get();
        void Initialize();
        void Shutdown();
        void RegisterMiddleware(TSharedPtr<INavigationMiddleware> Middleware);
        void UnregisterMiddleware(TSharedPtr<INavigationMiddleware> Middleware);
        bool ExecuteBeforeHooks(const FNavigationContext &Context, TOptional<Route::E> &OutRedirectRoute);
        void ExecuteAfterHooks(const FNavigationContext &Context);
        TArray<TSharedPtr<INavigationMiddleware>> GetRegisteredMiddleware() const;
        void ClearAllMiddleware();
        bool IsInitialized() const { return bIsInitialized; }

    private:
        FNavigationMiddlewareManager() = default;
        void SortMiddleware();

        TArray<TSharedPtr<INavigationMiddleware>> RegisteredMiddleware;
        mutable FCriticalSection MiddlewareLock;
        bool bIsInitialized = false;
        static FNavigationMiddlewareManager *Instance;
    };

} // namespace ConvaiEditor
