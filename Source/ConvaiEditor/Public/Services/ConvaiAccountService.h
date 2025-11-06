/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiAccountService.h
 *
 * Implementation of Convai account usage service.
 */

#pragma once
#include "CoreMinimal.h"
#include "Models/ConvaiAccountUsage.h"
#include "Services/IConvaiAccountService.h"
#include "Utility/CircuitBreaker.h"
#include "Utility/RetryPolicy.h"

DECLARE_DELEGATE_TwoParams(FOnAccountUsageReceived, const FConvaiAccountUsage & /*Usage*/, const FString & /*Error*/);

/**
 * Fetches account usage information from Convai API.
 */
class CONVAIEDITOR_API FConvaiAccountService : public IConvaiAccountService, public TSharedFromThis<FConvaiAccountService>
{
public:
	FConvaiAccountService();
	virtual ~FConvaiAccountService() = default;

	virtual void Startup() override;
	virtual void Shutdown() override;

	virtual void GetAccountUsage(const FString &ApiKey, FOnAccountUsageReceived Callback) override;

	static FName StaticType() { return TEXT("IConvaiAccountService"); }

private:
	bool bIsInitialized;
	TSharedPtr<ConvaiEditor::FCircuitBreaker> CircuitBreaker;
	TSharedPtr<ConvaiEditor::FRetryPolicy> RetryPolicy;
};
