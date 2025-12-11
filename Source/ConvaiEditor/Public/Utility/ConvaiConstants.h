/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiConstants.h
 *
 * Constants and definitions for ConvaiEditor plugin.
 */

#pragma once

#include "CoreMinimal.h"

namespace ConvaiEditor
{
    /** Constants namespace for ConvaiEditor plugin */
    namespace Constants
    {
        //----------------------------------------
        // Timer Intervals
        //----------------------------------------

        constexpr float WindowSizeCheckInterval = 0.1f;
        constexpr float AnimationUpdateInterval = 0.016f;
        constexpr float InputDebounceTime = 0.3f;
        constexpr float TooltipShowDelay = 0.5f;

        //----------------------------------------
        // API Headers
        //----------------------------------------

        const FString API_Key_Header = TEXT("CONVAI-API-KEY");
        const FString Auth_Token_Header = TEXT("API-AUTH-TOKEN");
        const FString User_Agent_Header = TEXT("X-UnrealEngine-Agent");
        const FString Content_Type_Header = TEXT("Content-Type");
        const FString JSON_Content_Type = TEXT("application/json");

        //----------------------------------------
        // API Key Validation
        //----------------------------------------

        const int32 MinApiKeyLength = 20;
        const int32 MaxApiKeyLength = 100;
        const int32 HexApiKeyLength = 32;
        const float ApiKeyValidationDebounceTime = 1.5f;
        const float ValidationCacheExpirationTime = 300.0f;

        //----------------------------------------
        // HTTP Response Codes
        //----------------------------------------

        const int32 MinValidResponseCode = 200;
        const int32 MaxValidResponseCode = 299;
        const int32 UnauthorizedResponseCode = 401;
        const int32 ForbiddenResponseCode = 403;
        const int32 NotFoundResponseCode = 404;
        const int32 InternalServerErrorResponseCode = 500;

        //----------------------------------------
        // Configuration Keys
        //----------------------------------------

        const FString ConfigKey_ApiKey = TEXT("ApiKey");
        const FString ConfigKey_AuthToken = TEXT("AuthToken");
        const FString ConfigKey_WelcomeCompleted = TEXT("welcome.completed");
        const FString ConfigKey_Theme = TEXT("Theme");
        const FString ConfigKey_Language = TEXT("Language");

        //----------------------------------------
        // UI Layout Constants
        //----------------------------------------

        /** Layout constants namespace */
        namespace Layout
        {
            namespace Window
            {
                const FVector2D DefaultSize(1024.0f, 600.0f);
                const float DefaultWidth = 1200.0f;
                const float DefaultHeight = 800.0f;
                const float MinWidth = 800.0f;
                const float MinHeight = 600.0f;
                const float HomePageSidebarWidth = 400.0f;
                const FVector2D LogoSize(135.0f, 29.0f);

                const float WelcomeWindowWidth = 640.0f;
                const float WelcomeWindowHeight = 800.0f;
                const float WelcomeWindowMinWidth = 640.0f;
                const float WelcomeWindowMinHeight = 800.0f;

                const float MainWindowWidth = 1500.0f;
                const float MainWindowHeight = 925.0f;
                const float MainWindowMinWidth = 1150.0f;
                const float MainWindowMinHeight = 56.0f;

                const float AuthWindowWidth = 900.0f;
                const float AuthWindowHeight = 600.0f;
                const float AuthWindowMinWidth = 700.0f;
                const float AuthWindowMinHeight = 450.0f;
            }

            namespace Components
            {
                namespace SampleCard
                {
                    const FVector2D Dimensions(450.0f, 338.0f);
                    const float ImageHeight = 150.0f;
                    const float TitleHeight = 28.0f;
                    const float TagHeight = 18.0f;
                    const float GradientHeight = 96.0f;
                    const float BorderThickness = 2.0f;
                }

                namespace WindowControlPanel
                {
                    const float SettingsButtonPaddingHorizontal = 12.0f;
                    const float SettingsButtonPaddingVertical = 0.0f;
                    const float ControlButtonPaddingHorizontal = 4.0f;
                    const float ControlButtonPaddingVertical = 0.0f;
                    const float IconSpacing = 8.0f;
                    const float DividerSideMargin = 4.0f;
                    const float DividerVerticalMargin = 8.0f;
                    const float ButtonWidth = 32.0f;
                    const float ButtonHeight = 32.0f;
                    const FVector2D ButtonSize(ButtonWidth, ButtonHeight);
                }

