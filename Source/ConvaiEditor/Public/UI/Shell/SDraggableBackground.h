/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SDraggableBackground.h
 *
 * Wrapper widget enabling window dragging on empty areas.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"

/**
 * Wrapper widget enabling window dragging on empty areas.
 */
class CONVAIEDITOR_API SDraggableBackground : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDraggableBackground)
    {
    }
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_ARGUMENT(TWeakPtr<SWindow>, ParentWindow)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    virtual FReply OnMouseButtonDown(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent) override;
    virtual FReply OnMouseMove(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent) override;
    virtual FReply OnMouseButtonUp(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent) override;

private:
    bool CanDragWindow(const FPointerEvent &MouseEvent);

    TWeakPtr<SWindow> ParentWindow;
    bool bIsDragging = false;
    FVector2D DragStartPosition;
    FVector2D WindowStartPosition;
};
