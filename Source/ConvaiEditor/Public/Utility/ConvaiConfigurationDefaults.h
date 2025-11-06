/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiConfigurationDefaults.h
 *
 * Centralized configuration defaults and schema definitions.
 */

#pragma once

#include "CoreMinimal.h"
#include "Utility/ConvaiConstants.h"
#include "Services/Configuration/IConfigurationValidator.h"

/** Configuration defaults namespace */
namespace ConvaiEditor::Configuration::Defaults
{
    /** Current configuration schema version */
    constexpr int32 CURRENT_SCHEMA_VERSION = 1;

    namespace Keys
    {
        inline const FString WINDOW_INITIAL_WIDTH = TEXT("window.initialWidth");
        inline const FString WINDOW_INITIAL_HEIGHT = TEXT("window.initialHeight");
        inline const FString WINDOW_MIN_WIDTH = TEXT("window.minWidth");
        inline const FString WINDOW_MIN_HEIGHT = TEXT("window.minHeight");

        inline const FString THEME_ID = TEXT("theme.id");

        inline const FString PERFORMANCE_ENABLE_CACHING = TEXT("performance.enableCaching");
        inline const FString PERFORMANCE_CACHE_TTL_SECONDS = TEXT("performance.cacheTTLSeconds");

        inline const FString NAVIGATION_MAX_HISTORY_SIZE = TEXT("navigation.maxHistorySize");
        inline const FString NAVIGATION_ENABLE_HISTORY_PERSISTENCE = TEXT("navigation.enableHistoryPersistence");

        inline const FString UPDATE_CHECK_ENABLED = TEXT("updateCheck.enabled");
        inline const FString UPDATE_CHECK_INTERVAL_HOURS = TEXT("updateCheck.intervalHours");

        inline const FString DEBUG_ENABLE_VERBOSE_LOGGING = TEXT("debug.enableVerboseLogging");
        inline const FString DEBUG_ENABLE_PERFORMANCE_TRACKING = TEXT("debug.enablePerformanceTracking");

        inline const FString PRIVACY_TELEMETRY_ENABLED = TEXT("privacy.telemetryEnabled");
        inline const FString PRIVACY_CRASH_REPORTING_ENABLED = TEXT("privacy.crashReportingEnabled");

        inline const FString USER_INFO_USERNAME = TEXT("userInfo.username");
        inline const FString USER_INFO_EMAIL = TEXT("userInfo.email");

        inline const FString WELCOME_COMPLETED = TEXT("welcome.completed");

        inline const FString META_CONFIG_VERSION = TEXT("meta.configVersion");
        inline const FString META_LAST_MODIFIED = TEXT("meta.lastModified");
    }

    namespace Values
    {
        constexpr int32 WINDOW_INITIAL_WIDTH = static_cast<int32>(Constants::Layout::Window::MainWindowWidth);
        constexpr int32 WINDOW_INITIAL_HEIGHT = static_cast<int32>(Constants::Layout::Window::MainWindowHeight);
        constexpr float WINDOW_MIN_WIDTH = Constants::Layout::Window::MainWindowMinWidth;
        constexpr float WINDOW_MIN_HEIGHT = Constants::Layout::Window::MainWindowMinHeight;

        inline const FString THEME_ID = TEXT("dark");

        constexpr bool PERFORMANCE_ENABLE_CACHING = true;
        constexpr int32 PERFORMANCE_CACHE_TTL_SECONDS = 3600;

        constexpr int32 NAVIGATION_MAX_HISTORY_SIZE = 50;
        constexpr bool NAVIGATION_ENABLE_HISTORY_PERSISTENCE = false;

        constexpr bool UPDATE_CHECK_ENABLED = true;
        constexpr int32 UPDATE_CHECK_INTERVAL_HOURS = 24;

        constexpr bool DEBUG_ENABLE_VERBOSE_LOGGING = false;
        constexpr bool DEBUG_ENABLE_PERFORMANCE_TRACKING = false;

        constexpr bool PRIVACY_TELEMETRY_ENABLED = false;
        constexpr bool PRIVACY_CRASH_REPORTING_ENABLED = true;

        inline const FString USER_INFO_USERNAME = TEXT("");
        inline const FString USER_INFO_EMAIL = TEXT("");

        constexpr bool WELCOME_COMPLETED = false;
    }

    namespace Types
    {
        inline const FString INT = TEXT("int");
        inline const FString FLOAT = TEXT("float");
        inline const FString STRING = TEXT("string");
        inline const FString BOOL = TEXT("bool");
    }

