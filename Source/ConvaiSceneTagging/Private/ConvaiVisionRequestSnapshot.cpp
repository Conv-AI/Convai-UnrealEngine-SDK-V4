// Copyright Convai. All Rights Reserved.

#include "ConvaiVisionRequestSnapshot.h"

#include "SceneAutoTaggerNativeCoreAdapter.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiVisionSnapshot, Log, All);

namespace
{
	/** Keeps folders of same-second parallel requests apart. */
	FThreadSafeCounter GSnapshotSequence;

	FString CreateSnapshotFolder(const TCHAR* PathKind)
	{
		const FString Folder = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("ConvaiSceneAutoTagger"),
			TEXT("Requests"),
			FString::Printf(
				TEXT("%s_%s_%03d"),
				*FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
				PathKind,
				GSnapshotSequence.Increment()));
		if (!IFileManager::Get().MakeDirectory(*Folder, true))
		{
			UE_LOG(
				LogConvaiVisionSnapshot,
				Warning,
				TEXT("The request snapshot folder '%s' could not be created."),
				*Folder);
			return FString();
		}
		return Folder;
	}

	void WriteSnapshotJson(const FString& Folder, const TCHAR* FileName, const TSharedRef<FJsonObject>& Json)
	{
		FString Contents;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Contents);
		FJsonSerializer::Serialize(Json, Writer);
		const FString FilePath = FPaths::Combine(Folder, FileName);
		if (!FFileHelper::SaveStringToFile(Contents, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(
				LogConvaiVisionSnapshot,
				Warning,
				TEXT("The request snapshot file '%s' could not be written."),
				*FilePath);
		}
	}

	void WriteSnapshotPng(const FString& Folder, const FString& FileName, const TArray<uint8>& PngBytes)
	{
		const FString FilePath = FPaths::Combine(Folder, FPaths::MakeValidFileName(FileName));
		if (!FFileHelper::SaveArrayToFile(PngBytes, *FilePath))
		{
			UE_LOG(
				LogConvaiVisionSnapshot,
				Warning,
				TEXT("The request snapshot image '%s' could not be written."),
				*FilePath);
		}
	}

	TSharedRef<FJsonObject> MakeRequestJson(const TCHAR* PathKind, const FString& AuthHeaderName)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("path"), PathKind);
		Json->SetStringField(TEXT("savedAtUtc"), FDateTime::UtcNow().ToIso8601());
		Json->SetStringField(
			TEXT("visionProtocolId"), FSceneAutoTaggerNativeCoreAdapter::Get().GetVisionProtocolId());
		Json->SetStringField(TEXT("authHeader"), AuthHeaderName);
		return Json;
	}

	void SetPipelineParamFields(const TSharedRef<FJsonObject>& Json, const FConvaiTaggingPipelineParams& Params)
	{
		Json->SetStringField(TEXT("characterId"), Params.CharacterID);
		Json->SetStringField(TEXT("sceneDescription"), Params.SceneDescription);
		Json->SetStringField(TEXT("descriptionFocus"), Params.DescriptionFocus);
		Json->SetNumberField(TEXT("timeoutSeconds"), Params.TimeoutSeconds);
		Json->SetNumberField(TEXT("maxParallelRequests"), Params.MaxParallelRequests);
		Json->SetNumberField(TEXT("maxRetriesOn429"), Params.MaxRetriesOn429);
		Json->SetNumberField(TEXT("retryBackoffSeconds"), Params.RetryBackoffSeconds);
		Json->SetStringField(TEXT("baseUrlOverride"), Params.BaseUrlOverride);
		Json->SetStringField(TEXT("modelOverride"), Params.ModelOverride);
	}
}

namespace ConvaiVisionRequestSnapshot
{
	FString SaveAutotagRequest(
		const TCHAR* PathKind,
		const FString& AuthHeaderName,
		const FString& FileName,
		const TArray<uint8>& PngBytes,
		const TArray<FConvaiVisionService::FCellMeta>& Cells,
		const FConvaiTaggingPipelineParams& Params)
	{
		const FString Folder = CreateSnapshotFolder(PathKind);
		if (Folder.IsEmpty())
		{
			return Folder;
		}

		WriteSnapshotPng(Folder, FileName, PngBytes);

		const TSharedRef<FJsonObject> Json = MakeRequestJson(PathKind, AuthHeaderName);
		SetPipelineParamFields(Json, Params);
		Json->SetStringField(TEXT("imageFileName"), FileName);
		TArray<TSharedPtr<FJsonValue>> CellsJson;
		CellsJson.Reserve(Cells.Num());
		for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
		{
			const FConvaiVisionService::FCellMeta& Cell = Cells[CellIndex];
			const TSharedRef<FJsonObject> CellJson = MakeShared<FJsonObject>();
			CellJson->SetNumberField(TEXT("cellNumber"), CellIndex + 1);
			CellJson->SetStringField(TEXT("echoId"), Cell.EchoId);
			CellJson->SetBoolField(TEXT("contextOnly"), Cell.bContextOnly);
			CellJson->SetStringField(TEXT("previousProposalName"), Cell.PreviousProposalName);
			CellJson->SetStringField(TEXT("previousProposalDescription"), Cell.PreviousProposalDescription);
			CellJson->SetStringField(TEXT("refinementNote"), Cell.RefinementNote);
			CellsJson.Add(MakeShared<FJsonValueObject>(CellJson));
		}
		Json->SetArrayField(TEXT("cells"), CellsJson);
		WriteSnapshotJson(Folder, TEXT("request.json"), Json);
		return Folder;
	}

