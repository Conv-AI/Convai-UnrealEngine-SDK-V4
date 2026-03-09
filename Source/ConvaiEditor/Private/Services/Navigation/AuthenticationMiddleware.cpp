/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AuthenticationMiddleware.cpp
 *
 * Implementation of authentication navigation middleware.
 */

#include "Services/Navigation/AuthenticationMiddleware.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/Configuration/IAuthProvider.h"
#include "Services/IWelcomeWindowManager.h"
#include "Logging/ConvaiEditorNavigationLog.h"

namespace ConvaiEditor
{
    FAuthenticationMiddleware::FAuthenticationMiddleware()
    {
        ProtectedRoutes = {
            Route::E::Account,
            Route::E::Settings,
            Route::E::Dashboard,
        };
    }

    FNavigationMiddlewareResult FAuthenticationMiddleware::CanNavigate(const FNavigationContext &Context)
    {
        if (!IsProtectedRoute(Context.ToRoute))
        {
            return FNavigationMiddlewareResult::Allow();
        }

        if (IsUserAuthenticated())
        {
            return FNavigationMiddlewareResult::Allow();
        }

        UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("AuthenticationMiddleware: authentication required for route: %s"), *Route::ToString(Context.ToRoute));

        TriggerWelcomeRedirect();

        return FNavigationMiddlewareResult::Block(FString::Printf(TEXT("Authentication required to access %s"), *Route::ToString(Context.ToRoute)));
    }

    void FAuthenticationMiddleware::OnBeforeNavigate(const FNavigationContext &Context)
    {
        if (IsProtectedRoute(Context.ToRoute))
        {
        }
    }

    void FAuthenticationMiddleware::OnAfterNavigate(const FNavigationContext &Context)
    {
        if (IsProtectedRoute(Context.ToRoute))
        {
        }
    }

    bool FAuthenticationMiddleware::IsProtectedRoute(Route::E Route) const
    {
        return ProtectedRoutes.Contains(Route);
    }

    void FAuthenticationMiddleware::AddProtectedRoute(Route::E Route)
    {
        if (!ProtectedRoutes.Contains(Route))
        {
            ProtectedRoutes.Add(Route);
        }
    }

    void FAuthenticationMiddleware::RemoveProtectedRoute(Route::E Route)
    {
        if (ProtectedRoutes.Remove(Route) > 0)
        {
        }
    }

    bool FAuthenticationMiddleware::IsUserAuthenticated() const
    {
        auto AuthResult = FConvaiDIContainerManager::Get().Resolve<IAuthProvider>();

        if (AuthResult.IsFailure())
        {
            UE_LOG(LogConvaiEditorNavigation, Error, TEXT("AuthenticationMiddleware: authentication service unavailable"));
            return false;
        }

        TSharedPtr<IAuthProvider> AuthProvider = AuthResult.GetValue();

        bool bIsAuthenticated = AuthProvider->HasAuthentication();

        return bIsAuthenticated;
    }

    void FAuthenticationMiddleware::TriggerWelcomeRedirect() const
    {
        FConvaiDIContainerManager::Get()
            .Resolve<IWelcomeWindowManager>()
            .LogOnFailure(LogConvaiEditorNavigation, TEXT("AuthenticationMiddleware: welcome window service unavailable"))
            .Tap([](TSharedPtr<IWelcomeWindowManager> WelcomeManager)
                 { WelcomeManager->ShowWelcomeWindow(); });
    }

} // namespace ConvaiEditor
