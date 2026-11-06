/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SDraggableBackground.cpp
 *
 * Implementation of the draggable background widget for window dragging.
 */

#include "UI/Shell/SDraggableBackground.h"
#include "Framework/Application/SlateApplication.h"
#include "SlateOptMacros.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Colors/SColorBlock.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

void SDraggableBackground::Construct(const FArguments &InArgs)
{
    ParentWindow = InArgs._ParentWindow;

    ChildSlot
        [SNew(SOverlay)

         + SOverlay::Slot()
               [SNew(SColorBlock)
                    .Color(FConvaiStyle::RequireColor("Convai.Color.windowBackground"))]

         + SOverlay::Slot()
               [InArgs._Content.Widget]];
}

FReply SDraggableBackground::OnMouseButtonDown(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent)
{
    if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
    {
        if (CanDragWindow(MouseEvent))
        {
            bIsDragging = true;
            DragStartPosition = MouseEvent.GetScreenSpacePosition();

            if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
            {
                WindowStartPosition = Window->GetPositionInScreen();
                return FReply::Handled().CaptureMouse(SharedThis(this));
            }
        }
    }

    return FReply::Unhandled();
}

FReply SDraggableBackground::OnMouseMove(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent)
{
    if (bIsDragging && HasMouseCapture())
    {
        if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
        {
            FVector2D CurrentPosition = MouseEvent.GetScreenSpacePosition();
            FVector2D Delta = CurrentPosition - DragStartPosition;

            Window->MoveWindowTo(WindowStartPosition + Delta);
            return FReply::Handled();
        }
    }

    return FReply::Unhandled();
}

FReply SDraggableBackground::OnMouseButtonUp(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent)
{
    if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && bIsDragging)
    {
        bIsDragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    return FReply::Unhandled();
}

bool SDraggableBackground::CanDragWindow(const FPointerEvent &MouseEvent)
{
    FWidgetPath WidgetsUnderCursor = FSlateApplication::Get().LocateWindowUnderMouse(
        MouseEvent.GetScreenSpacePosition(),
        FSlateApplication::Get().GetInteractiveTopLevelWindows(),
        false);

    if (WidgetsUnderCursor.IsValid())
    {
        for (int32 i = WidgetsUnderCursor.Widgets.Num() - 1; i >= 0; --i)
        {
            const FArrangedWidget &ArrangedWidget = WidgetsUnderCursor.Widgets[i];
            TSharedRef<SWidget> Widget = ArrangedWidget.Widget;

            const FName WidgetType = Widget->GetType();
            if (WidgetType == TEXT("SBorder") ||
                WidgetType == TEXT("SBox") ||
                WidgetType == TEXT("SOverlay") ||
                WidgetType == TEXT("SVerticalBox") ||
                WidgetType == TEXT("SHorizontalBox") ||
                WidgetType == TEXT("SWidgetSwitcher") ||
                WidgetType == TEXT("SDraggableBackground") ||
                WidgetType == TEXT("SWindow"))
            {
                continue;
            }

            if (Widget->IsInteractable())
            {
                return false;
            }
        }
    }

    return true;
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION
