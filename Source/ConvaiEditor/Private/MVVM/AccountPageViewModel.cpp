/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AccountPageViewModel.cpp
 *
 * Implementation of the account page view model.
 */

#include "MVVM/AccountPageViewModel.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/IConvaiAccountService.h"
#include "Utility/ConvaiValidationUtils.h"

void FAccountPageViewModel::Initialize()
{
	FViewModelBase::Initialize();
}

void FAccountPageViewModel::Shutdown()
{
	FViewModelBase::Shutdown();
}

void FAccountPageViewModel::LoadAccountUsage(const FString &ApiKey)
{
	StartLoading(FText::FromString(TEXT("Loading account data...")));

	TWeakPtr<FAccountPageViewModel> WeakViewModel = SharedThis(this);

	FConvaiValidationUtils::ResolveServiceWithCallbacks<IConvaiAccountService>(
		TEXT("FAccountPageViewModel::LoadAccountUsage"),
		[WeakViewModel, ApiKey](TSharedPtr<IConvaiAccountService> Service)
		{
			Service->GetAccountUsage(ApiKey, FOnAccountUsageReceived::CreateLambda([WeakViewModel](const FConvaiAccountUsage &Result, const FString &Error)
																				   {
																					   TSharedPtr<FAccountPageViewModel> ViewModel = WeakViewModel.Pin();
																					   if (!ViewModel.IsValid())
																					   {
																						   return;
																					   }

																					   ViewModel->StopLoading();

																					   if (!Error.IsEmpty())
																					   {
																						   UE_LOG(LogConvaiEditor, Warning, TEXT("Failed to load account usage - %s"), *Error);
																						   return;
																					   }
																					   ViewModel->Usage = Result;
																					   ViewModel->UsageChangedDelegate.Broadcast();
																					   ViewModel->BroadcastInvalidated(); }));
		},
		[WeakViewModel](const FString &Error)
		{
			TSharedPtr<FAccountPageViewModel> ViewModel = WeakViewModel.Pin();
			if (ViewModel.IsValid())
			{
				ViewModel->StopLoading();
			}
			UE_LOG(LogConvaiEditor, Error, TEXT("ConvaiAccountService resolution failed - %s"), *Error);
		});
}

FDateTime FAccountPageViewModel::GetPlanExpiryDate() const
{
	FDateTime Expiry;
	FDateTime::ParseIso8601(*Usage.RenewDate, Expiry);
	return Expiry;
}

FDateTime FAccountPageViewModel::GetNextQuotaRenewalDate() const
{
	FDateTime Now = FDateTime::UtcNow().GetDate();
	FDateTime Expiry;
	FDateTime::ParseIso8601(*Usage.RenewDate, Expiry);
	int32 RenewalDay = Expiry.GetDay();
	int32 Year = Now.GetYear();
	int32 Month = Now.GetMonth();
	int32 Day = RenewalDay;
	if (Now.GetDay() >= RenewalDay)
	{
		Month++;
		if (Month > 12)
		{
			Month = 1;
			Year++;
		}
	}
	int32 DaysInMonth = FDateTime::DaysInMonth(Year, Month);
	if (Day > DaysInMonth)
		Day = DaysInMonth;
	return FDateTime(Year, Month, Day);
}

int32 FAccountPageViewModel::GetDaysUntilPlanExpiry() const
{
	FDateTime Expiry = GetPlanExpiryDate();
	FDateTime Now = FDateTime::UtcNow().GetDate();
	return (Expiry - Now).GetDays();
}

int32 FAccountPageViewModel::GetDaysUntilQuotaRenewal() const
{
	FDateTime Renewal = GetNextQuotaRenewalDate();
	FDateTime Now = FDateTime::UtcNow().GetDate();
	return (Renewal - Now).GetDays();
}

FText FAccountPageViewModel::GetPlanExpiryText() const
{
	FDateTime Expiry = GetPlanExpiryDate();
	int32 DaysLeft = GetDaysUntilPlanExpiry();
	FString DateStr = Expiry.ToString(TEXT("%Y-%m-%d"));
	FString Suffix;
	if (DaysLeft == 0)
		Suffix = TEXT("(today)");
	else if (DaysLeft == 1)
		Suffix = TEXT("(tomorrow)");
	else if (DaysLeft > 1)
		Suffix = FString::Printf(TEXT("(in %d days)"), DaysLeft);
	else
		Suffix = TEXT("(expired)");
	return FText::FromString(FString::Printf(TEXT("%s %s"), *DateStr, *Suffix));
}

FText FAccountPageViewModel::GetQuotaRenewalText() const
{
	FDateTime Renewal = GetNextQuotaRenewalDate();
	int32 DaysLeft = GetDaysUntilQuotaRenewal();
	FString DateStr = Renewal.ToString(TEXT("%Y-%m-%d"));
	FString Suffix;
	if (DaysLeft == 0)
		Suffix = TEXT("(today)");
	else if (DaysLeft == 1)
		Suffix = TEXT("(tomorrow)");
	else if (DaysLeft > 1)
		Suffix = FString::Printf(TEXT("(in %d days)"), DaysLeft);
	else
		Suffix = TEXT("");
	return FText::FromString(FString::Printf(TEXT("%s %s"), *DateStr, *Suffix));
}
