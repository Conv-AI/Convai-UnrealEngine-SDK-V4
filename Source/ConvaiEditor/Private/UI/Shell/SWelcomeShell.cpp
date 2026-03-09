/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SWelcomeShell.cpp
 *
 * Implementation of the welcome shell window.
 */

#include "UI/Shell/SWelcomeShell.h"
#include "CoreMinimal.h"
#include "Widgets/Layout/SBox.h"
#include "UI/Shell/SDraggableBackground.h"

void SWelcomeShell::Construct(const FArguments &InArgs)
{
    const int32 FixedWidth = InArgs._InitialWidth;
    const int32 FixedHeight = InArgs._InitialHeight;

    SBaseShell::Construct(SBaseShell::FArguments()
                              .InitialWidth(FixedWidth)
                              .InitialHeight(FixedHeight)
                              .MinWidth(FixedWidth)
                              .MinHeight(FixedHeight)
                              .AllowClose(false)
                              .SizingRule(ESizingRule::FixedSize)
                              .IsTopmostWindow(true));
    SetWelcomeContent(SNew(SBox));
}

void SWelcomeShell::SetWelcomeContent(const TSharedRef<SWidget> &InContent)
{
    SetShellContent(
        SNew(SDraggableBackground)
            .ParentWindow(SharedThis(this))
                [InContent]);
}

bool SWelcomeShell::CanCloseWindow() const
{
    return bApiKeyValid;
}
