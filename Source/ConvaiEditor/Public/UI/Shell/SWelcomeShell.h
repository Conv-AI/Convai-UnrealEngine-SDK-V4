/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWelcomeShell.h
 *
 * Shell window for welcome and API key entry.
 */

#pragma once

#include "UI/Shell/SBaseShell.h"
#include "CoreMinimal.h"
#include "Utility/ConvaiConstants.h"

/**
 * Shell window for welcome and API key entry.
 */
class SWelcomeShell : public SBaseShell
{
public:
    SLATE_BEGIN_ARGS(SWelcomeShell)
        : _InitialWidth(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::WelcomeWindowWidth)), _InitialHeight(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::WelcomeWindowHeight)), _MinWidth(ConvaiEditor::Constants::Layout::Window::WelcomeWindowMinWidth), _MinHeight(ConvaiEditor::Constants::Layout::Window::WelcomeWindowMinHeight)
    {
    }
    SLATE_ARGUMENT(int32, InitialWidth)
    SLATE_ARGUMENT(int32, InitialHeight)
    SLATE_ARGUMENT(float, MinWidth)
    SLATE_ARGUMENT(float, MinHeight)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    /** Sets the welcome content widget */
    void SetWelcomeContent(const TSharedRef<SWidget> &InContent);

    /** Sets API key validity */
    void SetApiKeyValid(bool bInValid) { bApiKeyValid = bInValid; }

    /** Returns whether the API key is valid */
    bool IsApiKeyValid() const { return bApiKeyValid; }

protected:
    virtual bool CanCloseWindow() const override;
    bool bApiKeyValid = false;
};
