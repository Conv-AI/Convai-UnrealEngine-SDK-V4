/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAuthShell.h
 *
 * Shell window hosting OAuth browser with status overlay.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Shell/SBaseShell.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "UI/Widgets/SAuthStatusOverlay.h"
#include "Widgets/SOverlay.h"
#include "SWebBrowser.h"
#include "Utility/ConvaiConstants.h"

/**
 * Shell window hosting OAuth browser with status overlay.
 */
class CONVAIEDITOR_API SAuthShell : public SBaseShell
{
public:
    SLATE_BEGIN_ARGS(SAuthShell)
        : _InitialWidth(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::AuthWindowWidth)), _InitialHeight(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::AuthWindowHeight)), _MinWidth(ConvaiEditor::Constants::Layout::Window::AuthWindowMinWidth), _MinHeight(ConvaiEditor::Constants::Layout::Window::AuthWindowMinHeight)
    {
    }
    SLATE_ARGUMENT(int32, InitialWidth)
    SLATE_ARGUMENT(int32, InitialHeight)
    SLATE_ARGUMENT(float, MinWidth)
    SLATE_ARGUMENT(float, MinHeight)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    /** Initializes the browser with the specified URL */
    void InitWithURL(const FString &Url);

    /** Shows the status overlay with a message */
    void ShowOverlay(const FText &Message, const FText &SubMessage = FText::GetEmpty());

    /** Hides the status overlay */
    void HideOverlay();

    virtual void OnWindowClosed(const TSharedRef<SWindow> &ClosedWindow) override;

private:
    bool HandleBeforePopup(FString URL, FString Frame);
    void OnUrlChanged(const FText &NewURL);
    void OnConsoleMessage(const FString &Message, const FString &Source, int32 Line, EWebBrowserConsoleLogSeverity Severity);

    /** Returns JavaScript code for detecting CEF version */
    static FString GetCEFVersionDetectionScript();

    /** Returns JavaScript code for configuring logo and signup links */
    static FString GetLogoAndSignupLinksScript();

    /** Returns JavaScript code for configuring OAuth buttons */
    static FString GetOAuthButtonsScript();

    TSharedPtr<SWebBrowser> Browser;
    TSharedPtr<SAuthStatusOverlay> Overlay;
    FTSTicker::FDelegateHandle OverlayHideTickerHandle;
};