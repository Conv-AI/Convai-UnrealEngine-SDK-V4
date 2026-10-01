/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SAccountMenu.h
 *
 * Account menu dropdown widget displaying user information and account actions.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Account menu dropdown widget displaying user information and account actions.
 */
class CONVAIEDITOR_API SAccountMenu : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAccountMenu)
        : _Username(TEXT("")), _Email(TEXT(""))
    {
    }
    /** Username to display */
    SLATE_ARGUMENT(FString, Username)

    /** Email address to display */
    SLATE_ARGUMENT(FString, Email)

    /** Callback when manage account is clicked */
    SLATE_EVENT(FSimpleDelegate, OnManageAccountClicked)

    /** Callback when sign out is clicked */
    SLATE_EVENT(FSimpleDelegate, OnSignOutClicked)
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

private:
    FString Username;
    FString Email;
    FSimpleDelegate OnManageAccountClicked;
    FSimpleDelegate OnSignOutClicked;
    TSharedPtr<SButton> ManageButton;
    TSharedPtr<SButton> SignOutButton;

    TSharedRef<SWidget> BuildUserInfoSection();
    TSharedRef<SWidget> BuildManageAccountButton();
    TSharedRef<SWidget> BuildSignOutItem();
    TSharedRef<SWidget> BuildDivider();
    FReply HandleManageAccountClicked();
    FReply HandleSignOutClicked();
};
