/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NavigationHooksMiddleware.h
 *
 * Middleware for custom navigation hooks.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Navigation/INavigationMiddleware.h"

namespace ConvaiEditor
{
    DECLARE_DELEGATE_OneParam(FNavigationHook, const FNavigationContext & /*Context*/);

    /** Provides route-specific navigation hooks. */
    class CONVAIEDITOR_API FNavigationHooksMiddleware : public INavigationMiddleware
    {
    public:
        FNavigationHooksMiddleware();

        virtual FNavigationMiddlewareResult CanNavigate(const FNavigationContext &Context) override;
        virtual void OnBeforeNavigate(const FNavigationContext &Context) override;
        virtual void OnAfterNavigate(const FNavigationContext &Context) override;
        virtual int32 GetPriority() const override { return 25; }
        virtual FString GetName() const override { return TEXT("NavigationHooks"); }

        void RegisterBeforeHook(Route::E Route, FNavigationHook Hook);
        void RegisterAfterHook(Route::E Route, FNavigationHook Hook);
        void UnregisterHooksForRoute(Route::E Route);
        void ClearAllHooks();
        int32 GetBeforeHookCount() const { return BeforeHooks.Num(); }
        int32 GetAfterHookCount() const { return AfterHooks.Num(); }

    private:
        void ExecuteHooksForRoute(Route::E Route, const TMap<Route::E, TArray<FNavigationHook>> &Hooks, const FNavigationContext &Context);

        TMap<Route::E, TArray<FNavigationHook>> BeforeHooks;
        TMap<Route::E, TArray<FNavigationHook>> AfterHooks;
        mutable FCriticalSection HooksLock;
    };

} // namespace ConvaiEditor
