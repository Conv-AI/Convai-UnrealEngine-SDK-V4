/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SSamplesPage.cpp
 *
 * Implementation of the samples page.
 */

#include "UI/Pages/SSamplesPage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"
#include "MVVM/SamplesViewModel.h"
#include "MVVM/ViewModel.h"
#include "UI/Widgets/SCard.h"

void SSamplesPage::Construct(const FArguments &InArgs)
{
    ViewModel = FViewModelRegistry::Get().CreateViewModel<FSamplesViewModel>();
    if (ViewModel.IsValid() && !ViewModel->IsInitialized())
    {
        ViewModel->Initialize();
    }

    CardSize = ConvaiEditor::Constants::Layout::Components::SampleCard::Dimensions;
    CardSpacing = ConvaiEditor::Constants::Layout::Spacing::SampleCardSpacing;

    const FVector2D PageOuterPadding = ConvaiEditor::Constants::Layout::Components::SamplesPage::OuterPadding;

    const float InnerPagePaddingAmount = CardSpacing;

    TSharedPtr<SScrollBox> ThemedScrollBox;
    SAssignNew(ThemedScrollBox, SConvaiScrollBox)
        .ScrollBarAlwaysVisible(false)
        .ShowShadow(true);

    ThemedScrollBox->AddSlot()
        .Padding(FMargin(0.f))
            [SNew(SBox)
                 .Padding(FMargin(InnerPagePaddingAmount))
                 .HAlign(HAlign_Fill)
                 .VAlign(VAlign_Top)
                     [SNew(SBox)
                          .HAlign(HAlign_Center)
                              [SAssignNew(GridPanel, SUniformGridPanel)
                                   .SlotPadding(FMargin(CardSpacing / 2.f))]]];

    SBasePage::Construct(SBasePage::FArguments()
                             [SNew(SVerticalBox)

                              + SVerticalBox::Slot()
                                    .FillHeight(1.f)
                                    // Use the theme-driven FVector2D padding for FMargin
                                    .Padding(FMargin(PageOuterPadding.X, PageOuterPadding.Y, PageOuterPadding.X, PageOuterPadding.Y))
                                        [ThemedScrollBox.ToSharedRef()]]);

    RefreshSampleCards();

    ViewModel->OnInvalidated().AddSP(this, &SSamplesPage::OnViewModelInvalidated);
}

SSamplesPage::~SSamplesPage()
{
    if (ViewModel.IsValid())
    {
        ViewModel->OnInvalidated().RemoveAll(this);
    }
}

TSharedPtr<FViewModelBase> SSamplesPage::GetViewModel() const
{
    return ViewModel;
}

FName SSamplesPage::StaticClass()
{
    static FName TypeName = FName("SSamplesPage");
    return TypeName;
}

bool SSamplesPage::IsA(const FName &TypeName) const
{
    return TypeName == StaticClass() || SBasePage::IsA(TypeName);
}

void SSamplesPage::Tick(const FGeometry &AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
    SBasePage::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

    if (GridPanel.IsValid() && ViewModel.IsValid())
    {
        const FVector2D PanelSize = AllottedGeometry.GetLocalSize();

        if (FMath::Abs(LastPanelWidth - PanelSize.X) > ColumnRecalculationThreshold)
        {
            LastPanelWidth = PanelSize.X;

            const int32 NewMaxColumns = FMath::Max(1, FMath::FloorToInt(
                                                          (PanelSize.X - CardSpacing) / (CardSize.X + CardSpacing)));

            if (NewMaxColumns != CurrentColumns)
            {
                CurrentColumns = NewMaxColumns;
                RefreshSampleCards();
            }
        }
    }
}

void SSamplesPage::OnViewModelInvalidated()
{
    RefreshSampleCards();
}

void SSamplesPage::RefreshSampleCards()
{
    if (!GridPanel.IsValid() || !ViewModel.IsValid())
        return;

    GridPanel->ClearChildren();

    if (CurrentColumns <= 0)
    {
        CurrentColumns = 3;
    }

    const TArray<TSharedPtr<FSampleItem>> &Items = ViewModel->GetItems();
    for (int32 Index = 0; Index < Items.Num(); ++Index)
    {
        const TSharedPtr<FSampleItem> &SampleItem = Items[Index];

        int32 Row = Index / CurrentColumns;
        int32 Column = Index % CurrentColumns;

        GridPanel->AddSlot(Column, Row)
            [SNew(SBox)
                 .WidthOverride(CardSize.X + CardSpacing) // Add extra space for padding
                 .HeightOverride(CardSize.Y + CardSpacing)
                     [SNew(SOverlay) + SOverlay::Slot()
                                           .HAlign(HAlign_Center)
                                           .VAlign(VAlign_Center)
                                               [SNew(SBox)
                                                    .WidthOverride(CardSize.X)
                                                    .HeightOverride(CardSize.Y)
                                                    .MaxDesiredWidth(CardSize.X)
                                                    .MaxDesiredHeight(CardSize.Y)
                                                        [SNew(SCard)
                                                             .SampleItem(SampleItem)
                                                             .OnClicked(this, &SSamplesPage::OnSampleCardClicked, SampleItem)]]]];
    }
}

FReply SSamplesPage::OnSampleCardClicked(TSharedPtr<FSampleItem> ClickedItem)
{
    return FReply::Handled();
}