                namespace HomePageCard
                {
                    const FVector2D Dimensions(450.0f, 350.0f);
                    const float ImageHeight = 120.0f;
                    const float TitleHeight = 24.0f;
                    const FVector2D ContentPadding(12.0f, 8.0f);
                    const float BorderThickness = 2.0f;
                }

                namespace HomePageSidebar
                {
                    const int32 MaxAnnouncementsDisplay = 3;
                    const int32 MaxChangelogItemsVisible = 5;
                    const float ChangelogContentHeight = 120.0f;
                    const float ThinScrollBarThickness = 4.0f;
                }

                namespace StandardCard
                {
                    const float BorderThickness = 2.0f;
                }

                namespace ScrollBar
                {
                    const float Thickness = 2.0f;
                    const float VerticalPadding = 30.0f;
                }

                namespace AccountMenu
                {
                    const float Width = 280.0f;
                    const float BorderRadius = 12.0f;
                    const FMargin ContentPadding = FMargin(0.0f, 16.0f, 0.0f, 0.0f);
                    const float AvatarSize = 32.0f;
                    const float AvatarFontSize = 13.0f;
                    const float AvatarToTextSpacing = 10.0f;
                    const float UsernameToEmailSpacing = 2.0f;
                    const float UserInfoToButtonSpacing = 16.0f;
                    const float ButtonToDividerSpacing = 12.0f;
                    const float ItemHeight = 42.0f;
                    const float ItemPaddingHorizontal = 16.0f;
                    const float ItemPaddingVertical = 10.0f;
                    const float DividerThickness = 1.0f;
                    const FMargin DividerMargin = FMargin(0.0f, 0.0f);
                    const float UsernameFontSize = 13.0f;
                    const float EmailFontSize = 10.0f;
                    const float ItemTextFontSize = 12.0f;
                    const FVector2D IconSize = FVector2D(14.0f, 14.0f);
                    const float IconTextSpacing = 8.0f;
                    const float BorderThickness = 1.0f;
                    const float ManageButtonHeight = 28.0f;
                    const float ManageButtonRadius = 5.0f;
                    const float ManageButtonBorderWidth = 1.5f;
                }

                namespace ProgressBar
                {
                    const float Height = 8.0f;
                    const float AccountHeight = 8.0f;
                }

                namespace Separator
                {
                    const float Thickness = 2.0f;
                }

                namespace SamplesPage
                {
                    const FVector2D OuterPadding(18.0f, 0.0f);
                }
            }

            namespace Spacing
            {
                const float Nav = 12.0f;
                const float DropdownY = 6.0f;
                const float Window = 24.0f;
                const float Content = 24.0f;
                const float SampleCardPadding = 12.0f;
                const float SampleCardSpacing = 24.0f;
                const float HomePageCardSpacing = 32.0f;
                const float HomePageSidebarSpacing = 32.0f;
                const float AccountSection = 24.0f;
                const float SpaceBelowTitle = 10.0f;
                const float AccountHorizontal = 10.0f;
                const float HeaderPaddingTop = 4.0f;
                const float HeaderPaddingBottom = 4.0f;
                const float PaddingWindow = 24.0f;
                const float PaddingContent = 24.0f;
                const float AccountSectionSpacing = 24.0f;
                const float AccountHorizontalSpacing = 10.0f;
                const float ApiKeyIconUniformPadding = 8.0f;
                const float ScrollBarVerticalPadding = 30.0f;

                namespace Button
                {
                    const float Padding = 12.0f;
                }

                namespace AccountBox
                {
                    const float Horizontal = 16.0f;
                    const float VerticalOuter = 16.0f;
                    const float VerticalInner = 8.0f;
                }
            }

            namespace Radius
            {
                const float ContentContainer = 25.0f;
                const float Dropdown = 16.0f;
                const float Separator = 2.0f;
                const float SampleCard = 25.0f;
                const float SampleCardMask = 25.0f;
                const float HomePageCard = 25.0f;
                const float SampleCardTag = 8.0f;
                const float StandardCard = 10.0f;
                const float AccountProgressBar = 4.0f;
                const float CardCornerSmoothing = 1.5f;
                const float DevInfoBox = 8.0f;
            }

