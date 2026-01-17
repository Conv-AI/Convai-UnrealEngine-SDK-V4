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
        inline const FString EDITOR_UI_ENABLED = TEXT("editorUI.enabled");

        inline const FString WINDOW_INITIAL_WIDTH = TEXT("window.initialWidth");
        inline const FString WINDOW_INITIAL_HEIGHT = TEXT("window.initialHeight");
        inline const FString WINDOW_MIN_WIDTH = TEXT("window.minWidth");
        inline const FString WINDOW_MIN_HEIGHT = TEXT("window.minHeight");

        inline const FString THEME_ID = TEXT("theme.id");

        inline const FString NAVIGATION_MAX_HISTORY_SIZE = TEXT("navigation.maxHistorySize");

        inline const FString USER_INFO_USERNAME = TEXT("userInfo.username");
        inline const FString USER_INFO_EMAIL = TEXT("userInfo.email");

        inline const FString WELCOME_COMPLETED = TEXT("welcome.completed");

        inline const FString META_CONFIG_VERSION = TEXT("meta.configVersion");
    }

    namespace Values
    {
        constexpr bool EDITOR_UI_ENABLED = true;

        constexpr int32 WINDOW_INITIAL_WIDTH = static_cast<int32>(Constants::Layout::Window::MainWindowWidth);
        constexpr int32 WINDOW_INITIAL_HEIGHT = static_cast<int32>(Constants::Layout::Window::MainWindowHeight);
        constexpr float WINDOW_MIN_WIDTH = Constants::Layout::Window::MainWindowMinWidth;
        constexpr float WINDOW_MIN_HEIGHT = Constants::Layout::Window::MainWindowMinHeight;

        inline const FString THEME_ID = TEXT("dark");

        constexpr int32 NAVIGATION_MAX_HISTORY_SIZE = 50;

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

        constexpr int32 HISTORY_SIZE_MIN = 10;
        constexpr int32 HISTORY_SIZE_MAX = 1000;

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

        Schema.ExpectedTypes.Add(Keys::EDITOR_UI_ENABLED, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::WINDOW_INITIAL_WIDTH, Types::INT);
        Schema.ExpectedTypes.Add(Keys::WINDOW_INITIAL_HEIGHT, Types::INT);
        Schema.ExpectedTypes.Add(Keys::WINDOW_MIN_WIDTH, Types::FLOAT);
        Schema.ExpectedTypes.Add(Keys::WINDOW_MIN_HEIGHT, Types::FLOAT);
        Schema.ExpectedTypes.Add(Keys::THEME_ID, Types::STRING);
        Schema.ExpectedTypes.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE, Types::INT);
        Schema.ExpectedTypes.Add(Keys::USER_INFO_USERNAME, Types::STRING);
        Schema.ExpectedTypes.Add(Keys::USER_INFO_EMAIL, Types::STRING);
        Schema.ExpectedTypes.Add(Keys::WELCOME_COMPLETED, Types::BOOL);
        Schema.ExpectedTypes.Add(Keys::META_CONFIG_VERSION, Types::INT);

        Schema.RequiredKeys.Add(Keys::EDITOR_UI_ENABLED);
        Schema.RequiredKeys.Add(Keys::WINDOW_INITIAL_WIDTH);
        Schema.RequiredKeys.Add(Keys::WINDOW_INITIAL_HEIGHT);
        Schema.RequiredKeys.Add(Keys::THEME_ID);
        Schema.RequiredKeys.Add(Keys::META_CONFIG_VERSION);

        Schema.OptionalKeys.Add(Keys::WINDOW_MIN_WIDTH);
        Schema.OptionalKeys.Add(Keys::WINDOW_MIN_HEIGHT);
        Schema.OptionalKeys.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE);
        Schema.OptionalKeys.Add(Keys::USER_INFO_USERNAME);
        Schema.OptionalKeys.Add(Keys::USER_INFO_EMAIL);
        Schema.OptionalKeys.Add(Keys::WELCOME_COMPLETED);

        Schema.Constraints.Add(Keys::WINDOW_INITIAL_WIDTH,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_WIDTH_VALUE, Constraints::WINDOW_MAX_WIDTH_VALUE));
        Schema.Constraints.Add(Keys::WINDOW_INITIAL_HEIGHT,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_HEIGHT_VALUE, Constraints::WINDOW_MAX_HEIGHT_VALUE));
        Schema.Constraints.Add(Keys::WINDOW_MIN_WIDTH,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_WIDTH_VALUE, Constraints::WINDOW_MAX_WIDTH_VALUE));
        Schema.Constraints.Add(Keys::WINDOW_MIN_HEIGHT,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::WINDOW_MIN_HEIGHT_VALUE, Constraints::WINDOW_MAX_HEIGHT_VALUE));
        Schema.Constraints.Add(Keys::THEME_ID, TEXT("enum(dark,light,high-contrast)"));
        Schema.Constraints.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE,
                               FString::Printf(TEXT("range(%d,%d)"), Constraints::HISTORY_SIZE_MIN, Constraints::HISTORY_SIZE_MAX));

        Schema.Defaults.Add(Keys::EDITOR_UI_ENABLED, Values::EDITOR_UI_ENABLED ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::WINDOW_INITIAL_WIDTH, FString::FromInt(Values::WINDOW_INITIAL_WIDTH));
        Schema.Defaults.Add(Keys::WINDOW_INITIAL_HEIGHT, FString::FromInt(Values::WINDOW_INITIAL_HEIGHT));
        Schema.Defaults.Add(Keys::WINDOW_MIN_WIDTH, FString::SanitizeFloat(Values::WINDOW_MIN_WIDTH));
        Schema.Defaults.Add(Keys::WINDOW_MIN_HEIGHT, FString::SanitizeFloat(Values::WINDOW_MIN_HEIGHT));
        Schema.Defaults.Add(Keys::THEME_ID, Values::THEME_ID);
        Schema.Defaults.Add(Keys::NAVIGATION_MAX_HISTORY_SIZE, FString::FromInt(Values::NAVIGATION_MAX_HISTORY_SIZE));
        Schema.Defaults.Add(Keys::USER_INFO_USERNAME, Values::USER_INFO_USERNAME);
        Schema.Defaults.Add(Keys::USER_INFO_EMAIL, Values::USER_INFO_EMAIL);
        Schema.Defaults.Add(Keys::WELCOME_COMPLETED, Values::WELCOME_COMPLETED ? TEXT("true") : TEXT("false"));
        Schema.Defaults.Add(Keys::META_CONFIG_VERSION, FString::FromInt(CURRENT_SCHEMA_VERSION));

        return Schema;
    }
}
