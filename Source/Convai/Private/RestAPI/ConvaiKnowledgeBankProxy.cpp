
#include "RestAPI/ConvaiKnowledgeBankProxy.h"
#include "RestAPI/ConvaiURL.h"
#include "ConvaiDefinitions.h"
#include "Utility/Log/ConvaiLogger.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(KnowledgeBankHttpLogs);

namespace
{
	/**
	 * Appends UTF-8 bytes. The byte count comes from the converter, not from
	 * FString::Len(): a TCHAR count truncates the payload for any non-ASCII
	 * field, and file names are exactly where non-ASCII shows up.
	 */
	void AppendUtf8(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Text)
	{
		FTCHARToUTF8 Converted(*Text);
		DataToSend.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	void AppendFormField(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary, const FString& Name, const FString& Value)
	{
		AppendUtf8(DataToSend, FString::Printf(
			TEXT("\r\n------%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s"),
			*Boundary, *Name, *Value));
	}

	void AppendFileField(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary, const FString& Name, const FString& FileName, const TArray<uint8>& FileBytes)
	{
		AppendUtf8(DataToSend, FString::Printf(
			TEXT("\r\n------%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n"),
			*Boundary, *Name, *FileName));
		DataToSend.Append(FileBytes.GetData(), FileBytes.Num());
	}

	bool LoadDocumentFile(const FString& FilePath, TArray<uint8>& OutFileBytes)
	{
		if (FilePath.IsEmpty())
		{
			CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("File path is empty"));
			return false;
		}

		if (!FFileHelper::LoadFileToArray(OutFileBytes, *FilePath))
		{
			CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Could not read file: %s"), *FilePath);
			return false;
		}

		if (OutFileBytes.Num() == 0)
		{
			CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("File is empty: %s"), *FilePath);
			return false;
		}

		return true;
	}
}

UConvaiUploadKnowledgeBankDocument* UConvaiUploadKnowledgeBankDocument::ConvaiUploadKnowledgeBankDocumentProxy(FString FilePath)
{
	UConvaiUploadKnowledgeBankDocument* Proxy = NewObject<UConvaiUploadKnowledgeBankDocument>();
	Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::KnowledgeBankUpload);
	Proxy->AssociatedFilePath = FilePath;
	return Proxy;
}

bool UConvaiUploadKnowledgeBankDocument::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
	// Validate before Super and return false without calling HandleFailure: the
	// base broadcasts the failure itself when ConfigureRequest returns false.
	// Failing from inside AddContentTo* the way the LTM proxies do would fire
	// OnFailure and still let a bodyless request go out.
	if (!LoadDocumentFile(AssociatedFilePath, AssociatedFileBytes))
	{
		return false;
	}

	return Super::ConfigureRequest(Request, ConvaiHttpConstants::POST);
}

bool UConvaiUploadKnowledgeBankDocument::AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary)
{
	const FString FileName = FPaths::GetCleanFilename(AssociatedFilePath);
	AppendFormField(DataToSend, Boundary, TEXT("file_name"), FileName);
	AppendFileField(DataToSend, Boundary, TEXT("file"), FileName, AssociatedFileBytes);
	return true;
}

void UConvaiUploadKnowledgeBankDocument::HandleSuccess()
{
	Super::HandleSuccess();

	FConvaiKnowledgeBankDocument Document;
	if (UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(ResponseString, Document))
	{
		OnSuccess.Broadcast(Document);
	}
	else
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Could not parse upload response: %s"), *ResponseString);
		HandleFailure();
	}
}

void UConvaiUploadKnowledgeBankDocument::HandleFailure()
{
	Super::HandleFailure();
	if (bFailureBroadcast)
	{
		return;
	}
	bFailureBroadcast = true;
	OnFailure.Broadcast(FConvaiKnowledgeBankDocument());
}




UConvaiUpdateKnowledgeBankDocument* UConvaiUpdateKnowledgeBankDocument::ConvaiUpdateKnowledgeBankDocumentProxy(FString DocumentID, FString FilePath)
{
	UConvaiUpdateKnowledgeBankDocument* Proxy = NewObject<UConvaiUpdateKnowledgeBankDocument>();
	Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::KnowledgeBankUpdate);
	Proxy->AssociatedDocumentID = DocumentID;
	Proxy->AssociatedFilePath = FilePath;
	return Proxy;
}

bool UConvaiUpdateKnowledgeBankDocument::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
	if (AssociatedDocumentID.IsEmpty())
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Document ID is empty"));
		return false;
	}

	if (!LoadDocumentFile(AssociatedFilePath, AssociatedFileBytes))
	{
		return false;
	}

	return Super::ConfigureRequest(Request, ConvaiHttpConstants::POST);
}

