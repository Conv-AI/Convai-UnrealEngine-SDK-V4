/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SBaseShell.h
 *
 * Base class for Convai plugin windows.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWindow.h"
#include "Utility/ConvaiConstants.h"

/**
 * Base class for Convai plugin windows.
 */
class SBaseShell : public SWindow
{
public:
    SLATE_BEGIN_ARGS(SBaseShell)
        : _InitialWidth(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowWidth)), _InitialHeight(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowHeight)), _MinWidth(ConvaiEditor::Constants::Layout::Window::MainWindowMinWidth), _MinHeight(ConvaiEditor::Constants::Layout::Window::MainWindowMinHeight), _AllowClose(true), _SizingRule(ESizingRule::UserSized), _IsTopmostWindow(false)
    {
    }
    SLATE_ARGUMENT(int32, InitialWidth)
    SLATE_ARGUMENT(int32, InitialHeight)
    SLATE_ARGUMENT(float, MinWidth)
    SLATE_ARGUMENT(float, MinHeight)
    SLATE_ARGUMENT(bool, AllowClose)
    SLATE_ARGUMENT(ESizingRule, SizingRule)
    SLATE_ARGUMENT(bool, IsTopmostWindow)
    SLATE_END_ARGS()

    virtual void Construct(const FArguments &InArgs);

    /** Sets the main content of the shell window */
    virtual void SetShellContent(const TSharedRef<SWidget> &InContent);

    /** Sets whether the window can be closed */
    void SetAllowClose(bool bInAllowClose) { bAllowClose = bInAllowClose; }

    /** Returns whether the window can be closed */
    bool IsCloseAllowed() const { return bAllowClose; }

    /** Disables the topmost property of the window */
    void DisableTopmost();

protected:
    virtual bool CanCloseWindow() const { return bAllowClose; }
    virtual void OnWindowClosed(const TSharedRef<SWindow> &ClosedWindow);

    bool bAllowClose = true;
};
