/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAccountMenu.cpp
 *
 * Implementation of the account menu popup widget.
 */

#include "UI/Components/SAccountMenu.h"
#include "UI/Components/SCircularAvatar.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Styling/ConvaiStyle.h"
#include "Utility/ConvaiConstants.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "EditorStyleSet.h"

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#define CONVAI_EDITOR_STYLE FAppStyle
#else
#define CONVAI_EDITOR_STYLE FEditorStyle
#endif

void SAccountMenu::Construct(const FArguments &InArgs)
{
    Username = InArgs._Username;
    Email = InArgs._Email;
    OnManageAccountClicked = InArgs._OnManageAccountClicked;
    OnSignOutClicked = InArgs._OnSignOutClicked;

    const ISlateStyle &Style = FConvaiStyle::Get();
    using namespace ConvaiEditor::Constants::Layout::Components::AccountMenu;

    ChildSlot
        [SNew(SBox)
             .WidthOverride(Width)
                 [SNew(SBorder)
                      .BorderImage_Lambda([&Style]() -> const FSlateBrush *
                                          {
                static TSharedPtr<FSlateRoundedBoxBrush> MenuBrush;
                if (!MenuBrush.IsValid())
                {
                    MenuBrush = MakeShared<FSlateRoundedBoxBrush>(
                        Style.GetColor("Convai.Color.component.accountMenu.bg"),
                        BorderRadius,
                        Style.GetColor("Convai.Color.component.accountMenu.manageAccountBorder"),
                        BorderThickness
                    );
                }
                return MenuBrush.Get(); })
                      .Padding(ContentPadding)
                          [SNew(SVerticalBox)

                           + SVerticalBox::Slot()
                                 .AutoHeight()
                                 .HAlign(HAlign_Center)
                                 .Padding(ItemPaddingHorizontal, 0.0f, ItemPaddingHorizontal, UserInfoToButtonSpacing)
                                     [BuildUserInfoSection()]

                           + SVerticalBox::Slot()
                                 .AutoHeight()
                                 .HAlign(HAlign_Center)
                                 .Padding(0.0f, 0.0f, 0.0f, ButtonToDividerSpacing)
                                     [BuildManageAccountButton()]

                           + SVerticalBox::Slot()
                                 .AutoHeight()
                                 .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                                     [BuildDivider()]

                           + SVerticalBox::Slot()
                                 .AutoHeight()
                                 .HAlign(HAlign_Fill)
                                 .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                                     [BuildSignOutItem()]]]];
}

TSharedRef<SWidget> SAccountMenu::BuildUserInfoSection()
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    using namespace ConvaiEditor::Constants::Layout::Components::AccountMenu;

    return SNew(SVerticalBox)

           + SVerticalBox::Slot()
                 .AutoHeight()
                 .HAlign(HAlign_Center)
                 .Padding(0.0f, 0.0f, 0.0f, AvatarToTextSpacing)
                     [SNew(SCircularAvatar)
                          .Username(Username)
                          .Size(AvatarSize)
                          .FontSize(AvatarFontSize)]

           + SVerticalBox::Slot()
                 .AutoHeight()
                 .HAlign(HAlign_Center)
                     [SNew(STextBlock)
                          .Text(FText::FromString(Username))
                          .Font(FCoreStyle::GetDefaultFontStyle("Regular", UsernameFontSize))
                          .ColorAndOpacity(Style.GetColor("Convai.Color.component.accountMenu.textPrimary"))
                          .Justification(ETextJustify::Center)]

           + SVerticalBox::Slot()
                 .AutoHeight()
                 .HAlign(HAlign_Center)
                 .Padding(0.0f, UsernameToEmailSpacing, 0.0f, 0.0f)
                     [SNew(STextBlock)
                          .Text(FText::FromString(Email))
                          .Font(FCoreStyle::GetDefaultFontStyle("Regular", EmailFontSize))
                          .ColorAndOpacity(Style.GetColor("Convai.Color.component.accountMenu.textSecondary"))
                          .Justification(ETextJustify::Center)];
}

