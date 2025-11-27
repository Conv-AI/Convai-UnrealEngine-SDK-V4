/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiNestedDropdown.cpp
 *
 * Implementation of the nested dropdown menu widget.
 */

#include "UI/Dropdown/SConvaiNestedDropdown.h"
#include "UI/Dropdown/SConvaiDropdown.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "UI/Utility/HoverAwareMenuWrapper.h"
#include "Framework/Application/SlateApplication.h"
#include "SlateOptMacros.h"
#include "Utility/ConvaiConstants.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

void FNestedDropdownHoverState::Shutdown()
{
    ClearTicker();
    if (SubMenuAnchor.IsValid())
    {
        SubMenuAnchor->SetIsOpen(false);
    }
}

void FNestedDropdownHoverState::ClearTicker()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
}

void FNestedDropdownHoverState::CloseIfNotHovered()
{
    if (!bParentHovered && !bSubMenuHovered && bSubMenuOpen)
    {
        CloseSubMenu();
    }
}

void FNestedDropdownHoverState::OpenSubMenu()
{
    if (SubMenuAnchor.IsValid() && !bSubMenuOpen)
    {
        SubMenuAnchor->SetIsOpen(true);
        bSubMenuOpen = true;
    }
}

void FNestedDropdownHoverState::CloseSubMenu()
{
    if (SubMenuAnchor.IsValid() && bSubMenuOpen)
    {
        SubMenuAnchor->SetIsOpen(false);
        bSubMenuOpen = false;
    }
}

void SConvaiNestedDropdown::Construct(const FArguments &InArgs)
{
    CurrentNestingLevel = InArgs._NestingLevel;

    if (CurrentNestingLevel >= MaxNestingDepth)
    {
        SConvaiDropdown::Construct(SConvaiDropdown::FArguments()
                                       .Entries(InArgs._Entries)
                                       .OwningWindow(InArgs._OwningWindow)
                                       .FontStyle(InArgs._FontStyle)
                                       .SupportsNested(false));
        return;
    }

    SConvaiDropdown::Construct(SConvaiDropdown::FArguments()
                                   .Entries(InArgs._Entries)
                                   .OwningWindow(InArgs._OwningWindow)
                                   .FontStyle(InArgs._FontStyle)
                                   .SupportsNested(true));
}

SConvaiNestedDropdown::~SConvaiNestedDropdown()
{
    for (auto &HoverState : NestedHoverStates)
    {
        if (HoverState.IsValid())
        {
            HoverState->Shutdown();
        }
    }
    NestedHoverStates.Empty();
}

TSharedRef<SWidget> SConvaiNestedDropdown::BuildNestedEntry(const FConvaiMenuEntry &Entry)
{
    TSharedPtr<FNestedDropdownHoverState> HoverState = MakeShareable(new FNestedDropdownHoverState());
    NestedHoverStates.Add(HoverState);

    const float PadX = ConvaiEditor::Constants::Layout::Spacing::Nav;
    const float PadY = ConvaiEditor::Constants::Layout::Spacing::DropdownY;

    TSharedPtr<SButton> ParentButton;

    TSharedRef<SMenuAnchor> SubMenuAnchor = SNew(SMenuAnchor)
                                                .Method(EPopupMethod::UseCurrentWindow)
                                                .UseApplicationMenuStack(false)
                                                .Placement(MenuPlacement_BelowAnchor)
                                                .OnGetMenuContent_Lambda([this, Entry, HoverState]()
                                                                         { return CreateSubMenuContent(Entry, HoverState); })
                                                .OnMenuOpenChanged_Lambda([HoverState, ParentButton](bool bOpen)
                                                                          {
            HoverState->bSubMenuOpen = bOpen;
            if (ParentButton.IsValid())
            {
                ParentButton->Invalidate(EInvalidateWidget::Paint);
            } })
                                                    [SAssignNew(ParentButton, SButton)
                                                         .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                                                         .ContentPadding(FMargin(PadX, PadY))
                                                         .OnClicked_Lambda([Action = Entry.Action]()
                                                                           {
                if (Action.IsBound()) 
                {
                    Action.Execute();
                }
                return FReply::Handled(); })
                                                             [SNew(STextBlock)
                                                                  .Text(Entry.Label)
                                                                  .Font(FConvaiStyle::Get().GetFontStyle(FontStyleName))
                                                                  .ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([ParentButton, HoverState]()
                                                                                                                         {
                    const bool bHovered = ParentButton.IsValid() && ParentButton->IsHovered();
                    const bool bSubMenuOpen = HoverState.IsValid() && HoverState->bSubMenuOpen;
                    
                    return (bHovered || bSubMenuOpen)
                        ? FConvaiStyle::Get().GetColor("Convai.Color.dropdownTextHover")
                        : FConvaiStyle::Get().GetColor("Convai.Color.dropdownText"); }))]];

    HoverState->SubMenuAnchor = SubMenuAnchor;

    SetupNestedHoverBehavior(ParentButton, HoverState, Entry);

    return SubMenuAnchor;
}

