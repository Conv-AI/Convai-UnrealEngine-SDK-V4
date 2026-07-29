/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiValidationUtils.cpp
 *
 * Implementation of validation utility functions.
 */

#include "Utility/ConvaiValidationUtils.h"

bool FConvaiValidationUtils::IsValidString(const FString &Str, const FString &Context)
{
	if (Str.IsEmpty())
	{
		return false;
	}
	return true;
}

bool FConvaiValidationUtils::Check(bool bCondition, const FString &ErrorMessage)
{
	if (!bCondition)
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("Validation failed: %s"), *ErrorMessage);
	}
	return bCondition;
}

bool FConvaiValidationUtils::GetJsonObjectField(const TSharedPtr<FJsonObject> &JsonObject, const FString &FieldName, const TSharedPtr<FJsonObject> **OutObject, const FString &Context)
{
	if (!IsNotNull(JsonObject.Get(), FString::Printf(TEXT("Parent JsonObject in GetJsonObjectField. FieldName: %s. Context: %s"), *FieldName, *Context)))
	{
		return false;
	}

	if (!JsonObject->TryGetObjectField(FieldName, *OutObject) || !IsNotNull(*OutObject, FString::Printf(TEXT("JsonObject field is null. FieldName: %s. Context: %s"), *FieldName, *Context)))
	{
		return false;
	}

	return true;
}

bool FConvaiValidationUtils::GetJsonStringField(const TSharedPtr<FJsonObject> &JsonObject, const FString &FieldName, FString &OutString, const FString &Context)
{
	if (!IsNotNull(JsonObject.Get(), FString::Printf(TEXT("Parent JsonObject in GetJsonStringField. FieldName: %s. Context: %s"), *FieldName, *Context)))
	{
		return false;
	}

	if (!JsonObject->TryGetStringField(FieldName, OutString) || OutString.IsEmpty())
	{
		return false;
	}

	return true;
}

bool FConvaiValidationUtils::IsIntInRange(int32 Value, int32 Min, int32 Max, const FString &Context)
{
	if (Value >= Min && Value <= Max)
	{
		return true;
	}

	return false;
}

bool FConvaiValidationUtils::IsFloatInRange(float Value, float Min, float Max, const FString &Context)
{
	if (Value >= Min && Value <= Max)
	{
		return true;
	}

	return false;
}

UMaterialInterface *FConvaiValidationUtils::LoadMaterialInterface(const FString &MaterialPath, const FString &Context)
{
	if (!IsValidString(MaterialPath, FString::Printf(TEXT("MaterialPath in LoadMaterialInterface. Context: %s"), *Context)))
	{
		return nullptr;
	}

	FString FullMaterialRef = FString::Printf(TEXT("/Script/Engine.Material'%s'"), *MaterialPath);
	UMaterialInterface *Material = LoadObject<UMaterialInterface>(nullptr, *FullMaterialRef);

	if (Material != nullptr)
	{
		return Material;
	}

	FSoftObjectPath MaterialSoftPath(MaterialPath);
	Material = Cast<UMaterialInterface>(MaterialSoftPath.TryLoad());

	if (Material != nullptr)
	{
		return Material;
	}

	UE_LOG(LogConvaiEditor, Error, TEXT("Failed to load material: %s"), *MaterialPath);
	return nullptr;
}