	FString SaveVisionRequest(const FConvaiVisionService::FVisionRequest& Request)
	{
		const FString Folder = CreateSnapshotFolder(TEXT("Vision"));
		if (Folder.IsEmpty())
		{
			return Folder;
		}

		/* Caller-supplied wire names go to indexed disk names: the transport
		 * contract accepts pairs a case-insensitive filesystem (or name
		 * sanitization, or a literal request.json) would silently collapse. */
		TArray<TSharedPtr<FJsonValue>> ImagesJson;
		ImagesJson.Reserve(Request.Images.Num());
		for (int32 ImageIndex = 0; ImageIndex < Request.Images.Num(); ++ImageIndex)
		{
			const FConvaiVisionService::FVisionImage& Image = Request.Images[ImageIndex];
			const FString DiskFileName = FString::Printf(TEXT("image_%02d.png"), ImageIndex + 1);
			WriteSnapshotPng(Folder, DiskFileName, Image.PngBytes);
			const TSharedRef<FJsonObject> ImageJson = MakeShared<FJsonObject>();
			ImageJson->SetStringField(TEXT("fileName"), Image.FileName);
			ImageJson->SetStringField(TEXT("diskFile"), DiskFileName);
			ImagesJson.Add(MakeShared<FJsonValueObject>(ImageJson));
		}

		const TSharedRef<FJsonObject> Json = MakeRequestJson(TEXT("Vision"), Request.AuthHeaderAndKey.Key);
		Json->SetStringField(TEXT("prompt"), Request.Prompt);
		Json->SetStringField(TEXT("characterId"), Request.CharacterID);
		Json->SetNumberField(TEXT("timeoutSeconds"), Request.TimeoutSeconds);
		Json->SetNumberField(TEXT("maxParallelRequests"), Request.MaxParallelRequests);
		Json->SetNumberField(TEXT("maxRetriesOn429"), Request.MaxRetriesOn429);
		Json->SetNumberField(TEXT("retryBackoffSeconds"), Request.RetryBackoffSeconds);
		Json->SetStringField(TEXT("baseUrlOverride"), Request.BaseUrlOverride);
		Json->SetStringField(TEXT("modelOverride"), Request.ModelOverride);
		Json->SetArrayField(TEXT("images"), ImagesJson);
		TArray<TSharedPtr<FJsonValue>> ExtraParamsJson;
		ExtraParamsJson.Reserve(Request.ExtraParams.Num());
		for (const TPair<FString, FString>& Extra : Request.ExtraParams)
		{
			const TSharedRef<FJsonObject> ExtraJson = MakeShared<FJsonObject>();
			ExtraJson->SetStringField(TEXT("key"), Extra.Key);
			ExtraJson->SetStringField(TEXT("value"), Extra.Value);
			ExtraParamsJson.Add(MakeShared<FJsonValueObject>(ExtraJson));
		}
		Json->SetArrayField(TEXT("extraParams"), ExtraParamsJson);
		WriteSnapshotJson(Folder, TEXT("request.json"), Json);
		return Folder;
	}

	void SaveResult(const FString& SnapshotDir, const FConvaiVisionService::FResult& Result)
	{
		if (SnapshotDir.IsEmpty())
		{
			return;
		}
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), Result.bSuccess);
		Json->SetNumberField(TEXT("httpStatus"), Result.HttpStatus);
		Json->SetStringField(TEXT("error"), Result.Error);
		TArray<TSharedPtr<FJsonValue>> ObjectsJson;
		ObjectsJson.Reserve(Result.Objects.Num());
		for (const FConvaiVisionService::FObjectResult& Object : Result.Objects)
		{
			const TSharedRef<FJsonObject> ObjectJson = MakeShared<FJsonObject>();
			ObjectJson->SetNumberField(TEXT("cellIndex"), Object.CellIndex);
			ObjectJson->SetStringField(TEXT("echoId"), Object.EchoId);
			ObjectJson->SetStringField(TEXT("name"), Object.Name);
			ObjectJson->SetStringField(TEXT("description"), Object.Description);
			ObjectJson->SetNumberField(TEXT("confidence"), Object.Confidence);
			ObjectsJson.Add(MakeShared<FJsonValueObject>(ObjectJson));
		}
		Json->SetArrayField(TEXT("objects"), ObjectsJson);
		WriteSnapshotJson(SnapshotDir, TEXT("response.json"), Json);
	}

	void SaveRawResult(const FString& SnapshotDir, const FConvaiVisionService::FRawResult& Result)
	{
		if (SnapshotDir.IsEmpty())
		{
			return;
		}
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), Result.bSuccess);
		Json->SetNumberField(TEXT("httpStatus"), Result.HttpStatus);
		Json->SetStringField(TEXT("error"), Result.Error);
		Json->SetStringField(TEXT("rawBody"), Result.RawBody);
		WriteSnapshotJson(SnapshotDir, TEXT("response.json"), Json);
	}
}
