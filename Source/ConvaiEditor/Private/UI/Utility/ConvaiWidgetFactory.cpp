/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiWidgetFactory.cpp
 *
 * Implementation of widget factory utilities.
 */

#include "UI/Utility/ConvaiWidgetFactory.h"
#include "Styling/ConvaiStyle.h"
#include "UI/Utility/ConvaiAccessibility.h"
#include "UI/Widgets/SCard.h"
#include "UI/Widgets/SRoundedBox.h"
#include "UI/Widgets/SContentContainer.h"
#include "UI/Widgets/SConvaiScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Images/SImage.h"
#include "Styling/ConvaiStyleResources.h"
#include "Styling/SlateTypes.h"
#include "Styling/IConvaiStyleRegistry.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SBorder.h"
#include "ConvaiEditor.h"

void FConvaiWidgetFactory::Initialize()
{
    auto Registry = FConvaiStyle::GetStyleRegistry();
    if (!Registry.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Style registry not available - button styles not registered"));
        return;
    }
    TSharedPtr<FSlateStyleSet> StyleSet = Registry->GetMutableStyleSet();
    if (!StyleSet.IsValid())
    {
        UE_LOG(LogConvaiEditor, Warning, TEXT("Style set not valid - button styles not registered"));
        return;
    }

    auto RegisterButtonStyle = [&StyleSet](const FString &StyleNamePrefix,
                                           const FLinearColor &BgColor,
                                           const FLinearColor &TextColor)
    {
        FString BgBrushKey = FString::Printf(TEXT("%s.RoundedBrush"), *StyleNamePrefix);
        auto BgBrushRes = FConvaiStyleResources::Get().GetOrCreateRoundedBoxBrush(FName(*BgBrushKey), BgColor, 8.f);
        const FSlateBrush *BgBrush = BgBrushRes.IsSuccess() ? BgBrushRes.GetValue().Get() : FConvaiStyle::GetTransparentBrush();

        FButtonStyle ButtonStyle = FButtonStyle()
                                       .SetNormal(*BgBrush)
                                       .SetHovered(*BgBrush)
                                       .SetPressed(*BgBrush)
                                       .SetDisabled(*BgBrush)
                                       .SetNormalPadding(FMargin(0.f))
                                       .SetPressedPadding(FMargin(0.f));

        StyleSet->Set(FName(*FString::Printf(TEXT("%s"), *StyleNamePrefix)), ButtonStyle);

        FTextBlockStyle TextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
        TextStyle.SetColorAndOpacity(TextColor);
        TextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));

        StyleSet->Set(FName(*FString::Printf(TEXT("%s.Text"), *StyleNamePrefix)), TextStyle);
    };

    FLinearColor PrimaryBg = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.primary.bg"));
    FLinearColor PrimaryText = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.primary.text"));
    RegisterButtonStyle(TEXT("Convai.Button.Primary"), PrimaryBg, PrimaryText);

    FLinearColor SecondaryBg = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.secondary.bg"));
    FLinearColor SecondaryText = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.secondary.text"));
    RegisterButtonStyle(TEXT("Convai.Button.Secondary"), SecondaryBg, SecondaryText);

    FLinearColor PositiveBg = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.positive.bg"));
    FLinearColor PositiveText = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.positive.text"));
    RegisterButtonStyle(TEXT("Convai.Button.Positive"), PositiveBg, PositiveText);

    FLinearColor NegativeBg = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.negative.bg"));
    FLinearColor NegativeText = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.negative.text"));
    RegisterButtonStyle(TEXT("Convai.Button.Negative"), NegativeBg, NegativeText);

    FLinearColor DisabledBg = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.disabled.bg"));
    FLinearColor DisabledText = FConvaiStyle::RequireColor(FName("Convai.Color.component.button.disabled.text"));
    RegisterButtonStyle(TEXT("Convai.Button.Disabled"), DisabledBg, DisabledText);
}

