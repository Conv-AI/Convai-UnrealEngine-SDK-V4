/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SSettingsPage.cpp
 *
 * Implementation of the settings page.
 */

#include "UI/Pages/SSettingsPage.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Utility/ConvaiConstants.h"

#define LOCTEXT_NAMESPACE "ConvaiEditor.SettingsPage"

void SSettingsPage::Construct(const FArguments &InArgs)
{
    SBasePage::Construct(SBasePage::FArguments()
                             [SNew(SVerticalBox)

                              + SVerticalBox::Slot()
                                    .AutoHeight()
                                    .Padding(0, 0, 0, 20)
                                        [SNew(STextBlock)
                                             .Text(LOCTEXT("SettingsPageTitle", "Settings"))
                                             .Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.accountSectionTitle"))
                                             .ColorAndOpacity(FConvaiStyle::Get().GetColor("Convai.Color.accountSectionTitle"))]

                              + SVerticalBox::Slot()
                                    .FillHeight(1.0f)
                                        [SNew(SScrollBox)
                                             .Style(&FConvaiStyle::GetScrollBoxStyle(false))
                                             .ScrollBarStyle(&FConvaiStyle::GetScrollBarStyle())
                                             .ScrollBarVisibility(EVisibility::Visible)
                                             .ScrollBarThickness(FVector2D(
                                                 ConvaiEditor::Constants::Layout::Components::ScrollBar::Thickness,
                                                 ConvaiEditor::Constants::Layout::Components::ScrollBar::Thickness)) +
                                         SScrollBox::Slot()
                                             .Padding(FMargin(0, 0, 20, 0))
                                                 [SNew(SVerticalBox)

                                                  // Future sections can be added here
                                                  // For example:
                                                  // + SVerticalBox::Slot()
                                                  // .AutoHeight()
                                                  // .Padding(0, 0, 0, 20)
                                                  // [
                                                  //     SNew(SOtherSettingsSection)
                                                  // ]
    ]]]);

    FScrollBarStyle ScrollBarStyle = FConvaiStyle::GetScrollBarStyle();
    ScrollBarStyle.SetThickness(ConvaiEditor::Constants::Layout::Components::ScrollBar::Thickness);
    ScrollBarStyle.SetVerticalTopSlotImage(FSlateNoResource());
    ScrollBarStyle.SetVerticalBottomSlotImage(FSlateNoResource());
    ScrollBarStyle.SetHorizontalTopSlotImage(FSlateNoResource());
    ScrollBarStyle.SetHorizontalBottomSlotImage(FSlateNoResource());
    ScrollBarStyle.SetNormalThumbImage(FSlateColorBrush(FConvaiStyle::RequireColor("Convai.Color.icon.scrollBarThumb")));
    ScrollBarStyle.SetHoveredThumbImage(FSlateColorBrush(FConvaiStyle::RequireColor("Convai.Color.icon.scrollBarThumb")));
    ScrollBarStyle.SetDraggedThumbImage(FSlateColorBrush(FConvaiStyle::RequireColor("Convai.Color.icon.scrollBarThumb")));
    ScrollBarStyle.SetVerticalBackgroundImage(FSlateColorBrush(FConvaiStyle::RequireColor("Convai.Color.scrollBarTrack")));
    ScrollBarStyle.SetHorizontalBackgroundImage(FSlateColorBrush(FConvaiStyle::RequireColor("Convai.Color.scrollBarTrack")));
}

#undef LOCTEXT_NAMESPACE
