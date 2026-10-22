/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SSupportPage.h
 *
 * Support page displaying documentation and help resources.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/Pages/SBasePage.h"

class SRoundedBox;
class SCard;
class SImage;
class STextBlock;
class SOverlay;
class SButton;
class SWidget;
class SBox;
class SHorizontalBox;

/**
 * Support page displaying documentation and help resources.
 */
class CONVAIEDITOR_API SSupportPage : public SBasePage
{
public:
    SLATE_BEGIN_ARGS(SSupportPage)
    {
    }
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    static FName StaticClass()
    {
        static FName TypeName = FName("SSupportPage");
        return TypeName;
    }

    virtual bool IsA(const FName &TypeName) const override
    {
        return TypeName == StaticClass() || SBasePage::IsA(TypeName);
    }

private:
    TSharedRef<SWidget> CreateSupportCard(const FSlateBrush *ImageBrush, const FText &LabelText, float InBorderThickness, FOnClicked OnClickedDelegate = FOnClicked());
    TSharedRef<SWidget> BuildResourceCard(const FSlateBrush *ImageBrush, const FText &LabelText);

    FReply OnDocumentationCardClicked();
    FReply OnYouTubeCardClicked();
    FReply OnForumCardClicked();

    const FSlateBrush *DocumentationImageBrush = nullptr;
    const FSlateBrush *YoutubeTutorialsImageBrush = nullptr;
    const FSlateBrush *DeveloperForumImageBrush = nullptr;
    TSharedPtr<SHorizontalBox> CardsContainer;
    FVector2D BaseResourceCardSize;
    FVector2D CurrentCardSize;
    float ResourceCardBorderRadius = 8.0f;
    float ResourceCardSpacing = 20.0f;
};
