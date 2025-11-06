/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWebBrowserPage.h
 *
 * Web browser page widget displaying configurable URLs.
 */

#pragma once

#include "UI/Pages/SBasePage.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "SWebBrowser.h"
#include "Styling/ConvaiStyle.h"

#define LOCTEXT_NAMESPACE "SWebBrowserPage"

/** Web browser page widget displaying configurable URLs. */
class CONVAIEDITOR_API SWebBrowserPage : public SBasePage
{
public:
    SLATE_BEGIN_ARGS(SWebBrowserPage)
    {
        _URL = TEXT("https://convai.com");
    }
    SLATE_ARGUMENT(FString, URL)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs);

    static FName StaticClass()
    {
        static FName TypeName = FName("SWebBrowserPage");
        return TypeName;
    }

    virtual bool IsA(const FName &TypeName) const override
    {
        return TypeName == StaticClass() || SBasePage::IsA(TypeName);
    }

private:
    TSharedRef<SWidget> CreateMainLayout();
    TSharedRef<SWidget> CreateWebBrowser();

    void OnUrlChanged(const FText &InText);
    bool OnBeforePopup(FString URL, FString Frame);
    void OnLoadCompleted();
    void OnLoadError();
    void OnConsoleMessage(const FString &Message, const FString &Source, int32 Line, EWebBrowserConsoleLogSeverity Severity);

    TSharedPtr<SWebBrowser> WebBrowser;
    FString CurrentURL;
    static const FString DefaultURL;
};

#undef LOCTEXT_NAMESPACE
