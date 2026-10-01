/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NavigationHooksMiddleware.cpp
 *
 * Implementation of navigation hooks middleware.
 */

#include "Services/Navigation/NavigationHooksMiddleware.h"
#include "Logging/ConvaiEditorNavigationLog.h"

namespace ConvaiEditor
{
    FNavigationHooksMiddleware::FNavigationHooksMiddleware()
    {
    }

    FNavigationMiddlewareResult FNavigationHooksMiddleware::CanNavigate(const FNavigationContext &Context)
    {
        return FNavigationMiddlewareResult::Allow();
    }

    void FNavigationHooksMiddleware::OnBeforeNavigate(const FNavigationContext &Context)
    {
        FScopeLock Lock(&HooksLock);

        ExecuteHooksForRoute(Context.ToRoute, BeforeHooks, Context);
    }

    void FNavigationHooksMiddleware::OnAfterNavigate(const FNavigationContext &Context)
    {
        FScopeLock Lock(&HooksLock);

        ExecuteHooksForRoute(Context.ToRoute, AfterHooks, Context);
    }

    void FNavigationHooksMiddleware::RegisterBeforeHook(Route::E Route, FNavigationHook Hook)
    {
        if (!Hook.IsBound())
        {
            UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("NavigationHooksMiddleware: invalid before hook registration for route: %s"), *Route::ToString(Route));
            return;
        }

        FScopeLock Lock(&HooksLock);

        TArray<FNavigationHook> &Hooks = BeforeHooks.FindOrAdd(Route);
        Hooks.Add(Hook);
    }

    void FNavigationHooksMiddleware::RegisterAfterHook(Route::E Route, FNavigationHook Hook)
    {
        if (!Hook.IsBound())
        {
            UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("NavigationHooksMiddleware: invalid after hook registration for route: %s"), *Route::ToString(Route));
            return;
        }

        FScopeLock Lock(&HooksLock);

        TArray<FNavigationHook> &Hooks = AfterHooks.FindOrAdd(Route);
        Hooks.Add(Hook);
    }

    void FNavigationHooksMiddleware::UnregisterHooksForRoute(Route::E Route)
    {
        FScopeLock Lock(&HooksLock);

        int32 BeforeCount = BeforeHooks.Remove(Route);
        int32 AfterCount = AfterHooks.Remove(Route);

        if (BeforeCount > 0 || AfterCount > 0)
        {
        }
    }

    void FNavigationHooksMiddleware::ClearAllHooks()
    {
        FScopeLock Lock(&HooksLock);

        int32 TotalBeforeHooks = BeforeHooks.Num();
        int32 TotalAfterHooks = AfterHooks.Num();

        BeforeHooks.Empty();
        AfterHooks.Empty();
    }

    void FNavigationHooksMiddleware::ExecuteHooksForRoute(Route::E Route, const TMap<Route::E, TArray<FNavigationHook>> &Hooks, const FNavigationContext &Context)
    {
        const TArray<FNavigationHook> *RouteHooks = Hooks.Find(Route);

        if (!RouteHooks || RouteHooks->Num() == 0)
        {
            return;
        }

        for (const FNavigationHook &Hook : *RouteHooks)
        {
            if (Hook.IsBound())
            {
                Hook.Execute(Context);
            }
        }
    }

} // namespace ConvaiEditor