            namespace Icons
            {
                const FVector2D Home(28.0f, 28.0f);
                const FVector2D Settings(20.0f, 20.0f);
                const FVector2D VisibilityToggle(22.0f, 19.0f);
                const FVector2D Logo(135.0f, 29.0f);
                const FVector2D ConvaiLogo(135.0f, 29.0f);
                const FVector2D Gradient_1x256(1.0f, 256.0f);
                const FVector2D Documentation(350.0f, 550.0f);
                const FVector2D YoutubeTutorials(350.0f, 550.0f);
                const FVector2D ConvaiDeveloperForum(350.0f, 550.0f);
                const FVector2D Actions(16.0f, 16.0f);
                const FVector2D NarrativeDesign(16.0f, 16.0f);
                const FVector2D LongTermMemory(16.0f, 16.0f);
                const FVector2D OpenExternally(16.0f, 16.0f);
                const FVector2D Toggle(16.0f, 16.0f);
                const FVector2D ExternalLink(16.0f, 16.0f);
                const FVector2D SignOut(16.0f, 16.0f);
                const FVector2D WelcomeBanner(1920.0f, 400.0f);
                const FVector2D Minimize(12.0f, 12.0f);
                const FVector2D Maximize(12.0f, 12.0f);
                const FVector2D Restore(12.0f, 12.0f);
                const FVector2D Close(12.0f, 12.0f);
                const FVector2D Icon16(16.0f, 16.0f);
                const FVector2D Icon40(40.0f, 40.0f);
            }
        }

        //----------------------------------------
        // Typography
        //----------------------------------------
        namespace Typography
        {
            const FString DefaultFontFamily = TEXT("IBMPlexSans-Regular.ttf");
            const FString MediumFontFamily = TEXT("IBMPlexSans-Medium.ttf");
            const FString BoldFontFamily = TEXT("IBMPlexSans-Bold.ttf");

            namespace FontFiles
            {
                const FString Regular = TEXT("IBMPlexSans-Regular.ttf");
                const FString Medium = TEXT("IBMPlexSans-Medium.ttf");
                const FString Bold = TEXT("IBMPlexSans-Bold.ttf");
            }

            namespace Sizes
            {
                const float ExtraSmall = 9.0f;
                const float Small = 12.0f;
                const float Regular = 14.0f;
                const float Medium = 16.0f;
                const float Large = 18.0f;
                const float Heading = 24.0f;
                const float Title = 32.0f;
            }

            namespace Styles
            {
                namespace Nav
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 24.0f;
                }

                namespace Dropdown
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 16.0f;
                }

                namespace DropdownNav
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 14.0f;
                }

