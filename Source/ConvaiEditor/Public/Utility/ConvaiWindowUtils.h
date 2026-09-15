/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiWindowUtils.h
 *
 * Utility functions for window management and sizing.
 */

#pragma once

#include "CoreMinimal.h"
#include "Utility/ConvaiConstants.h"

namespace ConvaiEditor
{
    /** Window utility functions */
    namespace WindowUtils
    {
        /** Window dimension structure for consistent sizing */
        struct FWindowDimensions
        {
            /** Initial window width */
            float InitialWidth;

            /** Initial window height */
            float InitialHeight;

            /** Minimum window width for resizing constraints */
            float MinWidth;

            /** Minimum window height for resizing constraints */
            float MinHeight;

            FWindowDimensions(float InInitialWidth, float InInitialHeight, float InMinWidth, float InMinHeight)
                : InitialWidth(InInitialWidth), InitialHeight(InInitialHeight), MinWidth(InMinWidth), MinHeight(InMinHeight)
            {
            }

            /** Validates that all dimensions are within acceptable ranges */
            bool IsValid() const
            {
                return InitialWidth > 0.0f && InitialHeight > 0.0f &&
                       MinWidth > 0.0f && MinHeight > 0.0f &&
                       InitialWidth >= MinWidth && InitialHeight >= MinHeight;
            }
        };

        /** Gets the standard welcome window dimensions */
        inline FWindowDimensions GetWelcomeWindowDimensions()
        {
            return FWindowDimensions(
                ConvaiEditor::Constants::Layout::Window::WelcomeWindowWidth,
                ConvaiEditor::Constants::Layout::Window::WelcomeWindowHeight,
                ConvaiEditor::Constants::Layout::Window::WelcomeWindowMinWidth,
                ConvaiEditor::Constants::Layout::Window::WelcomeWindowMinHeight);
        }

        /** Gets the standard main application window dimensions */
        inline FWindowDimensions GetMainWindowDimensions()
        {
            return FWindowDimensions(
                ConvaiEditor::Constants::Layout::Window::MainWindowWidth,
                ConvaiEditor::Constants::Layout::Window::MainWindowHeight,
                ConvaiEditor::Constants::Layout::Window::MainWindowMinWidth,
                ConvaiEditor::Constants::Layout::Window::MainWindowMinHeight);
        }

        /** Gets the default window dimensions for new windows */
        inline FWindowDimensions GetDefaultWindowDimensions()
        {
            return FWindowDimensions(
                ConvaiEditor::Constants::Layout::Window::DefaultWidth,
                ConvaiEditor::Constants::Layout::Window::DefaultHeight,
                ConvaiEditor::Constants::Layout::Window::MinWidth,
                ConvaiEditor::Constants::Layout::Window::MinHeight);
        }

        /** Validates window dimensions and logs warnings for invalid values */
        inline bool ValidateWindowDimensions(const FWindowDimensions &Dimensions, const FString &WindowType)
        {
            if (!Dimensions.IsValid())
            {
                UE_LOG(LogConvaiEditor, Warning, TEXT("Invalid window dimensions for %s: Initial(%f,%f) Min(%f,%f)"),
                       *WindowType, Dimensions.InitialWidth, Dimensions.InitialHeight,
                       Dimensions.MinWidth, Dimensions.MinHeight);
                return false;
            }

            return true;
        }
    } // namespace WindowUtils
} // namespace ConvaiEditor