void FConvaiWidgetFactory::Shutdown()
{
}

TSharedRef<SButton> FConvaiWidgetFactory::CreatePrimaryButton(
    const FText &Text,
    FOnClicked OnClicked,
    const FText &ToolTipText)
{
    TSharedRef<SButton> Button = SNew(SButton)
                                     .ButtonStyle(FConvaiStyle::Get(), "Convai.Button.Primary")
                                     .TextStyle(FConvaiStyle::Get(), "Convai.Button.Primary.Text")
                                     .Text(Text)
                                     .ToolTipText(ToolTipText.IsEmpty() ? FText::FromString(TEXT("Primary Button")) : ToolTipText)
                                     .OnClicked(OnClicked)
                                     .HAlign(HAlign_Center)
                                     .VAlign(VAlign_Center);

    FConvaiAccessibility::ApplyAccessibleParams(Button, Text, ToolTipText);

    return Button;
}

TSharedRef<SButton> FConvaiWidgetFactory::CreateSecondaryButton(
    const FText &Text,
    FOnClicked OnClicked,
    const FText &ToolTipText)
{
    TSharedRef<SButton> Button = SNew(SButton)
                                     .ButtonStyle(FConvaiStyle::Get(), "Convai.Button.Secondary")
                                     .TextStyle(FConvaiStyle::Get(), "Convai.Button.Secondary.Text")
                                     .Text(Text)
                                     .ToolTipText(ToolTipText.IsEmpty() ? FText::FromString(TEXT("Secondary Button")) : ToolTipText)
                                     .OnClicked(OnClicked)
                                     .HAlign(HAlign_Center)
                                     .VAlign(VAlign_Center);

    FConvaiAccessibility::ApplyAccessibleParams(Button, Text, ToolTipText);

    return Button;
}

TSharedRef<SButton> FConvaiWidgetFactory::CreateIconButton(
    const FSlateBrush *Icon,
    FOnClicked OnClicked,
    const FText &ToolTipText)
{
    TSharedRef<SButton> Button = SNew(SButton)
                                     .ButtonStyle(FConvaiStyle::Get(), "Convai.Button.Icon")
                                     .ToolTipText(ToolTipText.IsEmpty() ? FText::FromString(TEXT("Icon Button")) : ToolTipText)
                                     .OnClicked(OnClicked)
                                     .HAlign(HAlign_Center)
                                     .VAlign(VAlign_Center)
                                     .Content()
                                         [SNew(SImage)
                                              .Image(Icon)
                                              .ColorAndOpacity(FSlateColor::UseForeground())];

    FConvaiAccessibility::ApplyAccessibleParams(Button, FText::GetEmpty(), ToolTipText);

    return Button;
}

TSharedRef<STextBlock> FConvaiWidgetFactory::CreateHeading(const FText &Text)
{
    return SNew(STextBlock)
        .TextStyle(FConvaiStyle::Get(), "Convai.Text.Heading")
        .Text(Text)
        .AutoWrapText(true);
}

TSharedRef<STextBlock> FConvaiWidgetFactory::CreateSubheading(const FText &Text)
{
    return SNew(STextBlock)
        .TextStyle(FConvaiStyle::Get(), "Convai.Text.Subheading")
        .Text(Text)
        .AutoWrapText(true);
}

TSharedRef<STextBlock> FConvaiWidgetFactory::CreateBodyText(const FText &Text)
{
    return SNew(STextBlock)
        .TextStyle(FConvaiStyle::Get(), "Convai.Text.Body")
        .Text(Text)
        .AutoWrapText(true);
}

TSharedRef<STextBlock> FConvaiWidgetFactory::CreateCaption(const FText &Text)
{
    return SNew(STextBlock)
        .TextStyle(FConvaiStyle::Get(), "Convai.Text.Caption")
        .Text(Text)
        .AutoWrapText(true);
}