                namespace DropdownIcon
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 8.0f;
                }

                namespace SampleCardTitle
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 20.0f;
                }

                namespace SampleCardTag
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 10.0f;
                }

                namespace AccountSectionTitle
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 20.0f;
                }

                namespace AccountLabel
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 14.0f;
                }

                namespace AccountValue
                {
                    const FString Family = TEXT("IBMPlexSansRegular");
                    const float Size = 14.0f;
                }

                namespace SupportResourceLabel
                {
                    const FString Family = TEXT("IBMPlexSansMedium");
                    const float Size = 20.0f;
                }

                namespace InfoBox
                {
                    const FString Family = TEXT("IBMPlexSansRegular");
                    const float Size = 12.0f;
                }
            }
        }

        //----------------------------------------
        // Icon Paths
        //----------------------------------------
        namespace Icons
        {
            /** Logo icon path */
            const FString Logo = TEXT("Icons/ConvaiLogo.png");

            /** Home icon path */
            const FString Home = TEXT("Icons/HomeIcon.png");

            /** Settings icon path */
            const FString Settings = TEXT("Icons/SettingsIcon.png");

            /** Eye visible icon path */
            const FString EyeVisible = TEXT("Icons/EyeVisible.png");

            /** Eye hidden icon path */
            const FString EyeHidden = TEXT("Icons/EyeHidden.png");

            /** Actions icon path */
            const FString Actions = TEXT("Icons/Actions.png");

            /** Narrative design icon path */
            const FString NarrativeDesign = TEXT("Icons/NarrativeDesign.png");

            /** Long term memory icon path */
            const FString LongTermMemory = TEXT("Icons/LongTermMemory.png");

            /** Open externally icon path */
            const FString OpenExternally = TEXT("Icons/OpenExternally.png");

            /** Toggle icon path */
            const FString Toggle = TEXT("Icons/Toggle.png");

            /** Minimize window icon path */
            const FString Minimize = TEXT("Icons/Minimize.png");

            /** Maximize window icon path */
            const FString Maximize = TEXT("Icons/Maximize.png");

            /** Restore window icon path */
            const FString Restore = TEXT("Icons/Restore Down.png");

            /** Close window icon path */
            const FString Close = TEXT("Icons/Close.png");
        }

        //----------------------------------------
        // Plugin Resource Paths
        //----------------------------------------
        namespace PluginResources
        {
            /** Plugin resources root directory */
            const FString Root = TEXT("Resources/ConvaiEditor");

            /** Plugin themes directory */
            const FString Themes = TEXT("Resources/ConvaiEditor/Themes");

            /** Plugin fonts directory */
            const FString Fonts = TEXT("Resources/ConvaiEditor/Fonts");

            /** Plugin images directory */
            const FString Images = TEXT("Resources/ConvaiEditor/Images");

            /** Plugin icons directory */
            const FString Icons = TEXT("Resources/ConvaiEditor/Icons");
        }

        //----------------------------------------
        // Material Paths
        //----------------------------------------
        namespace Materials
        {
            /** Rounded mask material for UI cards */
            const FString RoundedMask = TEXT("/Convai/Editor/M_UI_RoundedMask.M_UI_RoundedMask");
        }

        //----------------------------------------
        // Image Paths
        //----------------------------------------
        namespace Images
        {
            /** Gradient image path */
            const FString Gradient_1x256 = TEXT("Images/Gradient_1x256.png");

            namespace HomePage
            {
                /** Dashboard image path */
                const FString Dashboard = TEXT("Images/HomePage/Dashboard.png");

                /** Configurations image path */
                const FString Configurations = TEXT("Images/HomePage/Configurations.png");

                /** Convai experiences image path */
                const FString ConvaiExperiences = TEXT("Images/HomePage/ConvaiExperiences.png");
            }

            namespace Support
            {
                /** Documentation image path */
                const FString Documentation = TEXT("Images/Support/Documentation.png");

                /** YouTube tutorials image path */
                const FString YoutubeTutorials = TEXT("Images/Support/YoutubeTutorials.png");

                /** Convai developer forum image path */
                const FString ConvaiDeveloperForum = TEXT("Images/Support/ConvaiDeveloperForum.png");
            }

            namespace Samples
            {
                /** Sample 1 image path */
                const FString Sample1 = TEXT("Images/Samples/Sample1.png");

                /** Sample 2 image path */
                const FString Sample2 = TEXT("Images/Samples/Sample2.png");

                /** Sample 3 image path */
                const FString Sample3 = TEXT("Images/Samples/Sample3.png");

                /** Sample 4 image path */
                const FString Sample4 = TEXT("Images/Samples/Sample4.png");

                /** Sample 5 image path */
                const FString Sample5 = TEXT("Images/Samples/Sample5.png");

                /** Sample 6 image path */
                const FString Sample6 = TEXT("Images/Samples/Sample6.png");
            }
        }

        //----------------------------------------
        // OAuth Configuration
        //----------------------------------------
        namespace OAuth
        {
            /** Login URL format for OAuth authentication */
            inline const TCHAR *LoginUrlFormat = TEXT("https://login.convai.com/?ue=true&port=%d");

            /** Default ports to try for OAuth local HTTP server */
            inline const TArray<int32> DefaultPorts = {8080, 8081, 8082, 8083};

            /** OAuth callback timeout in seconds */
            constexpr float CallbackTimeoutSeconds = 120.0f;

            /** OAuth token refresh interval in seconds */
            constexpr float TokenRefreshIntervalSeconds = 3600.0f;

            /** OAuth window close delay after success (seconds) */
            constexpr float WindowCloseDelaySeconds = 1.5f;
        }

        //----------------------------------------
        // Validation Patterns
        //----------------------------------------

        /** Valid characters for API key */
        const FString ValidApiKeyCharacters = TEXT("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-");

        /** Valid characters for Auth token */
        const FString ValidAuthTokenCharacters = TEXT("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-.");
    } // namespace Constants
} // namespace ConvaiEditor
