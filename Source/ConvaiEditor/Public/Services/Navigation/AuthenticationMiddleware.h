/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AuthenticationMiddleware.h
 *
 * Middleware for route authentication protection.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Navigation/INavigationMiddleware.h"

namespace ConvaiEditor
{
    /**
     * Protects routes that require authentication.
     */
    class CONVAIEDITOR_API FAuthenticationMiddleware : public INavigationMiddleware
    {
    public:
        /** Constructor */
        FAuthenticationMiddleware();

        virtual FNavigationMiddlewareResult CanNavigate(const FNavigationContext &Context) override;
        virtual void OnBeforeNavigate(const FNavigationContext &Context) override;
        virtual void OnAfterNavigate(const FNavigationContext &Context) override;
        virtual int32 GetPriority() const override { return 100; }
        virtual FString GetName() const override { return TEXT("Authentication"); }

        bool IsProtectedRoute(Route::E Route) const;
        void AddProtectedRoute(Route::E Route);
        void RemoveProtectedRoute(Route::E Route);
        const TSet<Route::E> &GetProtectedRoutes() const { return ProtectedRoutes; }

    private:
        bool IsUserAuthenticated() const;
        void TriggerWelcomeRedirect() const;

        TSet<Route::E> ProtectedRoutes;
    };

} // namespace ConvaiEditor
