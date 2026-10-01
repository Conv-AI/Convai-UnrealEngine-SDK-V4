/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AccountPageViewModel.h
 *
 * ViewModel for AccountPage.
 */

#pragma once
#include "CoreMinimal.h"
#include "Models/ConvaiAccountUsage.h"
#include "MVVM/ViewModel.h"

DECLARE_MULTICAST_DELEGATE(FOnAccountUsageChanged);

/** ViewModel for AccountPage. */
class CONVAIEDITOR_API FAccountPageViewModel : public FViewModelBase
{
public:
	FAccountPageViewModel() = default;
	virtual ~FAccountPageViewModel() = default;

	virtual void Initialize() override;
	virtual void Shutdown() override;

	static FName StaticType() { return TEXT("FAccountPageViewModel"); }

	void LoadAccountUsage(const FString &ApiKey);

	const FConvaiAccountUsage &GetUsage() const { return Usage; }

	FOnAccountUsageChanged &OnUsageChanged() { return UsageChangedDelegate; }

	FDateTime GetPlanExpiryDate() const;
	FDateTime GetNextQuotaRenewalDate() const;
	int32 GetDaysUntilPlanExpiry() const;
	int32 GetDaysUntilQuotaRenewal() const;
	FText GetPlanExpiryText() const;
	FText GetQuotaRenewalText() const;

private:
	FConvaiAccountUsage Usage;
	FOnAccountUsageChanged UsageChangedDelegate;
};
