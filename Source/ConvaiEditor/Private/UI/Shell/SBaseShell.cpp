/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SBaseShell.cpp
 *
 * Implementation of the base shell window.
 */

#include "UI/Shell/SBaseShell.h"
#include "CoreMinimal.h"
#include "Widgets/Layout/SBox.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#elif PLATFORM_MAC
// The Cocoa (NSWindow) code lives in SBaseShell.Mac.mm — this .cpp is compiled as
// plain C++ on Mac and cannot use Objective-C. Bridge to it via a plain C++ function.
extern void ConvaiEditor_ClearWindowTopmost(void* OSWindowHandle);
#endif
// PLATFORM_LINUX: nothing platform-specific is actually used by this TU; X11
// headers aren't shipped with the UE 5.8 cross-compile toolchain (clang 20,
// v26) anyway. Slate handles all the window plumbing for us.

void SBaseShell::Construct(const FArguments &InArgs)
{
    bAllowClose = InArgs._AllowClose;
    SWindow::Construct(SWindow::FArguments()
                           .Title(FText::GetEmpty())
                           .CreateTitleBar(false)
                           .SupportsMaximize(InArgs._SizingRule != ESizingRule::FixedSize)
                           .SupportsMinimize(InArgs._SizingRule != ESizingRule::FixedSize)
                           .SizingRule(InArgs._SizingRule)
                           .MinWidth(InArgs._MinWidth)
                           .MinHeight(InArgs._MinHeight)
                           .ClientSize(FVector2D(InArgs._InitialWidth, InArgs._InitialHeight))
                           .AutoCenter(EAutoCenter::PrimaryWorkArea)
                           .IsTopmostWindow(InArgs._IsTopmostWindow)
                           .Style(FCoreStyle::Get(), "Window")
                               [SNew(SBox)]);
}

void SBaseShell::SetShellContent(const TSharedRef<SWidget> &InContent)
{
    SetContent(InContent);
}

void SBaseShell::OnWindowClosed(const TSharedRef<SWindow> &ClosedWindow)
{
}

void SBaseShell::DisableTopmost()
{
    if (TSharedPtr<FGenericWindow> GenericWindow = GetNativeWindow())
    {
#if PLATFORM_WINDOWS
        HWND WindowHandle = (HWND)GenericWindow->GetOSWindowHandle();
        if (WindowHandle)
        {
            SetWindowPos(WindowHandle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
#elif PLATFORM_LINUX
        // Linux X11 implementation removed for the UE 5.8 cross-compile build:
        // the v26 toolchain doesn't ship X11/Xlib.h, and the editor target this
        // file lives in is Windows-only in practice. If a Linux Editor build
        // ever becomes a target, wire up SDL2 or load libX11 dynamically.
        // TODO(linux-editor): restore a non-X11 "remove topmost hint" path.
#elif PLATFORM_MAC
        // macOS Cocoa implementation (defined in SBaseShell.Mac.mm)
        ConvaiEditor_ClearWindowTopmost(GenericWindow->GetOSWindowHandle());
#endif
    }
}
