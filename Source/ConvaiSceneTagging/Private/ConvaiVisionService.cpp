// Copyright Convai. All Rights Reserved.

#include "ConvaiVisionService.h"

#include "ConvaiSceneAutoTaggerSettings.h"
#include "ConvaiVisionRequestSnapshot.h"
#include "SceneAutoTaggerNativeCoreAdapter.h"

#include "Async/Async.h"
#include "ConvaiDefinitions.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiVisionService, Log, All);

namespace
{
	template <typename TResultType>
	void CompleteOnGameThread(
		TFunction<void(const TResultType&)>&& Completion,
		TResultType&& Result)
	{
		if (!Completion)
		{
			return;
		}
		AsyncTask(
			ENamedThreads::GameThread,
			[Completion = MoveTemp(Completion), Result = MoveTemp(Result)]()
			{
				Completion(Result);
			});
	}

	FConvaiVisionService::FResult MakeErrorResult(FString Error, const int32 HttpStatus = 0)
	{
		FConvaiVisionService::FResult Result;
		Result.HttpStatus = HttpStatus;
		Result.Error = MoveTemp(Error);
		return Result;
	}

	/** The core's reserved kv key for an SDK auth header name; NULL when the name is not a Convai auth header. */
	const char* AuthKvKeyForHeader(const FString& HeaderName)
	{
		if (HeaderName == ConvaiConstants::API_Key_Header)
		{
			return "api_key";
		}
		if (HeaderName == ConvaiConstants::Auth_Token_Header)
		{
			return "auth_token";
		}
		return nullptr;
	}

	/** UTF-8 copies of every request string; the views passed to the core stay valid until this dies. */
	struct FUtf8Request
	{
		TArray<TArray<uint8>> Strings;

		const char* Store(const FString& Value)
		{
			if (Value.IsEmpty())
			{
				return nullptr;
			}
			FTCHARToUTF8 Converted(*Value);
			TArray<uint8>& Bytes = Strings.AddDefaulted_GetRef();
			Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
			Bytes.Add(0);
			return reinterpret_cast<const char*>(Bytes.GetData());
		}
	};

	// ponytail: per-group in-flight gate polled at 50ms; a real semaphore only if a
	// caller ever runs enough parallel requests for the poll to matter. Groups keep
	// one family's limit from starving another (a limit-1 raw request must not wait
	// out a 6-wide tagging run).
	FCriticalSection GInFlightLock;
	TMap<FString, int32> GInFlightByGroup;

	void AcquireRequestSlot(const FString& Group, const int32 Limit)
	{
		for (;;)
		{
			{
				FScopeLock Lock(&GInFlightLock);
				int32& InFlight = GInFlightByGroup.FindOrAdd(Group);
				if (InFlight < Limit)
				{
					++InFlight;
					return;
				}
			}
			FPlatformProcess::Sleep(0.05f);
		}
	}

	void ReleaseRequestSlot(const FString& Group)
	{
		FScopeLock Lock(&GInFlightLock);
		int32& InFlight = GInFlightByGroup.FindChecked(Group);
		if (--InFlight == 0)
		{
			GInFlightByGroup.Remove(Group);
		}
	}

