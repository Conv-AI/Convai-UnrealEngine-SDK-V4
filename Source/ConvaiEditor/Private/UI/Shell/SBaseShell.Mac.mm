/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SBaseShell.Mac.mm
 *
 * macOS (Objective-C++) helpers for SBaseShell. SBaseShell.cpp is compiled as
 * plain C++ on Mac and therefore cannot use Cocoa/Objective-C; the native NSWindow
 * work lives here and is called through the plain C++ bridge below. UBT auto-globs
 * this file and compiles it as Objective-C++ on Apple platforms only.
 */

#ifdef __APPLE__

// Cocoa.h transitively includes CarbonCore/NumberFormatting.h, whose global
// `struct FVector` collides with Unreal's `FVector` type alias (pulled in via the
// module's shared PCH). Guard the system-header import the way the engine does in
// Mac/MacSystemIncludes.h: temporarily rename the Carbon symbol, then restore it.
#pragma push_macro("FVector")
#define FVector FVectorWorkaround
#import <Cocoa/Cocoa.h>
#undef FVector
#pragma pop_macro("FVector")

// Clears the "always on top" window level on a native NSWindow handle.
// Declared as `extern` in SBaseShell.cpp and called from its PLATFORM_MAC branch.
void ConvaiEditor_ClearWindowTopmost(void* OSWindowHandle)
{
    NSWindow* WindowHandle = (NSWindow*)OSWindowHandle;
    if (WindowHandle)
    {
        [WindowHandle setLevel:NSNormalWindowLevel];
    }
}

#endif
