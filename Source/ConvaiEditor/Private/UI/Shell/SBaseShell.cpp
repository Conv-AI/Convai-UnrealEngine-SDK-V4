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
#elif PLATFORM_LINUX
#include "X11/Xlib.h"
#include "X11/Xutil.h"
#elif PLATFORM_MAC
#include "Cocoa/Cocoa.h"
#endif

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
        // Linux X11 implementation
        Display *Display = XOpenDisplay(nullptr);
        if (Display)
        {
            Window WindowHandle = (Window)GenericWindow->GetOSWindowHandle();
            if (WindowHandle)
            {
                // Remove _NET_WM_STATE_ABOVE from window state
                Atom AboveState = XInternAtom(Display, "_NET_WM_STATE_ABOVE", False);
                Atom StateAtom = XInternAtom(Display, "_NET_WM_STATE", False);

                XEvent Event;
                memset(&Event, 0, sizeof(Event));
                Event.xclient.type = ClientMessage;
                Event.xclient.window = WindowHandle;
                Event.xclient.message_type = StateAtom;
                Event.xclient.format = 32;
                Event.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
                Event.xclient.data.l[1] = AboveState;
                Event.xclient.data.l[2] = 0;
                Event.xclient.data.l[3] = 0;
                Event.xclient.data.l[4] = 0;

                XSendEvent(Display, DefaultRootWindow(Display), False, SubstructureRedirectMask | SubstructureNotifyMask, &Event);
                XFlush(Display);
            }
            XCloseDisplay(Display);
        }
#elif PLATFORM_MAC
        // macOS Cocoa implementation
        NSWindow *WindowHandle = (NSWindow *)GenericWindow->GetOSWindowHandle();
        if (WindowHandle)
        {
            [WindowHandle setLevel:NSNormalWindowLevel];
        }
#endif
    }
}
