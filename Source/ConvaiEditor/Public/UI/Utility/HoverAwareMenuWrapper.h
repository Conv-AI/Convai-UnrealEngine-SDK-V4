/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HoverAwareMenuWrapper.h
 *
 * Menu wrapper widget with hover event delegates.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Styling/SlateTypes.h"
#include "Layout/Geometry.h"

/**
 * Menu wrapper widget with hover event delegates.
 */
class CONVAIEDITOR_API SHoverAwareMenuWrapper : public SBorder
{
public:
    SLATE_BEGIN_ARGS(SHoverAwareMenuWrapper)
        : _OnMenuHoverStart(), _OnMenuHoverEnd(), _Content()
    {
    }
    SLATE_EVENT(FSimpleDelegate, OnMenuHoverStart)
    SLATE_EVENT(FSimpleDelegate, OnMenuHoverEnd)
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs)
    {
        OnMenuHoverStart = InArgs._OnMenuHoverStart;
        OnMenuHoverEnd = InArgs._OnMenuHoverEnd;

        SBorder::Construct(SBorder::FArguments()
                               .BorderImage(FStyleDefaults::GetNoBrush())
                               .Padding(0)
                                   [InArgs._Content.Widget]);
    }

    virtual void OnMouseEnter(const FGeometry &MyGeometry, const FPointerEvent &MouseEvent) override
    {
        SBorder::OnMouseEnter(MyGeometry, MouseEvent);
        OnMenuHoverStart.ExecuteIfBound();
    }

    virtual void OnMouseLeave(const FPointerEvent &MouseEvent) override
    {
        SBorder::OnMouseLeave(MouseEvent);
        OnMenuHoverEnd.ExecuteIfBound();
    }

private:
    FSimpleDelegate OnMenuHoverStart;
    FSimpleDelegate OnMenuHoverEnd;
};