	/**
	 * One blocking DLL round trip with slot gating and internal 429 retries,
	 * shared by every vision path. The final result view — whatever its HTTP
	 * status — is handed to Consume; the return value is empty then, and the
	 * transport/core error otherwise. Never called on the game thread.
	 */
	FString RunVisionCall(
		const FString& ThrottleGroup,
		const int32 MaxParallelRequests,
		const int32 MaxRetriesOn429,
		const float RetryBackoffSeconds,
		TFunctionRef<convai_sat_status(const convai_sat_api_v4&, convai_sat_vision_result**, convai_sat_error_v1*)> Invoke,
		TFunctionRef<void(const convai_sat_vision_result_view_v4&)> Consume)
	{
		const convai_sat_api_v4* Api = FSceneAutoTaggerNativeCoreAdapter::Get().GetApi();
		if (Api == nullptr)
		{
			return FSceneAutoTaggerNativeCoreAdapter::Get().GetDiagnostic().IsEmpty()
				? FString(TEXT("The Scene Auto Tagger native core is not available."))
				: FSceneAutoTaggerNativeCoreAdapter::Get().GetDiagnostic();
		}

		const int32 MaxAttempts = 1 + FMath::Max(0, MaxRetriesOn429);
		AcquireRequestSlot(ThrottleGroup, FMath::Max(1, MaxParallelRequests));
		ON_SCOPE_EXIT { ReleaseRequestSlot(ThrottleGroup); };

		for (int32 Attempt = 1; ; ++Attempt)
		{
			convai_sat_vision_result* NativeResult = nullptr;
			convai_sat_error_v1 NativeError = {};
			NativeError.struct_size = sizeof(convai_sat_error_v1);
			const convai_sat_status CallStatus = Invoke(*Api, &NativeResult, &NativeError);
			ON_SCOPE_EXIT
			{
				if (NativeResult != nullptr)
				{
					Api->vision_result_free(NativeResult);
				}
			};

			if (CallStatus != CONVAI_SAT_STATUS_OK)
			{
				const FString CoreMessage = FString(UTF8_TO_TCHAR(NativeError.message));
				return CoreMessage.IsEmpty()
					? FString::Printf(TEXT("The vision request failed in the native core (status %d)."), CallStatus)
					: CoreMessage;
			}

			convai_sat_vision_result_view_v4 View = {};
			View.struct_size = sizeof(convai_sat_vision_result_view_v4);
			if (Api->vision_result_view(NativeResult, &View, &NativeError) != CONVAI_SAT_STATUS_OK)
			{
				const FString CoreMessage = FString(UTF8_TO_TCHAR(NativeError.message));
				return CoreMessage.IsEmpty()
					? FString(TEXT("The vision result could not be read from the native core."))
					: CoreMessage;
			}

			if (View.http_status == 429 && Attempt < MaxAttempts)
			{
				UE_LOG(
					LogConvaiVisionService,
					Display,
					TEXT("The vision endpoint is rate limited; retrying in %.0f s (attempt %d of %d)."),
					RetryBackoffSeconds,
					Attempt + 1,
					MaxAttempts);
				FPlatformProcess::Sleep(FMath::Max(0.0f, RetryBackoffSeconds));
				continue;
			}

			Consume(View);
			return FString();
		}
	}

	FString ServerErrorFromView(const convai_sat_vision_result_view_v4& View)
	{
		return View.server_message_utf8 != nullptr && View.server_message_utf8[0] != '\0'
			? FString(UTF8_TO_TCHAR(View.server_message_utf8))
			: FString::Printf(TEXT("The vision endpoint returned HTTP %d."), View.http_status);
	}