TSharedRef<SEditableText> FConvaiWidgetFactory::CreateTextInput(
    const FText &InitialText,
    const FText &PlaceholderText,
    FOnTextChanged OnTextChanged,
    FOnTextCommitted OnTextCommitted)
{
    TSharedRef<SEditableText> TextInput = SNew(SEditableText)
                                              .Style(FConvaiStyle::Get(), "Convai.EditableText")
                                              .Text(InitialText)
                                              .HintText(PlaceholderText)
                                              .OnTextChanged(OnTextChanged)
                                              .OnTextCommitted(OnTextCommitted);

    FConvaiAccessibility::ApplyAccessibleParams(TextInput, InitialText, PlaceholderText);

    return TextInput;
}

TSharedRef<SCheckBox> FConvaiWidgetFactory::CreateCheckbox(
    bool bInitialState,
    FOnCheckStateChanged OnStateChanged,
    const FText &LabelText)
{
    TSharedRef<SCheckBox> CheckBox = SNew(SCheckBox)
                                         .Style(FConvaiStyle::Get(), "Convai.CheckBox")
                                         .IsChecked(bInitialState ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
                                         .OnCheckStateChanged(OnStateChanged)
                                         .Content()
                                             [SNew(STextBlock)
                                                  .TextStyle(FConvaiStyle::Get(), "Convai.Text.Body")
                                                  .Text(LabelText)];

    FConvaiAccessibility::ApplyAccessibleParams(CheckBox, LabelText, FText::GetEmpty());

    return CheckBox;
}

TSharedRef<SCard> FConvaiWidgetFactory::CreateCard(TSharedRef<SWidget> Content)
{
    return SNew(SCard)
        [Content];
}

TSharedRef<SRoundedBox> FConvaiWidgetFactory::CreateRoundedBox(
    TSharedRef<SWidget> Content,
    float CornerRadius)
{
    return SNew(SRoundedBox)
        .BorderRadius(CornerRadius)
            [Content];
}

TSharedRef<SContentContainer> FConvaiWidgetFactory::CreateContentContainer(TSharedRef<SWidget> Content)
{
    return SNew(SContentContainer)
        [Content];
}

TSharedRef<SConvaiScrollBox> FConvaiWidgetFactory::CreateScrollBox()
{
    return SNew(SConvaiScrollBox);
}

TSharedRef<SBox> FConvaiWidgetFactory::CreateSizedBox(
    TSharedRef<SWidget> Content,
    TOptional<float> Width,
    TOptional<float> Height,
    FMargin Padding)
{
    TSharedRef<SBox> Box = SNew(SBox)
                               .Padding(Padding)
                                   [Content];

    if (Width.IsSet())
    {
        Box->SetWidthOverride(Width.GetValue());
    }

    if (Height.IsSet())
    {
        Box->SetHeightOverride(Height.GetValue());
    }

    return Box;
}

TSharedRef<SBox> FConvaiWidgetFactory::CreateSizedBox(
    TSharedRef<SWidget> Content,
    const FVector2D &Dimensions,
    FMargin Padding)
{
    return CreateSizedBox(Content, Dimensions.X, Dimensions.Y, Padding);
}

TSharedRef<SWidget> FConvaiWidgetFactory::CreateClickableCard(
    const FText &Title,
    const FText &Description,
    const FSlateBrush *BackgroundImage,
    FOnClicked OnClicked,
    const FVector2D &CardSize,
    float BorderRadius,
    float BorderThickness)
{
    const FLinearColor CardBackgroundColor = FConvaiStyle::RequireColor(FName("Convai.Color.surface.window"));
    const FLinearColor BorderColor = FConvaiStyle::RequireColor(FName("Convai.Color.component.standardCard.outline"));

    FName BgBrushKey = FName(*FString::Printf(TEXT("CardBackground_%s"), *Title.ToString()));
    auto BgBrushResult = FConvaiStyleResources::Get().GetOrCreateColorBrush(BgBrushKey, CardBackgroundColor);

    const FSlateBrush *BackgroundBrush = BgBrushResult.IsSuccess()
                                             ? BgBrushResult.GetValue().Get()
                                             : FConvaiStyle::GetTransparentBrush();

    TSharedRef<SWidget> CardContent = SNew(SBorder)
                                          .BorderImage(BackgroundBrush)
                                          .Padding(0)
                                              [SNew(SOverlay)

                                               + SOverlay::Slot()
                                                     .HAlign(HAlign_Fill)
                                                     .VAlign(VAlign_Fill)
                                                         [SNew(SScaleBox)
                                                              .Stretch(EStretch::ScaleToFill)
                                                                  [SNew(SImage)
                                                                       .Image(BackgroundImage)
                                                                       .ColorAndOpacity(FLinearColor::White)]]

                                               + SOverlay::Slot()
                                                     .VAlign(VAlign_Bottom)
                                                     .HAlign(HAlign_Fill)
                                                         [CreateGradientOverlay()]

                                               + SOverlay::Slot()
                                                     .VAlign(VAlign_Bottom)
                                                     .HAlign(HAlign_Center)
                                                     .Padding(FMargin(16.0f, 0.0f, 16.0f, 20.0f))
                                                         [SNew(SVerticalBox)

                                                          + SVerticalBox::Slot()
                                                                .AutoHeight()
                                                                .Padding(0, 0, 0, 4)
                                                                    [SNew(STextBlock)
                                                                         .Text(Title)
                                                                         .Font(FConvaiStyle::Get().GetFontStyle("Convai.Font.supportResourceLabel"))
                                                                         .ColorAndOpacity(FLinearColor::White)
                                                                         .ShadowOffset(FVector2D(1.0f, 1.0f))
                                                                         .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f))
                                                                         .Justification(ETextJustify::Center)]

                                                          + SVerticalBox::Slot()
                                                                .AutoHeight()
                                                                    [SNew(STextBlock)
                                                                         .Text(Description)
                                                                         .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                                                         .ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f))
                                                                         .ShadowOffset(FVector2D(1.0f, 1.0f))
                                                                         .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f))
                                                                         .Justification(ETextJustify::Center)
                                                                         .Visibility(Description.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)]]];

    return CreateSizedBox(
        SNew(SCard)
            .BorderRadius(BorderRadius)
            .BorderThickness(BorderThickness)
            .BorderColor(BorderColor)
            .BackgroundColor(CardBackgroundColor)
            .OnClicked(OnClicked)
                [CardContent],
        CardSize);
}

