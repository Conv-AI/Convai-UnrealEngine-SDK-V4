/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * LoggingMiddleware.h
 *
 * Middleware for navigation logging and analytics.
 */

#pragma once

#include "CoreMinimal.h"
#include "Services/Navigation/INavigationMiddleware.h"

namespace ConvaiEditor
{
    /**
     * Navigation analytics data.
     */
    struct CONVAIEDITOR_API FNavigationAnalytics
    {
        /** Source route */
        Route::E FromRoute;

        /** Destination route */
        Route::E ToRoute;

        /** Navigation duration in seconds */
        double Duration;

        /** Timestamp when navigation occurred */
        FDateTime Timestamp;

        /** Whether navigation was successful */
        bool bSuccessful;

        /** Constructor */
        FNavigationAnalytics()
            : FromRoute(Route::E::None), ToRoute(Route::E::None), Duration(0.0), Timestamp(FDateTime::Now()), bSuccessful(false)
        {
        }
    };

    /**
     * Logs navigation events and collects analytics.
     */
    class CONVAIEDITOR_API FLoggingMiddleware : public INavigationMiddleware
    {
    public:
        /** Constructor */
        FLoggingMiddleware();

        /** INavigationMiddleware interface */
        virtual FNavigationMiddlewareResult CanNavigate(const FNavigationContext &Context) override;
        virtual void OnBeforeNavigate(const FNavigationContext &Context) override;
        virtual void OnAfterNavigate(const FNavigationContext &Context) override;
        virtual int32 GetPriority() const override { return 50; } // Medium priority
        virtual FString GetName() const override { return TEXT("Logging"); }

        /**
         * Get navigation analytics history
         * @return Array of navigation analytics
         */
        const TArray<FNavigationAnalytics> &GetAnalyticsHistory() const { return AnalyticsHistory; }

        /**
         * Get total navigation count
         * @return Total number of navigations
         */
        int32 GetTotalNavigationCount() const { return TotalNavigationCount; }

        /**
         * Get navigation count for a specific route
         * @param Route Route to query
         * @return Number of navigations to this route
         */
        int32 GetNavigationCountForRoute(Route::E Route) const;

        /**
         * Get average navigation duration
         * @return Average duration in seconds
         */
        double GetAverageNavigationDuration() const;

        /**
         * Clear analytics history
         */
        void ClearAnalytics();

        /**
         * Set maximum analytics history size
         * @param MaxSize Maximum number of analytics entries to keep
         */
        void SetMaxAnalyticsHistorySize(int32 MaxSize) { MaxAnalyticsHistorySize = MaxSize; }

    private:
        /**
         * Track navigation event
         * @param FromRoute Source route
         * @param ToRoute Destination route
         * @param Duration Navigation duration
         * @param bSuccessful Whether navigation was successful
         */
        void TrackNavigationEvent(Route::E FromRoute, Route::E ToRoute, double Duration, bool bSuccessful);

        /**
         * Get current navigation start time
         * @param Context Navigation context
         * @return Start time
         */
        double GetNavigationStartTime(const FNavigationContext &Context) const;

        /** Navigation analytics history */
        TArray<FNavigationAnalytics> AnalyticsHistory;

        /** Route navigation counts */
        TMap<Route::E, int32> RouteNavigationCounts;

        /** Total navigation count */
        int32 TotalNavigationCount;

        /** Maximum analytics history size */
        int32 MaxAnalyticsHistorySize;

        /** Navigation start times (keyed by route) */
        TMap<Route::E, double> NavigationStartTimes;

        /** Thread safety for analytics */
        mutable FCriticalSection AnalyticsLock;
    };

} // namespace ConvaiEditor