	/** One protected-prompt (autotag) round trip. Never called on the game thread. */
	FConvaiVisionService::FResult ExecuteVisionRequest(
		const convai_sat_vision_task Task,
		const TPair<FString, FString>& AuthHeaderAndKey,
		const TArray<uint8>& PngBytes,
		const FString& FileName,
		const TArray<FConvaiVisionService::FCellMeta>& Cells,
		const FConvaiTaggingPipelineParams& Params)
	{
		FUtf8Request Utf8;
		TArray<convai_sat_vision_cell_v3> NativeCells;
		NativeCells.Reserve(Cells.Num());
		for (const FConvaiVisionService::FCellMeta& Cell : Cells)
		{
			convai_sat_vision_cell_v3& NativeCell = NativeCells.AddZeroed_GetRef();
			NativeCell.struct_size = sizeof(convai_sat_vision_cell_v3);
			NativeCell.context_only = Cell.bContextOnly ? 1u : 0u;
			NativeCell.echo_id_utf8 = Utf8.Store(Cell.EchoId);
			NativeCell.previous_proposal_name_utf8 = Utf8.Store(Cell.PreviousProposalName);
			NativeCell.previous_proposal_description_utf8 = Utf8.Store(Cell.PreviousProposalDescription);
			NativeCell.refinement_note_utf8 = Utf8.Store(Cell.RefinementNote);
		}

		convai_sat_vision_image_v3 Image = {};
		Image.struct_size = sizeof(convai_sat_vision_image_v3);
		Image.cell_count = static_cast<uint32>(NativeCells.Num());
		Image.file_name_utf8 = Utf8.Store(FileName);
		Image.png_bytes = PngBytes.GetData();
		Image.png_size = static_cast<uint64>(PngBytes.Num());
		Image.cells = NativeCells.GetData();

		/* Reserved keys require non-empty values; a pair is emitted only when
		 * its value is non-empty, matching the v3 NULL-field-means-omit behavior. */
		TArray<convai_sat_kv_v4> KvParams;
		const auto AddParam = [&Utf8, &KvParams](const char* Key, const FString& Value)
		{
			if (const char* StoredValue = Utf8.Store(Value))
			{
				KvParams.Add({ Key, StoredValue });
			}
		};
		AddParam(AuthKvKeyForHeader(AuthHeaderAndKey.Key), AuthHeaderAndKey.Value);
		AddParam("character_id", Params.CharacterID);
		AddParam("base_url", Params.BaseUrlOverride);
		AddParam("model", Params.ModelOverride);

		convai_sat_vision_autotag_request_v4 Request = {};
		Request.struct_size = sizeof(convai_sat_vision_autotag_request_v4);
		Request.task = Task;
		Request.scene_description_utf8 = Utf8.Store(Params.SceneDescription);
		Request.description_focus_utf8 = Utf8.Store(Params.DescriptionFocus);
		Request.image_count = 1;
		Request.timeout_seconds = Params.TimeoutSeconds;
		Request.images = &Image;
		Request.param_count = static_cast<uint32>(KvParams.Num());
		Request.params = KvParams.GetData();

		FConvaiVisionService::FResult Result;
		const FString TransportError = RunVisionCall(
			TEXT("tagging"),
			Params.MaxParallelRequests,
			Params.MaxRetriesOn429,
			Params.RetryBackoffSeconds,
			[&Request](const convai_sat_api_v4& Api, convai_sat_vision_result** OutResult, convai_sat_error_v1* OutError)
			{
				return Api.vision_autotag(&Request, OutResult, OutError);
			},
			[&Result](const convai_sat_vision_result_view_v4& View)
			{
				Result.HttpStatus = View.http_status;
				if (View.http_status < 200 || View.http_status > 299)
				{
					Result.Error = ServerErrorFromView(View);
					return;
				}
				Result.bSuccess = true;
				for (uint32 ImageIndex = 0; ImageIndex < View.image_count; ++ImageIndex)
				{
					const convai_sat_vision_result_image_v4& ResultImage = View.images[ImageIndex];
					for (uint32 ObjectIndex = 0; ObjectIndex < ResultImage.object_count; ++ObjectIndex)
					{
						const convai_sat_vision_object_view_v3& ObjectView = ResultImage.objects[ObjectIndex];
						FConvaiVisionService::FObjectResult& Object = Result.Objects.AddDefaulted_GetRef();
						Object.CellIndex = ObjectView.cell_index;
						Object.EchoId = FString(UTF8_TO_TCHAR(ObjectView.echo_id_utf8 ? ObjectView.echo_id_utf8 : ""));
						Object.Name = FString(UTF8_TO_TCHAR(ObjectView.name_utf8 ? ObjectView.name_utf8 : ""));
						Object.Description = FString(UTF8_TO_TCHAR(ObjectView.description_utf8 ? ObjectView.description_utf8 : ""));
						Object.Confidence = ObjectView.confidence;
					}
				}
			});

		if (!TransportError.IsEmpty())
		{
			return MakeErrorResult(TransportError);
		}
		return Result;
	}

