/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SCharacterDashboard.h
 *
 * Character dashboard widget displaying Convai characters.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCharacterDashboardViewModel;

/**
 * Character dashboard widget displaying Convai characters.
 */
class SCharacterDashboard : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCharacterDashboard) {}
    SLATE_ARGUMENT(TSharedPtr<FCharacterDashboardViewModel>, ViewModel)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    /** Destructor */
    ~SCharacterDashboard();

    /** Refreshes the character list UI */
    void RefreshCharacterList();

private:
    TSharedRef<SWidget> GenerateCharacterRow(const TSharedPtr<struct FConvaiCharacterMetadata> &CharacterMetadata) const;
    const FSlateBrush *GetFeatureIconBrush(const FString &FeatureName, bool bIsActive) const;
    FSlateColor GetFeatureIconColor(bool bIsActive) const;

    TSharedPtr<FCharacterDashboardViewModel> ViewModel;
    FDelegateHandle CharacterListUpdatedHandle;
    TSharedPtr<SVerticalBox> CharacterListContainer;
};
