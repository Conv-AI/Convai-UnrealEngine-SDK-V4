/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SBasePage.h
 *
 * Base class for all page widgets.
 */

#pragma once

#include "Widgets/SCompoundWidget.h"
#include "MVVM/ViewModel.h"

/**
 * Base class for all page widgets.
 */
class SBasePage : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SBasePage) {}
    SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments &InArgs)
    {
        ChildSlot
            [InArgs._Content.Widget];
    }

    /** Returns the ViewModel associated with this page */
    virtual TSharedPtr<FViewModelBase> GetViewModel() const
    {
        return nullptr;
    }

    static FName StaticClass()
    {
        static FName TypeName = FName("SBasePage");
        return TypeName;
    }

    virtual bool IsA(const FName &TypeName) const
    {
        return TypeName == StaticClass();
    }

    virtual void OnPageActivated() {}
};