bool UConvaiUpdateKnowledgeBankDocument::AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary)
{
	AppendFormField(DataToSend, Boundary, TEXT("document_id"), AssociatedDocumentID);
	AppendFileField(DataToSend, Boundary, TEXT("file"), FPaths::GetCleanFilename(AssociatedFilePath), AssociatedFileBytes);
	return true;
}

void UConvaiUpdateKnowledgeBankDocument::HandleSuccess()
{
	Super::HandleSuccess();

	FConvaiKnowledgeBankDocument Document;
	if (UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(ResponseString, Document))
	{
		OnSuccess.Broadcast(Document);
	}
	else
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Could not parse update response: %s"), *ResponseString);
		HandleFailure();
	}
}

void UConvaiUpdateKnowledgeBankDocument::HandleFailure()
{
	Super::HandleFailure();
	if (bFailureBroadcast)
	{
		return;
	}
	bFailureBroadcast = true;
	OnFailure.Broadcast(FConvaiKnowledgeBankDocument());
}




UConvaiListKnowledgeBankDocuments* UConvaiListKnowledgeBankDocuments::ConvaiListKnowledgeBankDocumentsProxy(FString CharacterID)
{
	UConvaiListKnowledgeBankDocuments* Proxy = NewObject<UConvaiListKnowledgeBankDocuments>();
	Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::KnowledgeBankList);
	Proxy->AssociatedCharacterID = CharacterID;
	return Proxy;
}

bool UConvaiListKnowledgeBankDocuments::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
	if (AssociatedCharacterID.IsEmpty())
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Character ID is empty"));
		return false;
	}

	return Super::ConfigureRequest(Request, ConvaiHttpConstants::POST);
}

bool UConvaiListKnowledgeBankDocuments::AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary)
{
	AppendFormField(DataToSend, Boundary, TEXT("character_id"), AssociatedCharacterID);
	return true;
}

void UConvaiListKnowledgeBankDocuments::HandleSuccess()
{
	Super::HandleSuccess();

	TArray<FConvaiKnowledgeBankDocument> Documents;
	if (UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(ResponseString, Documents))
	{
		OnSuccess.Broadcast(Documents);
	}
	else
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Could not parse list response: %s"), *ResponseString);
		HandleFailure();
	}
}

void UConvaiListKnowledgeBankDocuments::HandleFailure()
{
	Super::HandleFailure();
	if (bFailureBroadcast)
	{
		return;
	}
	bFailureBroadcast = true;
	OnFailure.Broadcast(TArray<FConvaiKnowledgeBankDocument>());
}




UConvaiDeleteKnowledgeBankDocument* UConvaiDeleteKnowledgeBankDocument::ConvaiDeleteKnowledgeBankDocumentProxy(FString DocumentID)
{
	UConvaiDeleteKnowledgeBankDocument* Proxy = NewObject<UConvaiDeleteKnowledgeBankDocument>();
	Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::KnowledgeBankDelete);
	Proxy->AssociatedDocumentID = DocumentID;
	return Proxy;
}

bool UConvaiDeleteKnowledgeBankDocument::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
	if (AssociatedDocumentID.IsEmpty())
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Document ID is empty"));
		return false;
	}

	return Super::ConfigureRequest(Request, ConvaiHttpConstants::POST);
}

bool UConvaiDeleteKnowledgeBankDocument::AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary)
{
	AppendFormField(DataToSend, Boundary, TEXT("document_id"), AssociatedDocumentID);
	return true;
}

void UConvaiDeleteKnowledgeBankDocument::HandleSuccess()
{
	// Handed back verbatim rather than parsed: the docs promise
	// {"Successfully deleted document"} (not even valid JSON) while the live API
	// answers {"message": ..., "results": [{"document_id": ..., "status": ...}]}.
	// Until one of those is authoritative, callers get the raw body.
	Super::HandleSuccess();
	OnSuccess.Broadcast(ResponseString);
}

void UConvaiDeleteKnowledgeBankDocument::HandleFailure()
{
	Super::HandleFailure();
	if (bFailureBroadcast)
	{
		return;
	}
	bFailureBroadcast = true;
	OnFailure.Broadcast(ResponseString);
}




UConvaiSetKnowledgeBankDocumentStatus* UConvaiSetKnowledgeBankDocumentStatus::ConvaiSetKnowledgeBankDocumentStatusProxy(FString CharacterID, const TArray<FConvaiKnowledgeBankDocument>& Documents)
{
	UConvaiSetKnowledgeBankDocumentStatus* Proxy = NewObject<UConvaiSetKnowledgeBankDocumentStatus>();
	Proxy->URL = UConvaiURL::GetEndpoint(EConvaiEndpoint::CharacterUpdate);
	Proxy->AssociatedCharacterID = CharacterID;
	Proxy->AssociatedDocuments = Documents;
	return Proxy;
}