    namespace Constraints
    {
        constexpr int32 WINDOW_MIN_WIDTH_VALUE = 55;
        constexpr int32 WINDOW_MIN_HEIGHT_VALUE = 55;
        constexpr int32 WINDOW_MAX_WIDTH_VALUE = 7680;
        constexpr int32 WINDOW_MAX_HEIGHT_VALUE = 4320;

        constexpr int32 CACHE_TTL_MIN_SECONDS = 60;
        constexpr int32 CACHE_TTL_MAX_SECONDS = 86400;

        constexpr int32 HISTORY_SIZE_MIN = 10;
        constexpr int32 HISTORY_SIZE_MAX = 1000;

        constexpr int32 UPDATE_CHECK_INTERVAL_MIN_HOURS = 1;
        constexpr int32 UPDATE_CHECK_INTERVAL_MAX_HOURS = 168;

        inline const TArray<FString> VALID_THEME_IDS = {
            TEXT("dark"),
            TEXT("light"),
            TEXT("high-contrast")};
    }

    /** Build the complete configuration schema */
    inline struct FConfigurationSchema BuildDefaultSchema()
    {
        struct FConfigurationSchema Schema;
        Schema.Version = CURRENT_SCHEMA_VERSION;

        Schema.ExpectedTypes.Add(Keys::WINDOW_INITIAL_WIDTH, Types::INT);
        Schema.ExpectedTypes.Add(Keys::WINDOW_INITIAL_HEIGHT, Types::INT);
        Schema.ExpectedTypes.Add(Keys::WINDOW_MIN_WIDTH, Types::FLOAT);
        Schema.ExpectedTypes.Add(Keys::WINDOW_MIN_HEIGHT, Types::FLOAT);
        Schema.ExpectedTypes.Add(Keys::THEME_ID, Types::STRING);
        Schema.ExpectedTypes.Add(Keys::PERFORMANCE_ENABLE_CACHING, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::PERFORMANCE_CACHE_TTL_SECONDS, Types::INT);
        Schema.ExpectedTypes.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE, Types::INT);
        Schema.ExpectedTypes.Add(Keys::NAVIGATION_ENABLE_HISTORY_PERSISTENCE, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::UPDATE_CHECK_ENABLED, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::UPDATE_CHECK_INTERVAL_HOURS, Types::INT);
        Schema.ExpectedTypes.Add(Keys::DEBUG_ENABLE_VERBOSE_LOGGING, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::DEBUG_ENABLE_PERFORMANCE_TRACKING, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::PRIVACY_TELEMETRY_ENABLED, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::PRIVACY_CRASH_REPORTING_ENABLED, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::USER_INFO_USERNAME, Types::STRING);
        Schema.ExpectedTypes.Add(Keys::USER_INFO_EMAIL, Types::STRING);
        Schema.ExpectedTypes.Add(Keys::WELCOME_COMPLETED, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::META_CONFIG_VERSION, Types::INT);
        Schema.ExpectedTypes.Add(Keys::META_LAST_MODIFIED, Types::STRING);

        Schema.RequiredKeys.Add(Keys::WINDOW_INITIAL_WIDTH);
        Schema.RequiredKeys.Add(Keys::WINDOW_INITIAL_HEIGHT);
        Schema.RequiredKeys.Add(Keys::THEME_ID);
        Schema.RequiredKeys.Add(Keys::META_CONFIG_VERSION);

