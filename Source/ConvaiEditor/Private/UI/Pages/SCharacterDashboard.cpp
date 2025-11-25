/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SCharacterDashboard.cpp
 *
 * Implementation of the character dashboard page.
 */

#include "UI/Pages/SCharacterDashboard.h"
#include "MVVM/CharacterDashboardViewModel.h"
#include "Models/ConvaiCharacterMetadata.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "UI/Widgets/SConvaiScrollBox.h"

void SCharacterDashboard::Construct(const FArguments &InArgs)
{
    ViewModel = InArgs._ViewModel;

    SAssignNew(CharacterListContainer, SVerticalBox);

    TSharedRef<SConvaiScrollBox> ScrollBox = SNew(SConvaiScrollBox)
                                                 .ScrollBarAlwaysVisible(false)
                                                 .ShowShadow(false)
                                                 .CustomScrollBarPadding(FMargin(0.f, 10.0f));

    ScrollBox->AddSlot()
        [CharacterListContainer.ToSharedRef()];

    ChildSlot
        [SNew(SBox)
             .HeightOverride(229.0f)
                 [ScrollBox]];

    if (ViewModel.IsValid())
    {
        TWeakPtr<SCharacterDashboard> WeakSelf(SharedThis(this));

        // Bind to ViewModel updates
        CharacterListUpdatedHandle = ViewModel->OnCharacterListUpdated().AddLambda([WeakSelf]()
                                                                                   {
            if (TSharedPtr<SCharacterDashboard> Dashboard = WeakSelf.Pin())
            {
                Dashboard->RefreshCharacterList();
            } });

        RefreshCharacterList();
    }
}

SCharacterDashboard::~SCharacterDashboard()
{
    if (ViewModel.IsValid() && CharacterListUpdatedHandle.IsValid())
    {
        ViewModel->OnCharacterListUpdated().Remove(CharacterListUpdatedHandle);
    }
}

void SCharacterDashboard::RefreshCharacterList()
{
    if (!CharacterListContainer.IsValid() || !ViewModel.IsValid())
    {
        return;
    }

    TWeakPtr<SCharacterDashboard> WeakWidget = SharedThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakWidget]()
              {
        TSharedPtr<SCharacterDashboard> Widget = WeakWidget.Pin();
        if (!Widget.IsValid())
        {
            return;
        }

        if (!Widget->CharacterListContainer.IsValid() || !Widget->ViewModel.IsValid())
        {
            return;
        }

        Widget->CharacterListContainer->ClearChildren();
        const auto &Characters = Widget->ViewModel->GetCharacters();
        for (const auto &Character : Characters)
        {
            Widget->CharacterListContainer->AddSlot()
                .AutoHeight()
                .Padding(0, 8, 0, 0)
                    [Widget->GenerateCharacterRow(Character)];
        } });
}

TSharedRef<SWidget> SCharacterDashboard::GenerateCharacterRow(const TSharedPtr<FConvaiCharacterMetadata> &CharacterMetadata) const
{
    FTextBlockStyle CharacterNameTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    CharacterNameTextStyle.SetColorAndOpacity(FConvaiStyle::RequireColor("Convai.Color.text.primary"));
    CharacterNameTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 20));

    const FSlateColor InactiveColor = FConvaiStyle::Get().GetSlateColor("Convai.Color.feature.inactive");
    const FSlateColor ActiveColor = FConvaiStyle::Get().GetSlateColor("Convai.Color.feature.active");

    FString FullName = CharacterMetadata->CharacterName;
    FString FirstName;
    if (FullName.Split(TEXT(" "), &FirstName, nullptr))
    {
        // FirstName already set
    }
    else
    {
        FirstName = FullName;
    }

    return SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(0.5f).VAlign(VAlign_Center).HAlign(HAlign_Center).Padding(0, 0, 30, 0)[SNew(STextBlock).Text(FText::FromString(FirstName)).TextStyle(&CharacterNameTextStyle).Justification(ETextJustify::Center)] + SHorizontalBox::Slot().FillWidth(0.5f).VAlign(VAlign_Center).HAlign(HAlign_Center).Padding(30, 0, 0, 0)[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SImage).Image(FConvaiStyle::Get().GetBrush("Convai.Icon.Actions")).ColorAndOpacity(InactiveColor).DesiredSizeOverride(FVector2D(16, 16))] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)[SNew(SImage).Image(FConvaiStyle::Get().GetBrush("Convai.Icon.NarrativeDesign")).ColorAndOpacity(CharacterMetadata->bIsNarrativeDriven ? ActiveColor : InactiveColor).DesiredSizeOverride(FVector2D(16, 16))] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)[SNew(SImage).Image(FConvaiStyle::Get().GetBrush("Convai.Icon.LongTermMemory")).ColorAndOpacity(CharacterMetadata->bIsLongTermMemoryEnabled ? ActiveColor : InactiveColor).DesiredSizeOverride(FVector2D(16, 16))]];
}

const FSlateBrush *SCharacterDashboard::GetFeatureIconBrush(const FString &FeatureName, bool bIsActive) const
{
    FName BrushKey;
    if (FeatureName == "Action")
    {
        BrushKey = FName("Convai.Icon.Actions");
    }
    else if (FeatureName == "Narrative")
    {
        BrushKey = FName("Convai.Icon.NarrativeDesign");
    }
    else if (FeatureName == "LTM")
    {
        BrushKey = FName("Convai.Icon.LongTermMemory");
    }
    else
    {
        return nullptr;
    }
    const ISlateStyle *Style = &FConvaiStyle::Get();
    return Style ? Style->GetBrush(BrushKey) : nullptr;
}

FSlateColor SCharacterDashboard::GetFeatureIconColor(bool bIsActive) const
{
    return bIsActive
               ? FConvaiStyle::Get().GetSlateColor("Convai.Color.feature.active")
               : FConvaiStyle::Get().GetSlateColor("Convai.Color.feature.inactive");
}