/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiShell.h
 *
 * Main SDK window with page navigation.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Shell/SBaseShell.h"
#include "Styling/ConvaiStyle.h"
#include "UI/Header/SHeaderBar.h"
#include "InputCoreTypes.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "UI/Shell/SDraggableBackground.h"
#include "Services/IUIContainer.h"
#include "Utility/ConvaiConstants.h"

#define LOCTEXT_NAMESPACE "SConvaiShell"

/** Main SDK window with page navigation. */
class SConvaiShell : public SBaseShell, public IUIContainer
{
public:
    SLATE_BEGIN_ARGS(SConvaiShell)
        : _InitialWidth(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowWidth)), _InitialHeight(static_cast<int32>(ConvaiEditor::Constants::Layout::Window::MainWindowHeight)), _MinWidth(ConvaiEditor::Constants::Layout::Window::MainWindowMinWidth), _MinHeight(ConvaiEditor::Constants::Layout::Window::MainWindowMinHeight), _ShouldBeTopmost(false)
    {
    }
    SLATE_ARGUMENT(int32, InitialWidth)
    SLATE_ARGUMENT(int32, InitialHeight)
    SLATE_ARGUMENT(float, MinWidth)
    SLATE_ARGUMENT(float, MinHeight)
    SLATE_ARGUMENT(bool, ShouldBeTopmost)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs)
    {
        const float WindowPad = ConvaiEditor::Constants::Layout::Spacing::Window;
        const float ContentPad = ConvaiEditor::Constants::Layout::Spacing::Content;

        SBaseShell::Construct(SBaseShell::FArguments()
                                  .InitialWidth(InArgs._InitialWidth)
                                  .InitialHeight(InArgs._InitialHeight)
                                  .MinWidth(InArgs._MinWidth)
                                  .MinHeight(InArgs._MinHeight)
                                  .AllowClose(true)
                                  .IsTopmostWindow(InArgs._ShouldBeTopmost));

        TSharedRef<SWidget> ShellContent =
            SNew(SDraggableBackground)
                .ParentWindow(SharedThis(this))
                    [SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(SHeaderBar)] + SVerticalBox::Slot().FillHeight(1.f).Padding(WindowPad)[SNew(SBox).Padding(0)[SNew(SOverlay) + SOverlay::Slot()[SNew(SBox).Padding(0)[SNew(SImage).Image(FConvaiStyle::GetContentContainerBrush())]] + SOverlay::Slot().Padding(FMargin(0.f))[SAssignNew(PageSwitcher, SWidgetSwitcher).Visibility(EVisibility::SelfHitTestInvisible)]]]];
        SetShellContent(ShellContent);
    }

    virtual int32 AddPage(TSharedRef<SWidget> Content) override
    {
        if (PageSwitcher.IsValid())
        {
            const int32 NewIndex = PageSwitcher->GetNumWidgets();
            PageSwitcher->AddSlot()[Content];
            return NewIndex;
        }
        return INDEX_NONE;
    }

    virtual void ShowPage(int32 Index) override
    {
        if (PageSwitcher.IsValid() && PageSwitcher->GetNumWidgets() > Index)
        {
            PageSwitcher->SetActiveWidgetIndex(Index);
        }
    }

    virtual bool IsValid() const override
    {
        return PageSwitcher.IsValid();
    }

    virtual int32 GetPageCount() const override
    {
        return PageSwitcher.IsValid() ? PageSwitcher->GetNumWidgets() : 0;
    }

    virtual TSharedPtr<SWidget> GetPage(int32 PageIndex) const override
    {
        if (PageSwitcher.IsValid() && PageIndex >= 0 && PageIndex < PageSwitcher->GetNumWidgets())
        {
            return PageSwitcher->GetWidget(PageIndex);
        }
        return nullptr;
    }

private:
    TSharedPtr<SWidgetSwitcher> PageSwitcher;
};

#undef LOCTEXT_NAMESPACE