bool UConvaiSetKnowledgeBankDocumentStatus::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
	if (AssociatedCharacterID.IsEmpty())
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("Character ID is empty"));
		return false;
	}

	if (AssociatedDocuments.Num() == 0)
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("No documents supplied"));
		return false;
	}

	return Super::ConfigureRequest(Request, ConvaiHttpConstants::POST);
}

bool UConvaiSetKnowledgeBankDocumentStatus::AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend)
{
	ObjectToSend->SetStringField(TEXT("charID"), AssociatedCharacterID);

	TArray<TSharedPtr<FJsonValue>> Docs;
	Docs.Reserve(AssociatedDocuments.Num());
	for (const FConvaiKnowledgeBankDocument& Document : AssociatedDocuments)
	{
		if (Document.DocumentID.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"), Document.DocumentID);
		Entry->SetStringField(TEXT("status"), Document.bConnected ? TEXT("active") : TEXT("inactive"));
		Docs.Add(MakeShared<FJsonValueObject>(Entry));
	}

	if (Docs.Num() == 0)
	{
		CONVAI_LOG(KnowledgeBankHttpLogs, Error, TEXT("No documents carried an ID"));
		return false;
	}

	ObjectToSend->SetArrayField(TEXT("docs"), Docs);
	return true;
}

void UConvaiSetKnowledgeBankDocumentStatus::HandleSuccess()
{
	Super::HandleSuccess();
	OnSuccess.Broadcast(ResponseString);
}

void UConvaiSetKnowledgeBankDocumentStatus::HandleFailure()
{
	Super::HandleFailure();
	if (bFailureBroadcast)
	{
		return;
	}
	bFailureBroadcast = true;
	OnFailure.Broadcast(ResponseString);
}




bool UConvaiKnowledgeBankUtils::ReadKnowledgeBankDocument(const TSharedRef<FJsonObject>& Object, FConvaiKnowledgeBankDocument& OutDocument)
{
	Object->TryGetStringField(TEXT("id"), OutDocument.DocumentID);
	if (OutDocument.DocumentID.IsEmpty())
	{
		return false;
	}

	Object->TryGetStringField(TEXT("file_name"), OutDocument.FileName);
	Object->TryGetBoolField(TEXT("is_available"), OutDocument.bIsAvailable);
	Object->TryGetStringField(TEXT("timestamp"), OutDocument.Timestamp);

	FString Status;
	if (Object->TryGetStringField(TEXT("status"), Status))
	{
		OutDocument.bConnected = Status.Equals(TEXT("active"), ESearchCase::IgnoreCase);
	}

	// file_size arrives quoted ("72374"); TryGetNumberField reads both that and a
	// bare number, so it covers the API changing its mind.
	double FileSize = 0.0;
	if (Object->TryGetNumberField(TEXT("file_size"), FileSize))
	{
		OutDocument.FileSizeBytes = static_cast<int64>(FileSize);
	}

	return true;
}

bool UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocument(const FString& JsonString, FConvaiKnowledgeBankDocument& OutDocument)
{
	OutDocument = FConvaiKnowledgeBankDocument();

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return false;
	}

	return ReadKnowledgeBankDocument(JsonObject.ToSharedRef(), OutDocument);
}

bool UConvaiKnowledgeBankUtils::ParseKnowledgeBankDocumentArray(const FString& JsonString, TArray<FConvaiKnowledgeBankDocument>& OutDocuments)
{
	OutDocuments.Empty();

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Docs = nullptr;
	if (!JsonObject->TryGetArrayField(TEXT("docs"), Docs))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Docs)
	{
		if (!Value.IsValid())
		{
			continue;
		}

		// The documented response shows each entry as a string holding the
		// object's JSON rather than the object itself; accept either shape.
		TSharedPtr<FJsonObject> Entry = Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			FString Inner;
			if (!Value->TryGetString(Inner))
			{
				continue;
			}

			const TSharedRef<TJsonReader<>> InnerReader = TJsonReaderFactory<>::Create(Inner);
			if (!FJsonSerializer::Deserialize(InnerReader, Entry) || !Entry.IsValid())
			{
				continue;
			}
		}

		FConvaiKnowledgeBankDocument Document;
		if (ReadKnowledgeBankDocument(Entry.ToSharedRef(), Document))
		{
			OutDocuments.Add(Document);
		}
	}

	return true;
}
