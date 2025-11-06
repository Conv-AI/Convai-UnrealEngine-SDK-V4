/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAuthShell.cpp
 *
 * Implementation of the authentication shell window.
 */

#include "UI/Shell/SAuthShell.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiStyleResources.h"
#include "UI/Shell/SDraggableBackground.h"
#include "Utility/ConvaiConstants.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Containers/Ticker.h"
#include "IWebBrowserWindow.h"

void SAuthShell::Construct(const FArguments &InArgs)
{
    SBaseShell::Construct(SBaseShell::FArguments()
                              .InitialWidth(InArgs._InitialWidth)
                              .InitialHeight(InArgs._InitialHeight)
                              .MinWidth(InArgs._MinWidth)
                              .MinHeight(InArgs._MinHeight)
                              .AllowClose(true));

    const float WindowPad = ConvaiEditor::Constants::Layout::Spacing::Window;

    Overlay = SNew(SAuthStatusOverlay)
                  .Message(FText::FromString("Initializing Convai..."));

    TSharedRef<SWidget> Content =
        SNew(SDraggableBackground)
            .ParentWindow(SharedThis(this))
                [SNew(SOverlay) + SOverlay::Slot()[
                                      // Card background like main UI
                                      SNew(SBox)[SNew(SOverlay) + SOverlay::Slot()[SNew(SImage).Image(FConvaiStyle::GetContentContainerBrush())] + SOverlay::Slot().Padding(WindowPad)[SAssignNew(Browser, SWebBrowser).ShowControls(false).ShowAddressBar(false).ShowErrorMessage(false).OnBeforePopup(this, &SAuthShell::HandleBeforePopup).OnUrlChanged(this, &SAuthShell::OnUrlChanged)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
                                                                                                                                                                                           .OnConsoleMessage(this, &SAuthShell::OnConsoleMessage)
#endif
    ]]] +
                 SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)[Overlay.ToSharedRef()]];

    SetShellContent(Content);
}

void SAuthShell::InitWithURL(const FString &Url)
{
    if (Browser.IsValid())
    {
        Browser->LoadURL(Url);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakOverlay = TWeakPtr<SAuthStatusOverlay>(Overlay)](float)
                                                                           { if(auto O=WeakOverlay.Pin()){ O->SetVisibility(EVisibility::Collapsed);} return false; }),
                                             1.0f);
    }
}

void SAuthShell::ShowOverlay(const FText &Message, const FText &SubMessage)
{
    if (Overlay.IsValid())
    {
        Overlay->SetStatus(Message, SubMessage);
        Overlay->SetVisibility(EVisibility::Visible);
    }
}

void SAuthShell::HideOverlay()
{
    if (Overlay.IsValid())
    {
        Overlay->SetVisibility(EVisibility::Collapsed);
    }
}

void SAuthShell::OnWindowClosed(const TSharedRef<SWindow> &ClosedWindow)
{
    if (Browser.IsValid())
    {
        Browser->LoadURL(TEXT("about:blank"));

        Browser.Reset();
    }

    SBaseShell::OnWindowClosed(ClosedWindow);
}

bool SAuthShell::HandleBeforePopup(FString URL, FString Frame)
{

    if (Browser.IsValid())
    {
        Browser->LoadURL(URL);
    }
    return true;
}

FString SAuthShell::GetCEFVersionDetectionScript()
{
    return TEXT(R"(
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
                console.log('[OAuth CEF Version Info] CEF Version: ' + cefVersion + ', Chrome Version: ' + chromeVersion);
                console.log('[OAuth CEF Version Info] Full User Agent: ' + userAgent);
                return 'CEF: ' + cefVersion + ', Chrome: ' + chromeVersion;
            } catch (e) {
                console.log('[OAuth CEF Version Info] Error getting version: ' + e.message);
                return 'Error: ' + e.message;
            }
        })();
    )");
}

FString SAuthShell::GetLogoAndSignupLinksScript()
{
    return TEXT(R"(
        (function() {
            try {
                function configureElementForExternalBrowser(element, description) {
                    if (!element) return;
                    element.onclick = null;
                    var href = element.getAttribute('href');
                    element.addEventListener('click', function(e) {
                        e.preventDefault();
                        e.stopPropagation();
                        var now = Date.now();
                        if (element._lastClickTime && (now - element._lastClickTime) < 1000) {
                            console.log('[OAuth] Click ignored - too soon after last click');
                            return false;
                        }
                        element._lastClickTime = now;
                        if (href) {
                            var currentPageUrl = window.location.href;
                            console.log('[OAuth External Browser Request]: ' + href + '|' + currentPageUrl);
                            console.log('[OAuth] Opening ' + description + ' in external browser: ' + href);
                        }
                        return false;
                    }, { capture: true });
                    element.style.cursor = 'pointer';
                    console.log('[OAuth] Configured ' + description + ' for external browser');
                }
                document.querySelectorAll('a[href*="convai.com"]:not([href*="login.convai.com"])').forEach(function(el) {
                    configureElementForExternalBrowser(el, 'logo link');
                });
                document.querySelectorAll('a[href*="signup"], a[href*="register"], a[href*="create"]').forEach(function(el) {
                    configureElementForExternalBrowser(el, 'signup link');
                });
                console.log('[OAuth] Part 1 - Logo and signup links configured');
            } catch (e) {
                console.log('[OAuth] Error in part 1: ' + e.message);
            }
        })();
    )");
}

