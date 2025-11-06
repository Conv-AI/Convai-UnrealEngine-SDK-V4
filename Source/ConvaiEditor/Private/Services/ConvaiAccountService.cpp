/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiAccountService.cpp
 *
 * Implementation of account service for Convai API integration.
 */

#include "Services/ConvaiAccountService.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Utility/ConvaiURLs.h"
#include "ConvaiEditor.h"
#include "Services/ConvaiDIContainer.h"
#include "Async/HttpAsyncOperation.h"
#include "Async/Async.h"

FConvaiAccountService::FConvaiAccountService()
	: bIsInitialized(false)
{
}

void FConvaiAccountService::Startup()
{
	bIsInitialized = true;

	ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
	CircuitConfig.Name = TEXT("ConvaiAPI");
	CircuitConfig.FailureThreshold = 3;
	CircuitConfig.SuccessThreshold = 2;
	CircuitConfig.OpenTimeoutSeconds = 45.0f;
	CircuitConfig.bEnableLogging = true;
	CircuitBreaker = MakeShared<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

	ConvaiEditor::FRetryPolicyConfig RetryConfig;
	RetryConfig.Name = TEXT("ConvaiAPI");
	RetryConfig.MaxAttempts = 2;
	RetryConfig.BaseDelaySeconds = 1.5f;
	RetryConfig.MaxDelaySeconds = 10.0f;
	RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Exponential;
	RetryConfig.bEnableJitter = true;
	RetryConfig.bEnableLogging = true;
	RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
	RetryPolicy = MakeShared<ConvaiEditor::FRetryPolicy>(RetryConfig);
}

void FConvaiAccountService::Shutdown()
{
	bIsInitialized = false;
}

void FConvaiAccountService::GetAccountUsage(const FString &ApiKey, FOnAccountUsageReceived Callback)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("ConvaiAccountService not initialized - service startup failed"));
		FConvaiAccountUsage EmptyUsage;
		Callback.ExecuteIfBound(EmptyUsage, TEXT("Service not initialized"));
		return;
	}

	if (CircuitBreaker && CircuitBreaker->IsOpen())
	{
		UE_LOG(LogConvaiEditor, Warning, TEXT("Convai API temporarily unavailable - circuit breaker open"));
		FConvaiAccountUsage EmptyUsage;
		Callback.ExecuteIfBound(EmptyUsage, TEXT("Convai API circuit breaker is open - service temporarily unavailable"));
		return;
	}

	ConvaiEditor::FHttpAsyncRequest HttpRequest(FConvaiURLs::GetUserAPIUsageURL());
	HttpRequest.WithVerb(TEXT("POST"))
		.WithHeader(TEXT("Content-Type"), TEXT("application/json"))
		.WithHeader(TEXT("CONVAI-API-KEY"), ApiKey)
		.WithBody(TEXT("{}"))
		.WithTimeout(30.0f);

	TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> AsyncOp;

	if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
	{
		AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(
			HttpRequest,
			CircuitBreaker,
			RetryPolicy,
			nullptr);
	}
	else
	{
		AsyncOp = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, nullptr);
	}

	AsyncOp->OnComplete([Callback, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
						{
		FConvaiAccountUsage Usage;
		FString Error;

		if (!Result.IsSuccess())
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("ConvaiAccountService: HTTP request failed: %s"), *Result.GetError());
			Error = Result.GetError();
			Callback.ExecuteIfBound(Usage, Error);
			return;
		}

		const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();
		

		if (!HttpResponse.IsSuccess())
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("ConvaiAccountService: HTTP %d"), HttpResponse.ResponseCode);
			Error = FString::Printf(TEXT("HTTP Error: %d"), HttpResponse.ResponseCode);
			Callback.ExecuteIfBound(Usage, Error);
			return;
		}

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse.Body);
		
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("ConvaiAccountService: Failed to parse JSON response"));
			Error = TEXT("Failed to parse JSON response.");
			Callback.ExecuteIfBound(Usage, Error);
			return;
		}

		const TSharedPtr<FJsonObject>* UsageV2Obj = nullptr;
		if (JsonObject->TryGetObjectField(TEXT("usage_v2"), UsageV2Obj))
		{
			(*UsageV2Obj)->TryGetStringField(TEXT("plan_name"), Usage.PlanName);
			
			FString Expiry;
			if ((*UsageV2Obj)->TryGetStringField(TEXT("expiry_ts"), Expiry))
			{
				Usage.RenewDate = Expiry.Left(10);
			}

			const TArray<TSharedPtr<FJsonValue>>* MetricsArray;
			if ((*UsageV2Obj)->TryGetArrayField(TEXT("metrics"), MetricsArray))
			{
				for (const TSharedPtr<FJsonValue>& MetricVal : *MetricsArray)
				{
					const TSharedPtr<FJsonObject>* MetricObj;
					if (MetricVal->TryGetObject(MetricObj))
					{
						FString Id;
						(*MetricObj)->TryGetStringField(TEXT("id"), Id);
						
						const TArray<TSharedPtr<FJsonValue>>* UsageDetails;
						if ((*MetricObj)->TryGetArrayField(TEXT("usage_details"), UsageDetails) && UsageDetails->Num() > 0)
						{
							const TSharedPtr<FJsonObject>* DetailObj;
							if ((*UsageDetails)[0]->TryGetObject(DetailObj))
							{
								float Limit = 0.0f, Used = 0.0f;
								(*DetailObj)->TryGetNumberField(TEXT("limit"), Limit);
								(*DetailObj)->TryGetNumberField(TEXT("usage"), Used);
								
								if (Id == TEXT("interactions"))
								{
									Usage.InteractionUsageLimit = Limit;
									Usage.InteractionUsageCurrent = Used;
								}
								else if (Id == TEXT("provider_pool_1"))
								{
									Usage.ElevenlabsUsageLimit = Limit;
									Usage.ElevenlabsUsageCurrent = Used;
								}
								else if (Id == TEXT("core-api"))
								{
									Usage.CoreAPIUsageLimit = Limit;
									Usage.CoreAPIUsageCurrent = Used;
								}
								else if (Id == TEXT("pixel_streaming"))
								{
									Usage.PixelStreamingUsageLimit = Limit;
									Usage.PixelStreamingUsageCurrent = Used;
								}
							}
						}
					}
				}
			}
		}

		const TSharedPtr<FJsonObject>* UsageObj = nullptr;
		if (JsonObject->TryGetObjectField(TEXT("usage"), UsageObj))
		{
			FString UserName;
			if ((*UsageObj)->TryGetStringField(TEXT("user_name"), UserName))
			{
				Usage.UserName = UserName;
			}

			FString Email;
			if ((*UsageObj)->TryGetStringField(TEXT("email"), Email) ||
				(*UsageObj)->TryGetStringField(TEXT("user_email"), Email))
			{
				Usage.Email = Email;
			}
		}

		if (Usage.Email.IsEmpty())
		{
			JsonObject->TryGetStringField(TEXT("email"), Usage.Email);
		}

		AsyncTask(ENamedThreads::GameThread, [Callback, Usage, Error]()
		{
			Callback.ExecuteIfBound(Usage, Error);
		}); });

	AsyncOp->Start();
}