TSharedRef<SWidget> SAccountMenu::BuildManageAccountButton()
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    using namespace ConvaiEditor::Constants::Layout::Components::AccountMenu;

    return SAssignNew(ManageButton, SButton)
        .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
        .OnClicked(this, &SAccountMenu::HandleManageAccountClicked)
            [SNew(SBox)
                 .HeightOverride(ManageButtonHeight)
                     [SNew(SBorder)
                          .BorderImage_Lambda([this, &Style]() -> const FSlateBrush *
                                              {
                    static TSharedPtr<FSlateRoundedBoxBrush> ButtonBrush;
                    static TSharedPtr<FSlateRoundedBoxBrush> HoverBrush;
                    
                    bool bButtonHovered = ManageButton.IsValid() && ManageButton->IsHovered();
                    
                    if (bButtonHovered)
                    {
                        if (!HoverBrush.IsValid())
                        {
                            FLinearColor BorderColor = Style.GetColor("Convai.Color.component.accountMenu.manageAccountBorderHover");
                            FLinearColor BgColor = Style.GetColor("Convai.Color.component.accountMenu.manageAccountBgHover");
                            
                            HoverBrush = MakeShared<FSlateRoundedBoxBrush>(
                                BgColor,
                                ManageButtonRadius,
                                BorderColor,
                                ManageButtonBorderWidth
                            );
                        }
                        return HoverBrush.Get();
                    }
                    else
                    {
                        if (!ButtonBrush.IsValid())
                        {
                            FLinearColor BorderColor = Style.GetColor("Convai.Color.component.accountMenu.manageAccountBorder");
                            FLinearColor BgColor = Style.GetColor("Convai.Color.component.accountMenu.manageAccountBg");
                            
                            ButtonBrush = MakeShared<FSlateRoundedBoxBrush>(
                                BgColor,
                                ManageButtonRadius,
                                BorderColor,
                                ManageButtonBorderWidth
                            );
                        }
                        return ButtonBrush.Get();
                    } })
                          .Padding(FMargin(10.0f, 0.0f))
                              [SNew(SHorizontalBox)

                               + SHorizontalBox::Slot()
                                     .AutoWidth()
                                     .VAlign(VAlign_Center)
                                         [SNew(STextBlock)
                                              .Text(FText::FromString("Manage account"))
                                              .Font(FCoreStyle::GetDefaultFontStyle("Regular", 11.0f))
                                              .ColorAndOpacity_Lambda([this, &Style]() -> FSlateColor
                                                                      {
                                                  if (ManageButton.IsValid() && ManageButton->IsHovered())
                                                  {
                                                      return Style.GetColor("Convai.Color.component.accountMenu.manageAccountTextHover");
                                                  }
                                                  return Style.GetColor("Convai.Color.component.accountMenu.manageAccountText"); })]

                               + SHorizontalBox::Slot()
                                     .AutoWidth()
                                     .VAlign(VAlign_Center)
                                     .Padding(6.0f, 0.0f, 0.0f, 0.0f)
                                         [SNew(SImage)
                                              .Image(Style.GetBrush("Convai.Icon.ExternalLink"))
                                              .DesiredSizeOverride(FVector2D(12.0f, 12.0f))
                                              .ColorAndOpacity_Lambda([this, &Style]() -> FSlateColor
                                                                      {
                                                  if (ManageButton.IsValid() && ManageButton->IsHovered())
                                                  {
                                                      return Style.GetColor("Convai.Color.component.accountMenu.manageAccountTextHover");
                                                  }
                                                  return Style.GetColor("Convai.Color.component.accountMenu.manageAccountText"); })]]]];
}

TSharedRef<SWidget> SAccountMenu::BuildSignOutItem()
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    using namespace ConvaiEditor::Constants::Layout::Components::AccountMenu;

    return SAssignNew(SignOutButton, SButton)
        .ButtonStyle(CONVAI_EDITOR_STYLE::Get(), "NoBorder")
        .OnClicked(this, &SAccountMenu::HandleSignOutClicked)
            [SNew(SBorder)
                 .BorderImage_Lambda([this, &Style]() -> const FSlateBrush *
                                     {
                    static TSharedPtr<FSlateColorBrush> HoverBrush;
                    
                    if (SignOutButton.IsValid() && SignOutButton->IsHovered())
                    {
                        if (!HoverBrush.IsValid())
                        {
                            HoverBrush = MakeShared<FSlateColorBrush>(
                                Style.GetColor("Convai.Color.component.accountMenu.itemBgHover")
                            );
                        }
                        return HoverBrush.Get();
                    }

                    return FStyleDefaults::GetNoBrush(); })
                 .Padding(FMargin(ItemPaddingHorizontal, ItemPaddingVertical))
                     [SNew(SHorizontalBox)

                      + SHorizontalBox::Slot()
                            .AutoWidth()
                            .VAlign(VAlign_Center)
                            .Padding(0.0f, 0.0f, IconTextSpacing, 0.0f)
                                [SNew(SImage)
                                     .Image(Style.GetBrush("Convai.Icon.SignOut"))
                                     .DesiredSizeOverride(IconSize)
                                     .ColorAndOpacity_Lambda([this, &Style]() -> FSlateColor
                                                             {
                            if (SignOutButton.IsValid() && SignOutButton->IsHovered())
                            {
                                return Style.GetColor("Convai.Color.component.accountMenu.signOutTextHover");
                            }
                            return Style.GetColor("Convai.Color.component.accountMenu.textPrimary"); })]

                      + SHorizontalBox::Slot()
                            .AutoWidth()
                            .VAlign(VAlign_Center)
                                [SNew(STextBlock)
                                     .Text(FText::FromString("Sign out"))
                                     .Font(FCoreStyle::GetDefaultFontStyle("Regular", ItemTextFontSize))
                                     .ColorAndOpacity_Lambda([this, &Style]() -> FSlateColor
                                                             {
                            if (SignOutButton.IsValid() && SignOutButton->IsHovered())
                            {
                                return Style.GetColor("Convai.Color.component.accountMenu.signOutTextHover");
                            }
                            return Style.GetColor("Convai.Color.component.accountMenu.textPrimary"); })]]];
}

TSharedRef<SWidget> SAccountMenu::BuildDivider()
{
    const ISlateStyle &Style = FConvaiStyle::Get();
    using namespace ConvaiEditor::Constants::Layout::Components::AccountMenu;

    return SNew(SBox)
        .HeightOverride(DividerThickness)
        .Padding(DividerMargin)
            [SNew(SImage)
                 .Image_Lambda([&Style]() -> const FSlateBrush *
                               {
                static TSharedPtr<FSlateColorBrush> DividerBrush;
                if (!DividerBrush.IsValid())
                {
                    DividerBrush = MakeShared<FSlateColorBrush>(
                        Style.GetColor("Convai.Color.component.accountMenu.divider")
                    );
                }
                return DividerBrush.Get(); })];
}

FReply SAccountMenu::HandleManageAccountClicked()
{
    if (OnManageAccountClicked.IsBound())
    {
        OnManageAccountClicked.Execute();
    }
    return FReply::Handled();
}

FReply SAccountMenu::HandleSignOutClicked()
{
    if (OnSignOutClicked.IsBound())
    {
        OnSignOutClicked.Execute();
    }
    return FReply::Handled();
}