	/** One raw Vision Request round trip. Never called on the game thread. */
	FConvaiVisionService::FRawResult ExecuteRawVisionRequest(const FConvaiVisionService::FVisionRequest& Request)
	{
		FUtf8Request Utf8;
		TArray<convai_sat_vision_image_v3> NativeImages;
		NativeImages.Reserve(Request.Images.Num());
		for (const FConvaiVisionService::FVisionImage& Image : Request.Images)
		{
			convai_sat_vision_image_v3& NativeImage = NativeImages.AddZeroed_GetRef();
			NativeImage.struct_size = sizeof(convai_sat_vision_image_v3);
			NativeImage.file_name_utf8 = Utf8.Store(Image.FileName);
			NativeImage.png_bytes = Image.PngBytes.GetData();
			NativeImage.png_size = static_cast<uint64>(Image.PngBytes.Num());
		}

		TArray<convai_sat_kv_v4> KvParams;
		const auto AddParam = [&Utf8, &KvParams](const char* Key, const FString& Value)
		{
			if (const char* StoredValue = Utf8.Store(Value))
			{
				KvParams.Add({ Key, StoredValue });
			}
		};
		AddParam(AuthKvKeyForHeader(Request.AuthHeaderAndKey.Key), Request.AuthHeaderAndKey.Value);
		AddParam("character_id", Request.CharacterID);
		AddParam("base_url", Request.BaseUrlOverride);
		AddParam("model", Request.ModelOverride);
		for (const TPair<FString, FString>& Extra : Request.ExtraParams)
		{
			if (const char* StoredKey = Utf8.Store(Extra.Key))
			{
				if (const char* StoredValue = Utf8.Store(Extra.Value))
				{
					KvParams.Add({ StoredKey, StoredValue });
				}
			}
		}

		convai_sat_vision_request_v4 NativeRequest = {};
		NativeRequest.struct_size = sizeof(convai_sat_vision_request_v4);
		NativeRequest.prompt_utf8 = Utf8.Store(Request.Prompt);
		NativeRequest.image_count = static_cast<uint32>(NativeImages.Num());
		NativeRequest.images = NativeImages.GetData();
		NativeRequest.timeout_seconds = Request.TimeoutSeconds;
		NativeRequest.param_count = static_cast<uint32>(KvParams.Num());
		NativeRequest.params = KvParams.GetData();

		FConvaiVisionService::FRawResult Result;
		const FString TransportError = RunVisionCall(
			TEXT("raw"),
			Request.MaxParallelRequests,
			Request.MaxRetriesOn429,
			Request.RetryBackoffSeconds,
			[&NativeRequest](const convai_sat_api_v4& Api, convai_sat_vision_result** OutResult, convai_sat_error_v1* OutError)
			{
				return Api.vision_request(&NativeRequest, OutResult, OutError);
			},
			[&Result](const convai_sat_vision_result_view_v4& View)
			{
				Result.HttpStatus = View.http_status;
				Result.RawBody = FString(UTF8_TO_TCHAR(View.raw_body_utf8 ? View.raw_body_utf8 : ""));
				Result.bSuccess = View.http_status >= 200 && View.http_status <= 299;
				if (!Result.bSuccess)
				{
					Result.Error = ServerErrorFromView(View);
				}
			});

		if (!TransportError.IsEmpty())
		{
			Result.Error = TransportError;
		}
		return Result;
	}

	FString ValidateAuth(const TPair<FString, FString>& AuthHeaderAndKey)
	{
		if (AuthHeaderAndKey.Value.TrimStartAndEnd().IsEmpty())
		{
			return TEXT("No Convai API key or auth token is configured.");
		}
		if (AuthKvKeyForHeader(AuthHeaderAndKey.Key) == nullptr)
		{
			return FString::Printf(
				TEXT("'%s' is not a Convai auth header; expected %s or %s."),
				*AuthHeaderAndKey.Key,
				*ConvaiConstants::API_Key_Header,
				*ConvaiConstants::Auth_Token_Header);
		}
		return FString();
	}

	FString ValidateCommonInputs(const TPair<FString, FString>& AuthHeaderAndKey, const TArray<uint8>& PngBytes)
	{
		FString AuthError = ValidateAuth(AuthHeaderAndKey);
		if (!AuthError.IsEmpty())
		{
			return AuthError;
		}
		if (AuthHeaderAndKey.Key == ConvaiConstants::Auth_Token_Header)
		{
			/* The core's protected-prompt path authenticates with an API key
			 * only; failing here beats the DLL's less specific rejection. */
			return TEXT("Protected tagging and description requests require a Convai API key; an auth token only works on RequestVision.");
		}
		if (PngBytes.IsEmpty())
		{
			return TEXT("The request image is empty.");
		}
		if (static_cast<uint64>(PngBytes.Num()) > CONVAI_SAT_VISION_MAX_IMAGE_BYTES)
		{
			return TEXT("The request image exceeds the 10 MB endpoint limit.");
		}
		if (!FSceneAutoTaggerNativeCoreAdapter::Get().Initialize())
		{
			return FSceneAutoTaggerNativeCoreAdapter::Get().GetDiagnostic();
		}
		return FString();
	}