        Schema.OptionalKeys.Add(Keys::WINDOW_MIN_WIDTH);
        Schema.OptionalKeys.Add(Keys::WINDOW_MIN_HEIGHT);
        Schema.OptionalKeys.Add(Keys::PERFORMANCE_ENABLE_CACHING);
        Schema.OptionalKeys.Add(Keys::PERFORMANCE_CACHE_TTL_SECONDS);
        Schema.OptionalKeys.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE);
        Schema.OptionalKeys.Add(Keys::NAVIGATION_ENABLE_HISTORY_PERSISTENCE);
        Schema.OptionalKeys.Add(Keys::UPDATE_CHECK_ENABLED);
        Schema.OptionalKeys.Add(Keys::UPDATE_CHECK_INTERVAL_HOURS);
        Schema.OptionalKeys.Add(Keys::DEBUG_ENABLE_VERBOSE_LOGGING);
        Schema.OptionalKeys.Add(Keys::DEBUG_ENABLE_PERFORMANCE_TRACKING);
        Schema.OptionalKeys.Add(Keys::PRIVACY_TELEMETRY_ENABLED);
        Schema.OptionalKeys.Add(Keys::PRIVACY_CRASH_REPORTING_ENABLED);
        Schema.OptionalKeys.Add(Keys::USER_INFO_USERNAME);
        Schema.OptionalKeys.Add(Keys::USER_INFO_EMAIL);
        Schema.OptionalKeys.Add(Keys::WELCOME_COMPLETED);
        Schema.OptionalKeys.Add(Keys::META_LAST_MODIFIED);

        Schema.Constraints.Add(Keys::WINDOW_INITIAL_WIDTH,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_WIDTH_VALUE, Constraints::WINDOW_MAX_WIDTH_VALUE));
        Schema.Constraints.Add(Keys::WINDOW_INITIAL_HEIGHT,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_HEIGHT_VALUE, Constraints::WINDOW_MAX_HEIGHT_VALUE));
        Schema.Constraints.Add(Keys::WINDOW_MIN_WIDTH,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_WIDTH_VALUE, Constraints::WINDOW_MAX_WIDTH_VALUE));
        Schema.Constraints.Add(Keys::WINDOW_MIN_HEIGHT,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_HEIGHT_VALUE, Constraints::WINDOW_MAX_HEIGHT_VALUE));
        Schema.Constraints.Add(Keys::THEME_ID, TEXT("enum(dark,light,high-contrast)"));
        Schema.Constraints.Add(Keys::PERFORMANCE_CACHE_TTL_SECONDS,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::CACHE_TTL_MIN_SECONDS, Constraints::CACHE_TTL_MAX_SECONDS));
        Schema.Constraints.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::HISTORY_SIZE_MIN, Constraints::HISTORY_SIZE_MAX));
        Schema.Constraints.Add(Keys::UPDATE_CHECK_INTERVAL_HOURS,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::UPDATE_CHECK_INTERVAL_MIN_HOURS, Constraints::UPDATE_CHECK_INTERVAL_MAX_HOURS));

        Schema.Defaults.Add(Keys::WINDOW_INITIAL_WIDTH, FString::FromInt(Values::WINDOW_INITIAL_WIDTH));
        Schema.Defaults.Add(Keys::WINDOW_INITIAL_HEIGHT, FString::FromInt(Values::WINDOW_INITIAL_HEIGHT));
        Schema.Defaults.Add(Keys::WINDOW_MIN_WIDTH, FString::SanitizeFloat(Values::WINDOW_MIN_WIDTH));
        Schema.Defaults.Add(Keys::WINDOW_MIN_HEIGHT, FString::SanitizeFloat(Values::WINDOW_MIN_HEIGHT));
        Schema.Defaults.Add(Keys::THEME_ID, Values::THEME_ID);
        Schema.Defaults.Add(Keys::PERFORMANCE_ENABLE_CACHING, Values::PERFORMANCE_ENABLE_CACHING ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::PERFORMANCE_CACHE_TTL_SECONDS, FString::FromInt(Values::PERFORMANCE_CACHE_TTL_SECONDS));
        Schema.Defaults.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE, FString::FromInt(Values::NAVIGATION_MAX_HISTORY_SIZE));
        Schema.Defaults.Add(Keys::NAVIGATION_ENABLE_HISTORY_PERSISTENCE, Values::NAVIGATION_ENABLE_HISTORY_PERSISTENCE ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::UPDATE_CHECK_ENABLED, Values::UPDATE_CHECK_ENABLED ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::UPDATE_CHECK_INTERVAL_HOURS, FString::FromInt(Values::UPDATE_CHECK_INTERVAL_HOURS));
        Schema.Defaults.Add(Keys::DEBUG_ENABLE_VERBOSE_LOGGING, Values::DEBUG_ENABLE_VERBOSE_LOGGING ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::DEBUG_ENABLE_PERFORMANCE_TRACKING, Values::DEBUG_ENABLE_PERFORMANCE_TRACKING ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::PRIVACY_TELEMETRY_ENABLED, Values::PRIVACY_TELEMETRY_ENABLED ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::PRIVACY_CRASH_REPORTING_ENABLED, Values::PRIVACY_CRASH_REPORTING_ENABLED ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::USER_INFO_USERNAME, Values::USER_INFO_USERNAME);
        Schema.Defaults.Add(Keys::USER_INFO_EMAIL, Values::USER_INFO_EMAIL);
        Schema.Defaults.Add(Keys::WELCOME_COMPLETED, Values::WELCOME_COMPLETED ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::META_CONFIG_VERSION, FString::FromInt(CURRENT_SCHEMA_VERSION));

        return Schema;
    }
}
