/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SSamplesPage.h
 *
 * Samples page displaying sample items in a responsive grid.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Pages/SBasePage.h"
#include "Widgets/Views/SListView.h"
#include "UI/Widgets/SConvaiScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "UI/Widgets/SCard.h"
#include "Styling/ConvaiStyle.h"
#include "MVVM/SamplesViewModel.h"
#include "Utility/ConvaiConstants.h"

class STextBlock;

/**
 * Samples page displaying sample items in a responsive grid.
 */
class CONVAIEDITOR_API SSamplesPage : public SBasePage
{
public:
    SLATE_BEGIN_ARGS(SSamplesPage) {}
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    virtual ~SSamplesPage();

    virtual TSharedPtr<FViewModelBase> GetViewModel() const override;

    static FName StaticClass();

    virtual bool IsA(const FName &TypeName) const override;

    virtual void Tick(const FGeometry &AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
    void OnViewModelInvalidated();
    void RefreshSampleCards();
    FReply OnSampleCardClicked(TSharedPtr<FSampleItem> ClickedItem);

    TSharedPtr<FSamplesViewModel> ViewModel;
    TSharedPtr<SUniformGridPanel> GridPanel;
    FVector2D CardSize;
    float CardSpacing = 0.0f;
    int32 CurrentColumns = 0;
    float LastPanelWidth = 0.0f;
    float ColumnRecalculationThreshold = 5.0f;
};
