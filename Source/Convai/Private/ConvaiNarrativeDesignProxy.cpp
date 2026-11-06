// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiNarrativeDesignProxy.h"
#include "ConvaiUtils.h"
#include "../Convai.h"
#include "Engine.h"
#include "RestAPI/ConvaiURL.h"

DEFINE_LOG_CATEGORY(ConvaiNarrativeHTTP);

namespace ConvaiNarrativeDesignPrivate
{
	FString CharacterIdBody(const FString& CharacterId)
	{
		FString Body;
		const TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&Body);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("character_id"), CharacterId);
		Writer->WriteObjectEnd();
		Writer->Close();
		return Body;
	}
}

UFetchNarrativeSectionsProxy* UFetchNarrativeSectionsProxy::FetchNarrativeSections(UObject* WorldContextObject, FString InCharacterId)
{
	UFetchNarrativeSectionsProxy* Proxy = NewObject<UFetchNarrativeSectionsProxy>();
	Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	Proxy->CharacterId = InCharacterId;
	return Proxy;
}

void UFetchNarrativeSectionsProxy::Activate()
{

	UWorld* World = WorldPtr.Get();

	if (!World)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Could not get a pointer to world!"));
		failed();
		return;
	}

	FHttpModule* Http = &FHttpModule::Get();
	if (!Http)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Could not get a pointer to http module!"));
		failed();
		return;
	}

	TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	FString AuthKey = AuthHeaderAndKey.Value;
	FString AuthHeader = AuthHeaderAndKey.Key;

	// Form Validation
	if (
		!UConvaiFormValidation::ValidateAuthKey(AuthKey)
		|| !UConvaiFormValidation::ValidateCharacterID(CharacterId)
		)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("UFetchNarrativeSectionsProxy::Activate Invalid Character or API key"));
		failed();
		return;
	}

	FHttpModule* HttpModule = &FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = HttpModule->CreateRequest();
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UFetchNarrativeSectionsProxy::OnHttpRequestCompleted);
	HttpRequest->SetURL(UConvaiURL::GetFullURL(TEXT("character/narrative/list-sections"), false));
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(AuthHeader, AuthKey);
	HttpRequest->SetContentAsString(ConvaiNarrativeDesignPrivate::CharacterIdBody(CharacterId));
	HttpRequest->ProcessRequest();
}

void UFetchNarrativeSectionsProxy::OnHttpRequestCompleted(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	NarrativeSections.Empty();
	if (!Response)
	{
		if (bWasSuccessful)
		{
			CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("HTTP request succeded - But response pointer is invalid"));
		}
		else
		{
			CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("HTTP request failed - Response pointer is invalid"));
		}

		failed();
		return;
	}

	if (!bWasSuccessful || Response->GetResponseCode() < 200 || Response->GetResponseCode() > 299)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("HTTP request failed with code %d"), Response->GetResponseCode());
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Response:%s"), *Response->GetContentAsString());

		failed();
		return;
	}

	const FString Body = Response->GetContentAsString();
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);

	if (FJsonSerializer::Deserialize(Reader, JsonArray)) // Deserialize into a JSON array
	{
		int32 SkippedEntries = 0;
		for (const TSharedPtr<FJsonValue>& Val : JsonArray)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Val->TryGetObject(Object))
			{
				++SkippedEntries;
				continue;
			}

			FNarrativeSection Section;
			if (FJsonObjectConverter::JsonObjectToUStruct(Object->ToSharedRef(), FNarrativeSection::StaticStruct(), &Section))
			{
				NarrativeSections.Add(Section);
			}
		}
		if (SkippedEntries > 0)
		{
			CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Skipped %d section entries that are not JSON objects"), SkippedEntries);
		}
		success();
		return;
	}

	CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Expected a JSON array of sections, Response:%s"), *Body);
	failed();
}

void UFetchNarrativeSectionsProxy::failed()
{
	//auto InvocationList = OnFailure.GetAllObjects();
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnFailure delegate is bound to %d functions before invokation."), InvocationList.Num());

	OnFailure.Broadcast(NarrativeSections);
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnFailure delegate is bound to %d functions after invokation."), InvocationList.Num());
	finish();
}

void UFetchNarrativeSectionsProxy::success()
{
	//auto InvocationList = OnSuccess.GetAllObjects();
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnSuccess delegate is bound to %d functions before invokation."), InvocationList.Num());

	OnSuccess.Broadcast(NarrativeSections);
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnSuccess delegate is bound to %d functions after invokation."), InvocationList.Num());
	finish();
}

void UFetchNarrativeSectionsProxy::finish()
{
}


UFetchNarrativeTriggersProxy* UFetchNarrativeTriggersProxy::FetchNarrativeTriggers(UObject* WorldContextObject, FString InCharacterId)
{
	UFetchNarrativeTriggersProxy* Proxy = NewObject<UFetchNarrativeTriggersProxy>();
	Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	Proxy->CharacterId = InCharacterId;
	return Proxy;
}

void UFetchNarrativeTriggersProxy::Activate()
{

	UWorld* World = WorldPtr.Get();

	if (!World)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Could not get a pointer to world!"));
		failed();
		return;
	}

	FHttpModule* Http = &FHttpModule::Get();
	if (!Http)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Could not get a pointer to http module!"));
		failed();
		return;
	}

	TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	FString AuthKey = AuthHeaderAndKey.Value;
	FString AuthHeader = AuthHeaderAndKey.Key;

	// Form Validation
	if (
		!UConvaiFormValidation::ValidateAuthKey(AuthKey)
		|| !UConvaiFormValidation::ValidateCharacterID(CharacterId)
		)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("UFetchNarrativeTriggersProxy::Activate Invalid Character or API key"));
		failed();
		return;
	}
	FHttpModule* HttpModule = &FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = HttpModule->CreateRequest();
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UFetchNarrativeTriggersProxy::OnHttpRequestCompleted);
	HttpRequest->SetURL(UConvaiURL::GetFullURL(TEXT("character/narrative/list-triggers"), false));
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(AuthHeader, AuthKey);
	HttpRequest->SetContentAsString(ConvaiNarrativeDesignPrivate::CharacterIdBody(CharacterId));
	HttpRequest->ProcessRequest();
}

void UFetchNarrativeTriggersProxy::OnHttpRequestCompleted(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	NarrativeTriggers.Empty();
	if (!Response)
	{
		if (bWasSuccessful)
		{
			CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("HTTP request succeded - But response pointer is invalid"));
		}
		else
		{
			CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("HTTP request failed - Response pointer is invalid"));
		}

		failed();
		return;
	}

	if (!bWasSuccessful || Response->GetResponseCode() < 200 || Response->GetResponseCode() > 299)
	{
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("HTTP request failed with code %d"), Response->GetResponseCode());
		CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Response:%s"), *Response->GetContentAsString());

		failed();
		return;
	}

	const FString Body = Response->GetContentAsString();
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);

	if (FJsonSerializer::Deserialize(Reader, JsonArray)) // Deserialize into a JSON array
	{
		int32 SkippedEntries = 0;
		for (const TSharedPtr<FJsonValue>& Val : JsonArray)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Val->TryGetObject(Object))
			{
				++SkippedEntries;
				continue;
			}

			FNarrativeTrigger Trigger;
			if (FJsonObjectConverter::JsonObjectToUStruct(Object->ToSharedRef(), FNarrativeTrigger::StaticStruct(), &Trigger))
			{
				NarrativeTriggers.Add(Trigger);
			}
		}
		if (SkippedEntries > 0)
		{
			CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Skipped %d trigger entries that are not JSON objects"), SkippedEntries);
		}
		success();
		return;
	}

	CONVAI_LOG(ConvaiNarrativeHTTP, Warning, TEXT("Expected a JSON array of triggers, Response:%s"), *Body);
	failed();
}

void UFetchNarrativeTriggersProxy::failed()
{
	//auto InvocationList = OnFailure.GetAllObjects();
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnFailure delegate is bound to %d functions before invokation."), InvocationList.Num());

	OnFailure.Broadcast(NarrativeTriggers);
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnFailure delegate is bound to %d functions after invokation."), InvocationList.Num());
	finish();
}

void UFetchNarrativeTriggersProxy::success()
{
	//auto InvocationList = OnSuccess.GetAllObjects();
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnSuccess delegate is bound to %d functions before invokation."), InvocationList.Num());

	OnSuccess.Broadcast(NarrativeTriggers);
	//CONVAI_LOG(ConvaiNarrativeHTTP, Log, TEXT("OnSuccess delegate is bound to %d functions after invokation."), InvocationList.Num());
	finish();
}

void UFetchNarrativeTriggersProxy::finish()
{
}

