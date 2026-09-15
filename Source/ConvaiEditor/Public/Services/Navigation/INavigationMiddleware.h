/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * INavigationMiddleware.h
 *
 * Interface for navigation middleware.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Routes.h"

// Forward declarations
class FJsonObject;

namespace ConvaiEditor
{
    /**
     * Navigation context information.
     */
    struct CONVAIEDITOR_API FNavigationContext
    {
        /** Source route */
        Route::E FromRoute;

        /** Destination route */
        Route::E ToRoute;

        /** Optional state data */
        TSharedPtr<FJsonObject> State;

        /** Timestamp when navigation started */
        double StartTime;

        /** User-defined metadata */
        TMap<FString, FString> Metadata;

        /** Constructor */
        FNavigationContext(Route::E From = Route::E::None, Route::E To = Route::E::None, TSharedPtr<FJsonObject> InState = nullptr)
            : FromRoute(From), ToRoute(To), State(InState), StartTime(FPlatformTime::Seconds())
        {
        }
    };

    /**
     * Navigation middleware result.
     */
    struct CONVAIEDITOR_API FNavigationMiddlewareResult
    {
        /** Whether navigation should proceed */
        bool bShouldProceed;

        /** Optional error message if navigation is blocked */
        FString ErrorMessage;

        /** Optional redirect route if navigation should be redirected */
        TOptional<Route::E> RedirectRoute;

        /** Static factory methods */
        static FNavigationMiddlewareResult Allow()
        {
            return FNavigationMiddlewareResult{true, FString(), TOptional<Route::E>()};
        }

        static FNavigationMiddlewareResult Block(const FString &Reason)
        {
            return FNavigationMiddlewareResult{false, Reason, TOptional<Route::E>()};
        }

        static FNavigationMiddlewareResult Redirect(Route::E NewRoute, const FString &Reason = FString())
        {
            FNavigationMiddlewareResult Result;
            Result.bShouldProceed = false;
            Result.ErrorMessage = Reason;
            Result.RedirectRoute = NewRoute;
            return Result;
        }
    };

    /**
     * Interface for navigation middleware.
     */
    class CONVAIEDITOR_API INavigationMiddleware
    {
    public:
        virtual ~INavigationMiddleware() = default;

        /** Checks if navigation is allowed */
        virtual FNavigationMiddlewareResult CanNavigate(const FNavigationContext &Context) = 0;

        /** Called before navigation */
        virtual void OnBeforeNavigate(const FNavigationContext &Context) = 0;

        /** Called after navigation */
        virtual void OnAfterNavigate(const FNavigationContext &Context) = 0;

        /** Returns middleware priority */
        virtual int32 GetPriority() const = 0;

        /** Returns middleware name */
        virtual FString GetName() const = 0;

        /** Returns true if middleware is enabled */
        virtual bool IsEnabled() const { return true; }
    };

} // namespace ConvaiEditor
