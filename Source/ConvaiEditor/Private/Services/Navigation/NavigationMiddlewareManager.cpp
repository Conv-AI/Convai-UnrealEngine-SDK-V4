/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NavigationMiddlewareManager.cpp
 *
 * Implementation of navigation middleware manager.
 */

#include "Services/Navigation/NavigationMiddlewareManager.h"
#include "Logging/ConvaiEditorNavigationLog.h"

namespace ConvaiEditor
{
    FNavigationMiddlewareManager *FNavigationMiddlewareManager::Instance = nullptr;

    FNavigationMiddlewareManager &FNavigationMiddlewareManager::Get()
    {
        if (!Instance)
        {
            Instance = new FNavigationMiddlewareManager();
        }
        return *Instance;
    }

    void FNavigationMiddlewareManager::Initialize()
    {
        if (bIsInitialized)
        {
            UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("NavigationMiddlewareManager: already initialized"));
            return;
        }

        bIsInitialized = true;
    }

    void FNavigationMiddlewareManager::Shutdown()
    {
        if (!bIsInitialized)
        {
            return;
        }

        FScopeLock Lock(&MiddlewareLock);

        RegisteredMiddleware.Empty();

        bIsInitialized = false;
    }

    void FNavigationMiddlewareManager::RegisterMiddleware(TSharedPtr<INavigationMiddleware> Middleware)
    {
        if (!Middleware.IsValid())
        {
            UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("NavigationMiddlewareManager: invalid middleware registration attempt"));
            return;
        }

        FScopeLock Lock(&MiddlewareLock);

        if (RegisteredMiddleware.Contains(Middleware))
        {
            UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("NavigationMiddlewareManager: middleware already registered: %s"), *Middleware->GetName());
            return;
        }

        RegisteredMiddleware.Add(Middleware);
        SortMiddleware();
    }

    void FNavigationMiddlewareManager::UnregisterMiddleware(TSharedPtr<INavigationMiddleware> Middleware)
    {
        if (!Middleware.IsValid())
        {
            return;
        }

        FScopeLock Lock(&MiddlewareLock);

        int32 RemovedCount = RegisteredMiddleware.Remove(Middleware);
    }

    bool FNavigationMiddlewareManager::ExecuteBeforeHooks(const FNavigationContext &Context, TOptional<Route::E> &OutRedirectRoute)
    {
        FScopeLock Lock(&MiddlewareLock);

        for (const TSharedPtr<INavigationMiddleware> &Middleware : RegisteredMiddleware)
        {
            if (!Middleware.IsValid() || !Middleware->IsEnabled())
            {
                continue;
            }

            FNavigationMiddlewareResult Result = Middleware->CanNavigate(Context);

            if (!Result.bShouldProceed)
            {
                UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("NavigationMiddlewareManager: navigation blocked by %s: %s"), *Middleware->GetName(), *Result.ErrorMessage);

                if (Result.RedirectRoute.IsSet())
                {
                    OutRedirectRoute = Result.RedirectRoute;
                }

                return false;
            }
        }

        for (const TSharedPtr<INavigationMiddleware> &Middleware : RegisteredMiddleware)
        {
            if (!Middleware.IsValid() || !Middleware->IsEnabled())
            {
                continue;
            }

            Middleware->OnBeforeNavigate(Context);
        }

        return true;
    }

    void FNavigationMiddlewareManager::ExecuteAfterHooks(const FNavigationContext &Context)
    {
        FScopeLock Lock(&MiddlewareLock);

        for (int32 i = RegisteredMiddleware.Num() - 1; i >= 0; --i)
        {
            const TSharedPtr<INavigationMiddleware> &Middleware = RegisteredMiddleware[i];

            if (!Middleware.IsValid() || !Middleware->IsEnabled())
            {
                continue;
            }

            Middleware->OnAfterNavigate(Context);
        }
    }

    TArray<TSharedPtr<INavigationMiddleware>> FNavigationMiddlewareManager::GetRegisteredMiddleware() const
    {
        FScopeLock Lock(&MiddlewareLock);
        return RegisteredMiddleware;
    }

    void FNavigationMiddlewareManager::ClearAllMiddleware()
    {
        FScopeLock Lock(&MiddlewareLock);

        RegisteredMiddleware.Empty();
    }

    void FNavigationMiddlewareManager::SortMiddleware()
    {
        RegisteredMiddleware.Sort([](const TSharedPtr<INavigationMiddleware> &A, const TSharedPtr<INavigationMiddleware> &B)
                                  { return A->GetPriority() > B->GetPriority(); });
    }

} // namespace ConvaiEditor
