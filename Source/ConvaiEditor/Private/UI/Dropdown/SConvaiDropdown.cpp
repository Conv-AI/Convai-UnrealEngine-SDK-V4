/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiDropdown.cpp
 *
 * Implementation of the dropdown menu widget.
 */

#include "UI/Dropdown/SConvaiDropdown.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Styling/SlateTypes.h"
#include "SlateOptMacros.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Utility/ConvaiConstants.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

static const FSlateNoResource EmptyBrush;

TSharedRef<SWidget> SConvaiDropdown::BuildEntry(const FConvaiMenuEntry &Entry)
{
    if (bSupportsNested && Entry.HasChildren())
    {
        return BuildNestedEntry(Entry);
    }

    const float PadX = ConvaiEditor::Constants::Layout::Spacing::Nav;
    const float PadY = ConvaiEditor::Constants::Layout::Spacing::DropdownY;

    TSharedRef<SButton> Btn = SNew(SButton)
                                  .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                                  .ContentPadding(FMargin(PadX, PadY))
                                  .OnClicked_Lambda([Action = Entry.Action, Window = Window]()
                                                    {
            if (Action.IsBound()) Action.Execute();
            if (FSlateApplication::IsInitialized())
            {
                if (TSharedPtr<SWindow> PinnedWindow = Window.Pin())
                {
                    PinnedWindow->RequestDestroyWindow();
                }
            }
            return FReply::Handled(); });

    Btn->SetContent(
        SNew(STextBlock)
            .Text(Entry.Label)
            .Font(FConvaiStyle::Get().GetFontStyle(FontStyleName))
            .ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([Btn, Entry]()
                                                                   {
            const bool bHovered = Btn->IsHovered();
            if (Entry.bHighlight)
            {
                double Time = FPlatformTime::Seconds();
                float Alpha = (FMath::Sin(Time * PI) + 1.0f) * 0.5f;
                FLinearColor Start = FConvaiStyle::Get().GetColor("Convai.Color.dropdownText");
                FLinearColor End = FLinearColor(0.2f, 0.8f, 0.2f, 1.0f);
                FLinearColor Mix = Start + (End - Start) * Alpha;
                return FSlateColor(Mix);
            }
            FLinearColor DefaultColor = bHovered
                ? FConvaiStyle::Get().GetColor("Convai.Color.dropdownTextHover")
                : FConvaiStyle::Get().GetColor("Convai.Color.dropdownText");
            return FSlateColor(DefaultColor); })));

    return Btn;
}

TSharedRef<SWidget> SConvaiDropdown::BuildNestedEntry(const FConvaiMenuEntry &Entry)
{
    const float PadX = ConvaiEditor::Constants::Layout::Spacing::Nav;
    const float PadY = ConvaiEditor::Constants::Layout::Spacing::DropdownY;

    TSharedRef<SButton> Btn = SNew(SButton)
                                  .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                                  .ContentPadding(FMargin(PadX, PadY))
                                  .OnClicked_Lambda([Action = Entry.Action, Window = Window]()
                                                    {
            if (Action.IsBound()) Action.Execute();
            if (FSlateApplication::IsInitialized())
            {
                if (TSharedPtr<SWindow> PinnedWindow = Window.Pin())
                {
                    PinnedWindow->RequestDestroyWindow();
                }
            }
            return FReply::Handled(); });

    Btn->SetContent(
        SNew(STextBlock)
            .Text(Entry.Label)
            .Font(FConvaiStyle::Get().GetFontStyle(FontStyleName))
            .ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([Btn]()
                                                                   {
            const bool bHovered = Btn->IsHovered();
            return bHovered
                ? FConvaiStyle::Get().GetColor("Convai.Color.dropdownTextHover")
                : FConvaiStyle::Get().GetColor("Convai.Color.dropdownText"); })));

    return Btn;
}

void SConvaiDropdown::Construct(const FArguments &Args)
{
    const ISlateStyle &ST = FConvaiStyle::Get();

    FontStyleName = Args._FontStyle.IsNone() ? "Convai.Font.dropdown" : Args._FontStyle;

    bSupportsNested = Args._SupportsNested;

    DropdownBackgroundBrush = FConvaiStyle::GetRoundedDropdownBrush();

    TSharedRef<SVerticalBox> MenuBox = SNew(SVerticalBox);

    for (const FConvaiMenuEntry &Entry : Args._Entries)
    {
        MenuBox->AddSlot()
            .AutoHeight()
            .Padding(0)
                [BuildEntry(Entry)];
    }

    ChildSlot
        [SNew(SBorder)
             .Padding(0)
             .BorderImage(DropdownBackgroundBrush)
                 [MenuBox]];

    Window = Args._OwningWindow;
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION
