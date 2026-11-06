/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiWidgetFactory.h
 *
 * Widget creation utilities with consistent styling.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/ConvaiStyle.h"

class SButton;
class STextBlock;
class SEditableText;
class SCheckBox;
class SCard;
class SRoundedBox;
class SContentContainer;
class SConvaiScrollBox;
class SBox;
class SWidget;
struct FSlateBrush;

/**
 * Widget creation utilities with consistent styling.
 */
class CONVAIEDITOR_API FConvaiWidgetFactory
{
public:
    /** Creates a primary button */
    static TSharedRef<SButton> CreatePrimaryButton(
        const FText &Text,
        FOnClicked OnClicked,
        const FText &ToolTipText = FText::GetEmpty());

    /** Creates a secondary button */
    static TSharedRef<SButton> CreateSecondaryButton(
        const FText &Text,
        FOnClicked OnClicked,
        const FText &ToolTipText = FText::GetEmpty());

    /** Creates an icon button */
    static TSharedRef<SButton> CreateIconButton(
        const FSlateBrush *Icon,
        FOnClicked OnClicked,
        const FText &ToolTipText = FText::GetEmpty());

    /** Creates a heading text block */
    static TSharedRef<STextBlock> CreateHeading(const FText &Text);

    /** Creates a subheading text block */
    static TSharedRef<STextBlock> CreateSubheading(const FText &Text);

    /** Creates a body text block */
    static TSharedRef<STextBlock> CreateBodyText(const FText &Text);

    /** Creates a caption text block */
    static TSharedRef<STextBlock> CreateCaption(const FText &Text);

    /** Creates a text input field */
    static TSharedRef<SEditableText> CreateTextInput(
        const FText &InitialText = FText::GetEmpty(),
        const FText &PlaceholderText = FText::GetEmpty(),
        FOnTextChanged OnTextChanged = FOnTextChanged(),
        FOnTextCommitted OnTextCommitted = FOnTextCommitted());

    /** Creates a checkbox */
    static TSharedRef<SCheckBox> CreateCheckbox(
        bool bInitialState = false,
        FOnCheckStateChanged OnStateChanged = FOnCheckStateChanged(),
        const FText &LabelText = FText::GetEmpty());

    /** Creates a card container */
    static TSharedRef<SCard> CreateCard(TSharedRef<SWidget> Content);

    /** Creates a rounded box container */
    static TSharedRef<SRoundedBox> CreateRoundedBox(
        TSharedRef<SWidget> Content,
        float CornerRadius = 8.0f);

    /** Creates a content container */
    static TSharedRef<SContentContainer> CreateContentContainer(TSharedRef<SWidget> Content);

    /** Creates a scrollable container */
    static TSharedRef<SConvaiScrollBox> CreateScrollBox();

    /** Creates a sized box */
    static TSharedRef<SBox> CreateSizedBox(
        TSharedRef<SWidget> Content,
        TOptional<float> Width = TOptional<float>(),
        TOptional<float> Height = TOptional<float>(),
        FMargin Padding = FMargin(0.0f));

    /** Creates a sized box with dimensions */
    static TSharedRef<SBox> CreateSizedBox(
        TSharedRef<SWidget> Content,
        const FVector2D &Dimensions,
        FMargin Padding = FMargin(0.0f));

    /** Creates a clickable card */
    static TSharedRef<SWidget> CreateClickableCard(
        const FText &Title,
        const FText &Description,
        const FSlateBrush *BackgroundImage,
        FOnClicked OnClicked,
        const FVector2D &CardSize,
        float BorderRadius = 8.0f,
        float BorderThickness = 0.0f);

    /** Creates a card grid layout */
    static TSharedRef<SWidget> CreateCardGrid(
        TArray<TSharedRef<SWidget>> Cards,
        float HorizontalSpacing = 16.0f,
        float VerticalSpacing = 16.0f,
        int32 CardsPerRow = 2);

    static void Initialize();
    static void Shutdown();

private:
    static TSharedRef<SWidget> CreateGradientOverlay(float Height = 150.0f);
};