TSharedRef<SWidget> FConvaiWidgetFactory::CreateCardGrid(
    TArray<TSharedRef<SWidget>> Cards,
    float HorizontalSpacing,
    float VerticalSpacing,
    int32 CardsPerRow)
{
    TSharedRef<SVerticalBox> GridContainer = SNew(SVerticalBox);

    for (int32 i = 0; i < Cards.Num(); i += CardsPerRow)
    {
        TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

        for (int32 j = 0; j < CardsPerRow && (i + j) < Cards.Num(); ++j)
        {
            Row->AddSlot()
                .AutoWidth()
                .Padding(j == 0 ? 0 : HorizontalSpacing / 2.0f, 0,
                         j == CardsPerRow - 1 ? 0 : HorizontalSpacing / 2.0f, 0)
                    [Cards[i + j]];
        }

        GridContainer->AddSlot()
            .AutoHeight()
            .Padding(0, i == 0 ? 0 : VerticalSpacing / 2.0f,
                     0, (i + CardsPerRow) >= Cards.Num() ? 0 : VerticalSpacing / 2.0f)
                [Row];
    }

    return GridContainer;
}

TSharedRef<SWidget> FConvaiWidgetFactory::CreateGradientOverlay(float Height)
{
    return SNew(SBox)
        .HeightOverride(Height)
            [SNew(SImage)
                 .Image(FConvaiStyle::Get().GetBrush("Convai.Gradient"))
                 .ColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f))];
}