	FString ValidateVisionRequest(const FConvaiVisionService::FVisionRequest& Request)
	{
		FString AuthError = ValidateAuth(Request.AuthHeaderAndKey);
		if (!AuthError.IsEmpty())
		{
			return AuthError;
		}
		if (Request.Prompt.TrimStartAndEnd().IsEmpty())
		{
			return TEXT("The vision request has an empty prompt.");
		}
		if (Request.CharacterID.TrimStartAndEnd().IsEmpty())
		{
			return TEXT("The vision request needs a Character ID.");
		}
		if (Request.Images.IsEmpty())
		{
			return TEXT("The vision request has no images.");
		}
		if (Request.Images.Num() > FConvaiVisionService::MaxImagesPerRequest)
		{
			return FString::Printf(
				TEXT("The vision request has %d images; the endpoint accepts at most %d."),
				Request.Images.Num(),
				FConvaiVisionService::MaxImagesPerRequest);
		}
		for (int32 ImageIndex = 0; ImageIndex < Request.Images.Num(); ++ImageIndex)
		{
			const FConvaiVisionService::FVisionImage& Image = Request.Images[ImageIndex];
			if (Image.PngBytes.IsEmpty())
			{
				return FString::Printf(TEXT("Vision request image %d is empty."), ImageIndex + 1);
			}
			if (static_cast<uint64>(Image.PngBytes.Num()) > CONVAI_SAT_VISION_MAX_IMAGE_BYTES)
			{
				return FString::Printf(
					TEXT("Vision request image %d exceeds the 10 MB endpoint limit."), ImageIndex + 1);
			}
			if (Image.FileName.TrimStartAndEnd().IsEmpty())
			{
				return FString::Printf(TEXT("Vision request image %d has an empty file name."), ImageIndex + 1);
			}
			for (int32 Other = 0; Other < ImageIndex; ++Other)
			{
				/* Byte-exact to match the core's uniqueness rule; UE's default
				 * FString comparison is case-insensitive and would reject pairs
				 * the transport contract accepts. */
				if (Request.Images[Other].FileName.Equals(Image.FileName, ESearchCase::CaseSensitive))
				{
					return FString::Printf(
						TEXT("Vision request image file names must be unique; '%s' repeats."),
						*Image.FileName);
				}
			}
		}
		if (!FSceneAutoTaggerNativeCoreAdapter::Get().Initialize())
		{
			return FSceneAutoTaggerNativeCoreAdapter::Get().GetDiagnostic();
		}
		return FString();
	}
}

void FConvaiVisionService::RequestTagging(
	const TPair<FString, FString>& AuthHeaderAndKey,
	TArray<uint8>&& SheetPng,
	const FString& FileName,
	TArray<FCellMeta> Cells,
	const FConvaiTaggingPipelineParams& Params,
	TFunction<void(const FResult&)> Completion)
{
	FString ValidationError = ValidateCommonInputs(AuthHeaderAndKey, SheetPng);
	if (ValidationError.IsEmpty() && Cells.IsEmpty())
	{
		ValidationError = TEXT("The tagging request has no cells.");
	}
	if (ValidationError.IsEmpty())
	{
		for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
		{
			if (Cells[CellIndex].EchoId.TrimStartAndEnd().IsEmpty())
			{
				ValidationError = FString::Printf(
					TEXT("Contact-sheet cell %d has an empty echo id; the native core rejects the whole request."),
					CellIndex + 1);
				break;
			}
		}
	}
	if (!ValidationError.IsEmpty())
	{
		CompleteOnGameThread(MoveTemp(Completion), MakeErrorResult(MoveTemp(ValidationError)));
		return;
	}

	const bool bSnapshot = GetDefault<UConvaiSceneAutoTaggerSettings>()->bSaveDebugCaptures;
	Async(
		EAsyncExecution::Thread,
		[AuthHeaderAndKey, SheetPng = MoveTemp(SheetPng), FileName, Cells = MoveTemp(Cells), Params,
			bSnapshot, Completion = MoveTemp(Completion)]() mutable
		{
			const FString SnapshotDir = bSnapshot
				? ConvaiVisionRequestSnapshot::SaveAutotagRequest(
					TEXT("Tagging"), AuthHeaderAndKey.Key, FileName, SheetPng, Cells, Params)
				: FString();
			FResult Result = ExecuteVisionRequest(
				CONVAI_SAT_VISION_TASK_OBJECT_TAGGING,
				AuthHeaderAndKey,
				SheetPng,
				FileName,
				Cells,
				Params);
			ConvaiVisionRequestSnapshot::SaveResult(SnapshotDir, Result);
			CompleteOnGameThread(MoveTemp(Completion), MoveTemp(Result));
		});
}

void FConvaiVisionService::RequestAssemblyDescription(
	const TPair<FString, FString>& AuthHeaderAndKey,
	TArray<uint8>&& Png,
	const FString& EchoId,
	const FConvaiTaggingPipelineParams& Params,
	TFunction<void(const FResult&)> Completion)
{
	FString ValidationError = ValidateCommonInputs(AuthHeaderAndKey, Png);
	if (ValidationError.IsEmpty() && EchoId.TrimStartAndEnd().IsEmpty())
	{
		ValidationError = TEXT("The assembly-description request has an empty echo id.");
	}
	if (!ValidationError.IsEmpty())
	{
		CompleteOnGameThread(MoveTemp(Completion), MakeErrorResult(MoveTemp(ValidationError)));
		return;
	}

	const bool bSnapshot = GetDefault<UConvaiSceneAutoTaggerSettings>()->bSaveDebugCaptures;
	Async(
		EAsyncExecution::Thread,
		[AuthHeaderAndKey, Png = MoveTemp(Png), EchoId, Params, bSnapshot,
			Completion = MoveTemp(Completion)]() mutable
		{
			TArray<FCellMeta> Cells;
			Cells.AddDefaulted_GetRef().EchoId = EchoId;
			const FString SnapshotDir = bSnapshot
				? ConvaiVisionRequestSnapshot::SaveAutotagRequest(
					TEXT("Assembly"), AuthHeaderAndKey.Key, TEXT("assembly.png"), Png, Cells, Params)
				: FString();
			FResult Result = ExecuteVisionRequest(
				CONVAI_SAT_VISION_TASK_ASSEMBLY_DESCRIPTION,
				AuthHeaderAndKey,
				Png,
				TEXT("assembly.png"),
				Cells,
				Params);
			/* Snapshot the wire result before normalization: an anomalous
			 * object count is exactly what the snapshot exists to show. */
			ConvaiVisionRequestSnapshot::SaveResult(SnapshotDir, Result);
			if (Result.bSuccess && Result.Objects.Num() != 1)
			{
				Result.bSuccess = false;
				Result.Error = FString::Printf(
					TEXT("The assembly description response carried %d objects; exactly one was expected."),
					Result.Objects.Num());
				Result.Objects.Reset();
			}
			CompleteOnGameThread(MoveTemp(Completion), MoveTemp(Result));
		});
}

void FConvaiVisionService::RequestSceneContextDescription(
	const TPair<FString, FString>& AuthHeaderAndKey,
	TArray<uint8>&& Png,
	const FString& EchoId,
	const FConvaiTaggingPipelineParams& Params,
	TFunction<void(const FResult&)> Completion)
{
	FString ValidationError = ValidateCommonInputs(AuthHeaderAndKey, Png);
	if (ValidationError.IsEmpty() && EchoId.TrimStartAndEnd().IsEmpty())
	{
		ValidationError = TEXT("The scene-context-description request has an empty echo id.");
	}
	if (!ValidationError.IsEmpty())
	{
		CompleteOnGameThread(MoveTemp(Completion), MakeErrorResult(MoveTemp(ValidationError)));
		return;
	}

	Async(
		EAsyncExecution::Thread,
		[AuthHeaderAndKey, Png = MoveTemp(Png), EchoId, Params,
			Completion = MoveTemp(Completion)]() mutable
		{
			TArray<FCellMeta> Cells;
			Cells.AddDefaulted_GetRef().EchoId = EchoId;
			FResult Result = ExecuteVisionRequest(
				CONVAI_SAT_VISION_TASK_SCENE_CONTEXT_DESCRIPTION,
				AuthHeaderAndKey,
				Png,
				TEXT("scene-context.png"),
				Cells,
				Params);
			if (Result.bSuccess && Result.Objects.Num() != 1)
			{
				Result.bSuccess = false;
				Result.Error = FString::Printf(
					TEXT("The scene context response carried %d objects; exactly one was expected."),
					Result.Objects.Num());
				Result.Objects.Reset();
			}
			CompleteOnGameThread(MoveTemp(Completion), MoveTemp(Result));
		});
}

void FConvaiVisionService::RequestVision(
	FVisionRequest&& Request,
	TFunction<void(const FRawResult&)> Completion)
{
	FString ValidationError = ValidateVisionRequest(Request);
	if (!ValidationError.IsEmpty())
	{
		FRawResult Result;
		Result.Error = MoveTemp(ValidationError);
		CompleteOnGameThread(MoveTemp(Completion), MoveTemp(Result));
		return;
	}

	const bool bSnapshot = GetDefault<UConvaiSceneAutoTaggerSettings>()->bSaveDebugCaptures;
	Async(
		EAsyncExecution::Thread,
		[Request = MoveTemp(Request), bSnapshot, Completion = MoveTemp(Completion)]() mutable
		{
			const FString SnapshotDir =
				bSnapshot ? ConvaiVisionRequestSnapshot::SaveVisionRequest(Request) : FString();
			FRawResult Result = ExecuteRawVisionRequest(Request);
			ConvaiVisionRequestSnapshot::SaveRawResult(SnapshotDir, Result);
			CompleteOnGameThread(MoveTemp(Completion), MoveTemp(Result));
		});
}