FString SAuthShell::GetOAuthButtonsScript()
{
    return TEXT(R"(
        (function() {
            try {
                var allElements = document.querySelectorAll('a, button');
                allElements.forEach(function(element) {
                    var elementText = element.textContent.trim().toLowerCase();
                    var href = element.getAttribute('href');
                    var isOAuthButton = elementText.includes('google') || 
                                       elementText.includes('github') || 
                                       elementText.includes('sign in with') ||
                                       elementText.includes('continue with') ||
                                       (href && (href.includes('google') ||
                                       href.includes('github') ||
                                       href.includes('oauth') ||
                                       href.includes('accounts.google.com') ||
                                       href.includes('github.com/login')));
                    if (isOAuthButton) {
                        element.onclick = null;
                        element.addEventListener('click', function(e) {
                            e.preventDefault();
                            e.stopPropagation();
                            var now = Date.now();
                            if (element._lastClickTime && (now - element._lastClickTime) < 1000) {
                                console.log('[OAuth] Click ignored - too soon after last click');
                                return false;
                            }
                            element._lastClickTime = now;
                            var oauthUrl = href;
                            if (!oauthUrl && element.tagName === 'BUTTON') {
                                var onclickStr = element.getAttribute('onclick');
                                if (onclickStr) {
                                    var urlMatch = onclickStr.match(/https?:\/\/[^\s'"]+/);
                                    if (urlMatch) {
                                        oauthUrl = urlMatch[0];
                                    }
                                }
                            }
                            if (oauthUrl) {
                                var currentPageUrl = window.location.href;
                                console.log('[OAuth External Browser Request]: ' + oauthUrl + '|' + currentPageUrl);
                                console.log('[OAuth] Opening OAuth button in external browser: ' + elementText);
                            } else {
                                console.log('[OAuth] OAuth button detected but no URL found: ' + elementText);
                            }
                            return false;
                        }, { capture: true });
                        element.style.cursor = 'pointer';
                        console.log('[OAuth] Configured OAuth button for external browser: ' + elementText);
                    }
                });
                console.log('[OAuth] Part 2 - OAuth buttons configured for external browser');
            } catch (e) {
                console.log('[OAuth] Error in part 2: ' + e.message);
            }
        })();
    )");
}

void SAuthShell::OnUrlChanged(const FText &NewURL)
{
    FString URLString = NewURL.ToString();

    if (Browser.IsValid() && URLString.Contains(TEXT("login.convai.com")))
    {
        Browser->ExecuteJavascript(GetCEFVersionDetectionScript());

        TWeakPtr<SWebBrowser> WeakBrowser = Browser;

        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakBrowser](float)
                                                                           {
                                                                               if (TSharedPtr<SWebBrowser> PinnedBrowser = WeakBrowser.Pin())
                                                                               {
                                                                                   PinnedBrowser->ExecuteJavascript(GetLogoAndSignupLinksScript());
                                                                                   FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakBrowser](float)
                                                                                                                                                      {
                                                                                       if (TSharedPtr<SWebBrowser> PinnedBrowser2 = WeakBrowser.Pin())
                                                                                       {
                                                                                           PinnedBrowser2->ExecuteJavascript(GetOAuthButtonsScript());
                                                                                       }
                                                                                       return false; }),
                                                                                                                        0.1f);
                                                                               }
                                                                               return false; }),
                                             1.0f);
    }
}

void SAuthShell::OnConsoleMessage(const FString &Message, const FString &Source, int32 Line, EWebBrowserConsoleLogSeverity Severity)
{
    if (Message.StartsWith(TEXT("[OAuth External Browser Request]:")))
    {
        FString URLData = Message.RightChop(35);
        URLData = URLData.TrimStartAndEnd();

        if (!URLData.IsEmpty())
        {
            FString Href;
            FString CurrentPageUrl;

            if (URLData.Contains(TEXT("|")))
            {
                URLData.Split(TEXT("|"), &Href, &CurrentPageUrl);
                Href = Href.TrimStartAndEnd();
                CurrentPageUrl = CurrentPageUrl.TrimStartAndEnd();
            }
            else
            {
                Href = URLData;
            }

            FString AbsoluteUrl;
            if (Href.StartsWith(TEXT("http://")) || Href.StartsWith(TEXT("https://")))
            {
                AbsoluteUrl = Href;
            }
            else if (Href.StartsWith(TEXT("ttps://")))
            {
                AbsoluteUrl = TEXT("h") + Href;
            }
            else if (Href.StartsWith(TEXT("/")))
            {
                if (CurrentPageUrl.StartsWith(TEXT("https://")))
                {
                    FString Origin = CurrentPageUrl.Left(CurrentPageUrl.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 8));
                    AbsoluteUrl = Origin + Href;
                }
                else
                {
                    AbsoluteUrl = TEXT("https://login.convai.com") + Href;
                }
            }
            else if (Href.StartsWith(TEXT("#")))
            {
                AbsoluteUrl = CurrentPageUrl + Href;
            }
            else
            {
                if (!CurrentPageUrl.IsEmpty())
                {
                    int32 LastSlash = CurrentPageUrl.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
                    if (LastSlash != INDEX_NONE)
                    {
                        FString BaseUrl = CurrentPageUrl.Left(LastSlash + 1);
                        AbsoluteUrl = BaseUrl + Href;
                    }
                    else
                    {
                        AbsoluteUrl = CurrentPageUrl + TEXT("/") + Href;
                    }
                }
                else
                {
                    AbsoluteUrl = TEXT("https://login.convai.com/") + Href;
                }
            }

            FPlatformProcess::LaunchURL(*AbsoluteUrl, nullptr, nullptr);
        }
        return;
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
            UE_LOG(LogConvaiEditor, Error, TEXT("Auth browser error: %s"), *Message);
        }
        else if (Severity == EWebBrowserConsoleLogSeverity::Warning)
        {
            UE_LOG(LogConvaiEditor, Warning, TEXT("Auth browser warning: %s"), *Message);
        }
    }
}