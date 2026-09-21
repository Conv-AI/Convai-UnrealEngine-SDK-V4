/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * LoggingMiddleware.cpp
 *
 * Implementation of logging navigation middleware.
 */

#include "Services/Navigation/LoggingMiddleware.h"
#include "Logging/ConvaiEditorNavigationLog.h"
#include "HAL/PlatformTime.h"

namespace ConvaiEditor
{
    FLoggingMiddleware::FLoggingMiddleware()
        : TotalNavigationCount(0), MaxAnalyticsHistorySize(100)
    {
    }

    FNavigationMiddlewareResult FLoggingMiddleware::CanNavigate(const FNavigationContext &Context)
    {
        return FNavigationMiddlewareResult::Allow();
    }

    void FLoggingMiddleware::OnBeforeNavigate(const FNavigationContext &Context)
    {
        FScopeLock Lock(&AnalyticsLock);

        NavigationStartTimes.Add(Context.ToRoute, Context.StartTime);

        if (Context.State.IsValid())
        {
        }

        if (Context.Metadata.Num() > 0)
        {
        }
    }

    void FLoggingMiddleware::OnAfterNavigate(const FNavigationContext &Context)
    {
        FScopeLock Lock(&AnalyticsLock);

        double StartTime = NavigationStartTimes.FindRef(Context.ToRoute);
        double Duration = FPlatformTime::Seconds() - StartTime;

        NavigationStartTimes.Remove(Context.ToRoute);

        TrackNavigationEvent(Context.FromRoute, Context.ToRoute, Duration, true);

        if (Duration > 1.0)
        {
        }
    }

    int32 FLoggingMiddleware::GetNavigationCountForRoute(Route::E Route) const
    {
        FScopeLock Lock(&AnalyticsLock);
        return RouteNavigationCounts.FindRef(Route);
    }

    double FLoggingMiddleware::GetAverageNavigationDuration() const
    {
        FScopeLock Lock(&AnalyticsLock);

        if (AnalyticsHistory.Num() == 0)
        {
            return 0.0;
        }

        double TotalDuration = 0.0;
        for (const FNavigationAnalytics &Analytics : AnalyticsHistory)
        {
            TotalDuration += Analytics.Duration;
        }

        return TotalDuration / AnalyticsHistory.Num();
    }

    void FLoggingMiddleware::ClearAnalytics()
    {
        FScopeLock Lock(&AnalyticsLock);

        AnalyticsHistory.Empty();
        RouteNavigationCounts.Empty();
        TotalNavigationCount = 0;
    }

    void FLoggingMiddleware::TrackNavigationEvent(Route::E FromRoute, Route::E ToRoute, double Duration, bool bSuccessful)
    {
        FNavigationAnalytics Analytics;
        Analytics.FromRoute = FromRoute;
        Analytics.ToRoute = ToRoute;
        Analytics.Duration = Duration;
        Analytics.Timestamp = FDateTime::Now();
        Analytics.bSuccessful = bSuccessful;

        AnalyticsHistory.Add(Analytics);

        if (AnalyticsHistory.Num() > MaxAnalyticsHistorySize)
        {
            AnalyticsHistory.RemoveAt(0, AnalyticsHistory.Num() - MaxAnalyticsHistorySize);
        }

        int32 &Count = RouteNavigationCounts.FindOrAdd(ToRoute, 0);
        Count++;

        TotalNavigationCount++;
    }

    double FLoggingMiddleware::GetNavigationStartTime(const FNavigationContext &Context) const
    {
        return NavigationStartTimes.FindRef(Context.ToRoute);
    }

} // namespace ConvaiEditor
