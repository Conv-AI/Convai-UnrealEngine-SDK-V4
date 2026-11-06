/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiValidationUtils.h
 *
 * Utility functions for service validation and resolution.
 */

#pragma once

#include "CoreMinimal.h"
#include "Logging/ConvaiEditorValidationLog.h"
#include "Services/ConvaiDIContainer.h"
#include "Misc/Optional.h"
#include "ConvaiEditor.h"

/**
 * Utility struct providing static methods for validation checks.
 */
struct CONVAIEDITOR_API FConvaiValidationUtils
{
public:
	/**
	 * Resolve a service from DI container
	 *
	 * @tparam TServiceType The service type to resolve
	 * @param Context Context string for error logging
	 * @return Optional service pointer, empty if resolution failed
	 */
	template <typename TServiceType>
	static TOptional<TSharedPtr<TServiceType>> ResolveService(const FString &Context)
	{
		static_assert(std::is_base_of_v<IConvaiService, TServiceType>,
					  "TServiceType must derive from IConvaiService");

		auto Result = FConvaiDIContainerManager::Get().Resolve<TServiceType>();
		if (Result.IsSuccess())
		{
			TSharedPtr<TServiceType> Service = Result.GetValue();
			if (Service.IsValid())
			{
				return TOptional<TSharedPtr<TServiceType>>(Service);
			}
			else
			{
				UE_LOG(LogConvaiEditor, Error, TEXT("%s: Service resolved but is invalid"), *Context);
				return TOptional<TSharedPtr<TServiceType>>();
			}
		}
		else
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("%s: Failed to resolve service: %s"),
				   *Context, *Result.GetError());
			return TOptional<TSharedPtr<TServiceType>>();
		}
	}

	/**
	 * Resolve a service from DI container with callbacks
	 *
	 * @tparam TServiceType The service type to resolve
	 * @param Context Context string for error logging
	 * @param OnSuccess Callback when service is resolved
	 * @param OnFailure Callback when resolution fails
	 */
	template <typename TServiceType>
	static void ResolveServiceWithCallbacks(
		const FString &Context,
		TFunction<void(TSharedPtr<TServiceType>)> OnSuccess,
		TFunction<void(const FString &)> OnFailure)
	{
		auto ServiceOpt = ResolveService<TServiceType>(Context);
		if (ServiceOpt.IsSet())
		{
			OnSuccess(ServiceOpt.GetValue());
		}
		else
		{
			OnFailure(FString::Printf(TEXT("Failed to resolve service in context: %s"), *Context));
		}
	}

	/**
	 * Validate that a service is initialized
	 *
	 * @param Service The service to validate
	 * @param Context Context string for error logging
	 * @return True if service is valid and initialized
	 */
	template <typename TServiceType>
	static bool ValidateService(const TSharedPtr<TServiceType> &Service, const FString &Context)
	{
		if (!Service.IsValid())
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("%s: Service is null"), *Context);
			return false;
		}

		if (!Service->IsInitialized())
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("%s: Service is not initialized"), *Context);
			return false;
		}

		return true;
	}

	/** Get DI container statistics */
	static IConvaiDIContainer::FContainerStats GetContainerStats()
	{
		return FConvaiDIContainerManager::Get().GetStats();
	}

	/**
	 * Checks if a raw pointer is not null
	 *
	 * @param Ptr The pointer to check
	 * @param Context Context for logging
	 * @return True if the pointer is not null
	 */
	template <typename T>
	static bool IsNotNull(const T *Ptr, const FString &Context)
	{
		if (Ptr == nullptr)
		{
			UE_LOG(LogConvaiEditorValidation, Error, TEXT("Null pointer detected. Context: %s"), *Context);
			return false;
		}
		return true;
	}

	/**
	 * Checks if a TSharedPtr is valid
	 *
	 * @param Ptr The TSharedPtr to check
	 * @param Context Context for logging
	 * @return True if the pointer is valid
	 */
	template <typename T>
	static bool IsValidPtr(const TSharedPtr<T> &Ptr, const FString &Context)
	{
		if (!Ptr.IsValid())
		{
			UE_LOG(LogConvaiEditorValidation, Error, TEXT("Invalid TSharedPtr detected. Context: %s"), *Context);
			return false;
		}
		return true;
	}

	/** Checks if a string is not empty */
	static bool IsValidString(const FString &Str, const FString &Context);

	/** Generic assertion check that logs error if condition is false */
	static bool Check(bool bCondition, const FString &ErrorMessage);

	/**
	 * Validates that a JSON object has a specific object field
	 *
	 * @param JsonObject The JSON object to check
	 * @param FieldName The name of the field
	 * @param OutObject Where the found object will be stored
	 * @param Context Context for logging
	 * @return True if field exists and is valid
	 */
	static bool GetJsonObjectField(const TSharedPtr<FJsonObject> &JsonObject, const FString &FieldName, const TSharedPtr<FJsonObject> **OutObject, const FString &Context);

	/**
	 * Validates that a JSON object has a non-empty string field
	 *
	 * @param JsonObject The JSON object to check
	 * @param FieldName The name of the field
	 * @param OutString Where the found value will be stored
	 * @param Context Context for logging
	 * @return True if field exists and is non-empty
	 */
	static bool GetJsonStringField(const TSharedPtr<FJsonObject> &JsonObject, const FString &FieldName, FString &OutString, const FString &Context);

	/**
	 * Validates that an integer is within a range
	 *
	 * @param Value The integer to check
	 * @param Min Minimum allowed value
	 * @param Max Maximum allowed value
	 * @param Context Context for logging
	 * @return True if value is within range
	 */
	static bool IsIntInRange(int32 Value, int32 Min, int32 Max, const FString &Context);

	/**
	 * Validates that a float is within a range
	 *
	 * @param Value The float to check
	 * @param Min Minimum allowed value
	 * @param Max Maximum allowed value
	 * @param Context Context for logging
	 * @return True if value is within range
	 */
	static bool IsFloatInRange(float Value, float Min, float Max, const FString &Context);

	/**
	 * Loads a material interface with fallback methods
	 *
	 * @param MaterialPath The path to the material asset
	 * @param Context Context for logging
	 * @return Loaded material interface or nullptr
	 */
	static UMaterialInterface *LoadMaterialInterface(const FString &MaterialPath, const FString &Context);
};
