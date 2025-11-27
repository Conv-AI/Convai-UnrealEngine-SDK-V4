/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWebBrowserPage.cpp
 *
 * Implementation of the web browser page.
 */

#include "UI/Pages/SWebBrowserPage.h"
#include "ConvaiEditor.h"
#include "Framework/Application/SlateApplication.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserWindow.h"
#include "WebBrowserModule.h"
#include "Utility/ConvaiURLs.h"

#define LOCTEXT_NAMESPACE "SWebBrowserPage"

const FString SWebBrowserPage::DefaultURL = FConvaiURLs::GetDashboardURL();

void SWebBrowserPage::Construct(const FArguments &InArgs)
{
    CurrentURL = InArgs._URL;

    if (!FModuleManager::Get().IsModuleLoaded("WebBrowser"))
    {
        if (!FModuleManager::Get().LoadModule("WebBrowser"))
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Failed to load WebBrowser module"));
        }
    }

    SBasePage::Construct(
        SBasePage::FArguments()
            .Content()
                [CreateMainLayout()]);
}

SWebBrowserPage::~SWebBrowserPage()
{
    CleanupBrowser();
}

void SWebBrowserPage::CleanupBrowser()
{
    if (WebBrowser.IsValid())
    {
        WebBrowser->LoadURL(TEXT("about:blank"));

        WebBrowser.Reset();
    }
}

TSharedRef<SWidget> SWebBrowserPage::CreateMainLayout()
{
    return CreateWebBrowser();
}

TSharedRef<SWidget> SWebBrowserPage::CreateWebBrowser()
{
    return SAssignNew(WebBrowser, SWebBrowser)
        .InitialURL(CurrentURL)
        .ShowControls(false)
        .ShowAddressBar(false)
        .ShowErrorMessage(true)
        .SupportsTransparency(false)
        .BrowserFrameRate(30)
        .OnUrlChanged(this, &SWebBrowserPage::OnUrlChanged)
        .OnLoadCompleted(this, &SWebBrowserPage::OnLoadCompleted)
        .OnLoadError(this, &SWebBrowserPage::OnLoadError)
        .OnBeforePopup(this, &SWebBrowserPage::OnBeforePopup)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
        .OnConsoleMessage(this, &SWebBrowserPage::OnConsoleMessage)
#endif
        ;
}

// Browser event handlers
void SWebBrowserPage::OnUrlChanged(const FText &InText)
{
    CurrentURL = InText.ToString();
}

void SWebBrowserPage::OnLoadCompleted()
{
    if (WebBrowser.IsValid())
    {
        FString JavaScriptCode = TEXT(R"(
            (function() {
                try {
                    var userAgent = navigator.userAgent;
                    var cefVersion = 'Unknown';
                    
                    var cefMatch = userAgent.match(/CEF\/(\d+\.\d+\.\d+)/);
                    if (cefMatch) {
                        cefVersion = cefMatch[1];
                    }
                    
                    var chromeMatch = userAgent.match(/Chrome\/(\d+\.\d+\.\d+\.\d+)/);
                    var chromeVersion = chromeMatch ? chromeMatch[1] : 'Unknown';
                    
                    console.log('[CEF Version Info] CEF Version: ' + cefVersion + ', Chrome Version: ' + chromeVersion);
                    console.log('[CEF Version Info] Full User Agent: ' + userAgent);
                    
                    return 'CEF: ' + cefVersion + ', Chrome: ' + chromeVersion;
                } catch (e) {
                    console.log('[CEF Version Info] Error getting version: ' + e.message);
                    return 'Error: ' + e.message;
                }
            })();
        )");

        WebBrowser->ExecuteJavascript(JavaScriptCode);
    }
}

void SWebBrowserPage::OnLoadError()
{
    UE_LOG(LogConvaiEditor, Error, TEXT("Web browser failed to load URL: %s"), *CurrentURL);
}

bool SWebBrowserPage::OnBeforePopup(FString URL, FString Frame)
{
    if (WebBrowser.IsValid())
    {
        WebBrowser->LoadURL(URL);
    }

    return true;
}

void SWebBrowserPage::OnConsoleMessage(const FString &Message, const FString &Source, int32 Line, EWebBrowserConsoleLogSeverity Severity)
{
    ELogVerbosity::Type LogVerbosity = ELogVerbosity::Log;
    switch (Severity)
    {
    case EWebBrowserConsoleLogSeverity::Default:
    case EWebBrowserConsoleLogSeverity::Verbose:
    case EWebBrowserConsoleLogSeverity::Debug:
    case EWebBrowserConsoleLogSeverity::Info:
        LogVerbosity = ELogVerbosity::Log;
        break;
    case EWebBrowserConsoleLogSeverity::Warning:
        LogVerbosity = ELogVerbosity::Warning;
        break;
    case EWebBrowserConsoleLogSeverity::Error:
    case EWebBrowserConsoleLogSeverity::Fatal:
        LogVerbosity = ELogVerbosity::Error;
        break;
    }

    bool bShouldLog = true;

    if (Message.Contains(TEXT("CORS policy")) ||
        Message.Contains(TEXT("Access-Control-Allow-Origin")) ||
        Message.Contains(TEXT("has been blocked by CORS policy")))
    {
        bShouldLog = false;
    }

    if (Message.Contains(TEXT("Permissions-Policy header")) ||
        Message.Contains(TEXT("Unrecognized feature:")) ||
        Message.Contains(TEXT("ch-ua-bitness")) ||
        Message.Contains(TEXT("ch-ua-full-version-list")) ||
        Message.Contains(TEXT("ch-ua-wow64")) ||
        Message.Contains(TEXT("ch-ua-form-factors")))
    {
        bShouldLog = false;
    }

    if (Message.Contains(TEXT("Deprecated API for given entry type")))
    {
        bShouldLog = false;
    }

    if (Message.Contains(TEXT("Failed to fetch RSC payload")) ||
        Message.Contains(TEXT("Falling back to browser navigation")))
    {
        bShouldLog = false;
    }

    if (bShouldLog)
    {
        if (Severity == EWebBrowserConsoleLogSeverity::Error || Severity == EWebBrowserConsoleLogSeverity::Fatal)
        {
            UE_LOG(LogConvaiEditor, Error, TEXT("Browser error: %s"), *Message);
        }
        else if (Severity == EWebBrowserConsoleLogSeverity::Warning)
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("Browser warning: %s"), *Message);
        }
    }
}

#undef LOCTEXT_NAMESPACE