TSharedRef<SWidget> SConvaiNestedDropdown::CreateSubMenuContent(const FConvaiMenuEntry &Entry, TSharedPtr<FNestedDropdownHoverState> HoverState)
{
    TSharedRef<SConvaiNestedDropdown> NestedDropdown = SNew(SConvaiNestedDropdown)
                                                           .Entries(Entry.Children)
                                                           .OwningWindow(Window)
                                                           .FontStyle(FontStyleName)
                                                           .NestingLevel(CurrentNestingLevel + 1);

    return SNew(SHoverAwareMenuWrapper)
        .RenderTransform(FSlateRenderTransform(FVector2D(185.0f, -40.0f)))
        .OnMenuHoverStart(FSimpleDelegate::CreateLambda([HoverState]()
                                                        {
            if (HoverState.IsValid())
            {
                HoverState->bSubMenuHovered = true;
                HoverState->ClearTicker();
            } }))
        .OnMenuHoverEnd(FSimpleDelegate::CreateLambda([HoverState]()
                                                      {
            if (HoverState.IsValid())
            {
                HoverState->bSubMenuHovered = false;
                
                HoverState->ClearTicker();
                HoverState->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                    FTickerDelegate::CreateLambda([HoverState](float DeltaTime)
                    {
                        if (HoverState.IsValid())
                        {
                            HoverState->CloseIfNotHovered();
                        }
                        return false;
                    }), 0.3f);
            } }))
            [NestedDropdown];
}

void SConvaiNestedDropdown::SetupNestedHoverBehavior(
    TSharedPtr<SButton> Button,
    TSharedPtr<FNestedDropdownHoverState> HoverState,
    const FConvaiMenuEntry &Entry)
{
    if (!Button.IsValid() || !HoverState.IsValid())
    {
        return;
    }

    Button->SetOnMouseEnter(FNoReplyPointerEventHandler::CreateLambda([HoverState](const FGeometry &, const FPointerEvent &)
                                                                      {
        if (HoverState.IsValid())
        {
            HoverState->bParentHovered = true;
            HoverState->ClearTicker();
            
            HoverState->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([HoverState](float DeltaTime)
                {
                    if (HoverState.IsValid() && HoverState->bParentHovered)
                    {
                        HoverState->OpenSubMenu();
                    }
                    return false;
                }), 0.2f);
        } }));

    Button->SetOnMouseLeave(FSimpleNoReplyPointerEventHandler::CreateLambda([HoverState](const FPointerEvent &)
                                                                            {
        if (HoverState.IsValid())
        {
            HoverState->bParentHovered = false;
            HoverState->ClearTicker();
            
            HoverState->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([HoverState](float DeltaTime)
                {
                    if (HoverState.IsValid())
                    {
                        HoverState->CloseIfNotHovered();
                    }
                    return false;
                }), 0.3f);
        } }));
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION
