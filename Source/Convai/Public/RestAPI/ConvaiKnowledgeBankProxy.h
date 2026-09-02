// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RestAPI/ConvaiAPIBase.h"
#include "ConvaiDefinitions.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ConvaiKnowledgeBankProxy.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(KnowledgeBankHttpLogs, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKnowledgeBankDocumentCallbackSignature, const FConvaiKnowledgeBankDocument&, Document);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKnowledgeBankDocumentListCallbackSignature, const TArray<FConvaiKnowledgeBankDocument>&, Documents);




// Upload knowledge bank document
UCLASS()
class CONVAI_API UConvaiUploadKnowledgeBankDocument : public UConvaiAPIBaseProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKnowledgeBankDocumentCallbackSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FKnowledgeBankDocumentCallbackSignature OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Convai Upload Knowledge Bank Document"), Category = "Convai|Knowledge Bank")
	static UConvaiUploadKnowledgeBankDocument* ConvaiUploadKnowledgeBankDocumentProxy(FString FilePath);

protected:
	virtual bool ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb) override;
	virtual bool AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary) override;
	virtual bool AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend) override { return false; }
	virtual void HandleSuccess() override;
	virtual void HandleFailure() override;

	FString AssociatedFilePath;
	TArray<uint8> AssociatedFileBytes;
	bool bFailureBroadcast = false;
};
// END upload knowledge bank document




// Update knowledge bank document
UCLASS()
class CONVAI_API UConvaiUpdateKnowledgeBankDocument : public UConvaiAPIBaseProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKnowledgeBankDocumentCallbackSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FKnowledgeBankDocumentCallbackSignature OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Convai Update Knowledge Bank Document"), Category = "Convai|Knowledge Bank")
	static UConvaiUpdateKnowledgeBankDocument* ConvaiUpdateKnowledgeBankDocumentProxy(FString DocumentID, FString FilePath);

protected:
	virtual bool ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb) override;
	virtual bool AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary) override;
	virtual bool AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend) override { return false; }
	virtual void HandleSuccess() override;
	virtual void HandleFailure() override;

	FString AssociatedDocumentID;
	FString AssociatedFilePath;
	TArray<uint8> AssociatedFileBytes;
	bool bFailureBroadcast = false;
};
// END update knowledge bank document




// List knowledge bank documents
UCLASS()
class CONVAI_API UConvaiListKnowledgeBankDocuments : public UConvaiAPIBaseProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKnowledgeBankDocumentListCallbackSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FKnowledgeBankDocumentListCallbackSignature OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Convai List Knowledge Bank Documents"), Category = "Convai|Knowledge Bank")
	static UConvaiListKnowledgeBankDocuments* ConvaiListKnowledgeBankDocumentsProxy(FString CharacterID);

protected:
	virtual bool ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb) override;
	virtual bool AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary) override;
	virtual bool AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend) override { return false; }
	virtual void HandleSuccess() override;
	virtual void HandleFailure() override;

	FString AssociatedCharacterID;
	bool bFailureBroadcast = false;
};
// END list knowledge bank documents




// Delete knowledge bank document
UCLASS()
class CONVAI_API UConvaiDeleteKnowledgeBankDocument : public UConvaiAPIBaseProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FStringHttpResponseCallbackSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FStringHttpResponseCallbackSignature OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Convai Delete Knowledge Bank Document"), Category = "Convai|Knowledge Bank")
	static UConvaiDeleteKnowledgeBankDocument* ConvaiDeleteKnowledgeBankDocumentProxy(FString DocumentID);

protected:
	virtual bool ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb) override;
	virtual bool AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary) override;
	virtual bool AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend) override { return false; }
	virtual void HandleSuccess() override;
	virtual void HandleFailure() override;

	FString AssociatedDocumentID;
	bool bFailureBroadcast = false;
};
// END delete knowledge bank document




// Connect / disconnect knowledge bank documents to a character
UCLASS()
class CONVAI_API UConvaiSetKnowledgeBankDocumentStatus : public UConvaiAPIBaseProxy
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FStringHttpResponseCallbackSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FStringHttpResponseCallbackSignature OnFailure;

	/**
	 * Sets bConnected for each supplied document on the character.
	 *
	 * Pass the full list returned by Convai List Knowledge Bank Documents with only
	 * the entries you meant to change flipped. It is not established whether the API
	 * merges the docs list into the character's existing set or replaces it outright;
	 * sending the full list is correct either way. Observing a character that keeps an
	 * untouched document after a single-entry call would settle it.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DisplayName = "Convai Set Knowledge Bank Document Status"), Category = "Convai|Knowledge Bank")
	static UConvaiSetKnowledgeBankDocumentStatus* ConvaiSetKnowledgeBankDocumentStatusProxy(FString CharacterID, const TArray<FConvaiKnowledgeBankDocument>& Documents);

protected:
	virtual bool ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb) override;
	virtual bool AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary) override { return false; }
	virtual bool AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend) override;
	virtual void HandleSuccess() override;
	virtual void HandleFailure() override;

	FString AssociatedCharacterID;
	TArray<FConvaiKnowledgeBankDocument> AssociatedDocuments;
	bool bFailureBroadcast = false;
};
// END connect / disconnect knowledge bank documents




UCLASS()
class CONVAI_API UConvaiKnowledgeBankUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Reads one document object. Returns false when it carries no "id", which is how an error body shows up. */
	static bool ReadKnowledgeBankDocument(const TSharedRef<FJsonObject>& Object, FConvaiKnowledgeBankDocument& OutDocument);

	/** Parses an upload / update response body. */
	static bool ParseKnowledgeBankDocument(const FString& JsonString, FConvaiKnowledgeBankDocument& OutDocument);

	/** Parses a list response body: { "docs": [ ... ] }. */
	static bool ParseKnowledgeBankDocumentArray(const FString& JsonString, TArray<FConvaiKnowledgeBankDocument>& OutDocuments);
};
