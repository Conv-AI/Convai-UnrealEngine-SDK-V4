/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SCircularAvatar.cpp
 *
 * Implementation of the circular avatar widget.
 */

#include "UI/Components/SCircularAvatar.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/AvatarHelpers.h"

void SCircularAvatar::Construct(const FArguments &InArgs)
{
    UsernameAttribute = InArgs._Username;
    Size = InArgs._Size;
    FontSize = InArgs._FontSize;

    Username = UsernameAttribute.Get();

    UpdateAvatarProperties();

    UpdateCircleBrush();

    VerticalOffset = FontSize * 0.08f;

    ChildSlot
        [SNew(SBox)
             .WidthOverride(Size)
             .HeightOverride(Size)
                 [SNew(SBorder)
                      .BorderImage_Lambda([this]() -> const FSlateBrush *
                                          {
        FString CurrentUsername = GetCurrentUsername();
        if (CurrentUsername != Username)
        {
            const_cast<SCircularAvatar *>(this)->Username = CurrentUsername;
            const_cast<SCircularAvatar *>(this)->UpdateAvatarProperties();
            const_cast<SCircularAvatar *>(this)->UpdateCircleBrush();
        }

        return CachedCircleBrush.Get(); })
                      .HAlign(HAlign_Fill)
                      .VAlign(VAlign_Fill)
                      .Padding(FMargin(0.f, VerticalOffset, 0.f, -VerticalOffset))
                          [SNew(SBox)
                               .HAlign(HAlign_Center)
                               .VAlign(VAlign_Center)
                                   [SNew(STextBlock)
                                        .Text_Lambda([this]() -> FText
                                                     {
        FString CurrentUsername = GetCurrentUsername();
        if (CurrentUsername != Username)
        {
            const_cast<SCircularAvatar *>(this)->Username = CurrentUsername;
            const_cast<SCircularAvatar *>(this)->UpdateAvatarProperties();
        }
        return FText::FromString(Initials); })
                                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", FontSize))
                                        .ColorAndOpacity(FLinearColor::White)
                                        .Justification(ETextJustify::Center)]]]];
}

void SCircularAvatar::SetUsername(const FString &InUsername)
{
    if (Username != InUsername)
    {
        Username = InUsername;
        UpdateAvatarProperties();
        UpdateCircleBrush();

        Invalidate(EInvalidateWidget::Layout);
    }
}

void SCircularAvatar::UpdateAvatarProperties()
{
    using namespace ConvaiEditor::AvatarHelpers;

    if (IsValidUsername(Username))
    {
        Initials = ExtractInitials(Username);
        BackgroundColor = GenerateAvatarColor(Username);
    }
    else
    {
        Initials = TEXT("??");
        BackgroundColor = GetFallbackColor();
    }
}

void SCircularAvatar::UpdateCircleBrush()
{
    CachedCircleBrush = MakeShared<FSlateRoundedBoxBrush>(
        BackgroundColor,
        Size / 2.0f);
}

FString SCircularAvatar::GetCurrentUsername() const
{
    return UsernameAttribute.Get();
}
