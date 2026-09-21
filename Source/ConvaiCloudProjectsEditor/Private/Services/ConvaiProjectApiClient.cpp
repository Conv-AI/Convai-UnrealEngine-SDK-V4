// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiProjectApiClient.h"
#include "Utility/ConvaiEditorHttpCompat.h"

#include "ConvaiUtils.h"
#include "RestAPI/ConvaiURL.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
/** Endpoints, spelled exactly as the dashboard proxy forwards them upstream. */
const TCHAR* EndpointList = TEXT("ps-applications/list");
const TCHAR* EndpointCreate = TEXT("ps-applications/create");
const TCHAR* EndpointGet = TEXT("ps-applications/get");
const TCHAR* EndpointUpdate = TEXT("ps-applications/update");
const TCHAR* EndpointReserve = TEXT("ps-applications/version/create");
const TCHAR* EndpointJobCreate = TEXT("jobs/create");
const TCHAR* EndpointJobGet = TEXT("jobs/get");
const TCHAR* EndpointCreateWithScene = TEXT("xp/streams/createWithScene");
const TCHAR* EndpointEditExperience = TEXT("xp/streams/editExperience");
const TCHAR* EndpointExperienceList = TEXT("xp/experiences");
const TCHAR* EndpointExperiencePublish = TEXT("xp/experiences/publish");

FString JsonToString(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

/** Unwraps the { ERROR: "..." } envelope the API uses for handled failures. */
bool ReadEnvelope(const FString& Body, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
	{
		OutError = TEXT("The Convai API returned a response that could not be read.");
		return false;
	}
	FString Reported;
	if (OutRoot->TryGetStringField(TEXT("ERROR"), Reported) && !Reported.IsEmpty())
	{
		OutError = Reported;
		return false;
	}
	return true;
}

/** Build ids arrive as task_id on some responses and job_id on others. */
FString ReadBuildId(const TSharedPtr<FJsonObject>& Build)
{
	if (!Build.IsValid()) return FString();
	FString Id;
	if (Build->TryGetStringField(TEXT("task_id"), Id) && !Id.IsEmpty()) return Id;
	Build->TryGetStringField(TEXT("job_id"), Id);
	return Id;
}
}

FConvaiProjectApiClient::FConvaiProjectApiClient(FString InAuthHeader, FString InApiKey, FString InBaseUrl)
	: AuthHeader(MoveTemp(InAuthHeader)), ApiKey(MoveTemp(InApiKey)), BaseUrl(MoveTemp(InBaseUrl))
{
	while (BaseUrl.EndsWith(TEXT("/"))) BaseUrl.LeftChopInline(1);
}

TSharedPtr<FConvaiProjectApiClient> FConvaiProjectApiClient::Create(FString& OutError)
{
	const TPair<FString, FString> Auth = UConvaiUtils::GetAuthHeaderAndKey();
	if (Auth.Value.IsEmpty())
	{
		OutError = TEXT("Sign in to Convai to manage your uploaded projects.");
		return nullptr;
	}

	// The whole SDK resolves one host, so -ConvaiBetaURL= already points a workstation at a
	// staging slot. The dedicated override exists for the case where only this API moves.
	FString BaseUrl;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ConvaiProjectApiURL="), BaseUrl) || BaseUrl.IsEmpty())
	{
		BaseUrl = UConvaiURL::GetBaseURL(true);
	}
	BaseUrl.TrimStartAndEndInline();
	return MakeShareable(new FConvaiProjectApiClient(Auth.Key, Auth.Value, BaseUrl));
}

void FConvaiProjectApiClient::CancelAll()
{
	++Epoch;
	for (const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>& Weak : Pending)
	{
		if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = Weak.Pin()) Request->CancelRequest();
	}
	Pending.Reset();
}

void FConvaiProjectApiClient::Post(const FString& Endpoint, const TSharedRef<FJsonObject>& Payload,
	TFunction<void(TSharedPtr<FJsonObject>, FString)> Completion)
{
	check(IsInGameThread());
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BaseUrl / Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetTimeout(120.0f);
	Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	if (!AuthHeader.IsEmpty()) Request->SetHeader(AuthHeader, ApiKey);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(JsonToString(Payload));

	const uint64 RequestEpoch = Epoch;
	TWeakPtr<FConvaiProjectApiClient> WeakSelf = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, RequestEpoch, Callback = MoveTemp(Completion)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			const TSharedPtr<FConvaiProjectApiClient> Self = WeakSelf.Pin();
			if (!Self || Self->Epoch != RequestEpoch) return;

			if (!bConnected || !Response.IsValid())
			{
				Callback(nullptr, TEXT("Could not reach the Convai API. Check your connection and retry."));
				return;
			}
			TSharedPtr<FJsonObject> Root;
			FString Error;
			if (!ReadEnvelope(Response->GetContentAsString(), Root, Error))
			{
				Callback(nullptr, Error);
				return;
			}
			if (!EHttpResponseCodes::IsOk(Response->GetResponseCode()))
			{
				Callback(nullptr, FString::Printf(TEXT("The Convai API rejected the request (%d)."), Response->GetResponseCode()));
				return;
			}
			Callback(Root, FString());
		});

	Pending.RemoveAll([](const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid(); });
	Pending.Add(Request);
	Request->ProcessRequest();
}

FConvaiProject FConvaiProjectApiClient::ParseProject(const TSharedPtr<FJsonObject>& Object)
{
	FConvaiProject Project;
	if (!Object.IsValid()) return Project;

	Object->TryGetStringField(TEXT("ps_application_id"), Project.Id);
	Object->TryGetStringField(TEXT("name"), Project.Name);
	Object->TryGetStringField(TEXT("updated_at"), Project.UpdatedAt);
	Project.ActiveBuildId = ReadBuildId(Object->HasTypedField<EJson::Object>(TEXT("active_build")) ? Object->GetObjectField(TEXT("active_build")) : nullptr);
	Project.DesiredBuildId = ReadBuildId(Object->HasTypedField<EJson::Object>(TEXT("desired_build")) ? Object->GetObjectField(TEXT("desired_build")) : nullptr);

	if (Object->HasTypedField<EJson::Object>(TEXT("metadata")))
	{
		const TSharedPtr<FJsonObject> Metadata = Object->GetObjectField(TEXT("metadata"));
		if (Metadata->HasTypedField<EJson::Object>(TEXT("deployment")))
		{
			const TSharedPtr<FJsonObject> Deployment = Metadata->GetObjectField(TEXT("deployment"));
			Deployment->TryGetStringField(TEXT("status"), Project.DeploymentStatus);
			// Fall back to the recorded ids: a light list response omits the build objects.
			if (Project.ActiveBuildId.IsEmpty()) Deployment->TryGetStringField(TEXT("active_build_id"), Project.ActiveBuildId);
			if (Project.DesiredBuildId.IsEmpty()) Deployment->TryGetStringField(TEXT("desired_build_id"), Project.DesiredBuildId);
		}
		Metadata->TryGetStringField(TEXT("experience_id"), Project.ExperienceId);
	}

	const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
	if (Object->TryGetArrayField(TEXT("versions"), Versions))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Versions)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry)) continue;

			FConvaiProjectVersion Version;
			(*Entry)->TryGetStringField(TEXT("asset_version_id"), Version.AssetVersionId);
			(*Entry)->TryGetStringField(TEXT("version"), Version.Version);
			(*Entry)->TryGetStringField(TEXT("file_name"), Version.FileName);
			(*Entry)->TryGetStringField(TEXT("upload_status"), Version.UploadStatus);
			double Size = 0.0;
			if ((*Entry)->TryGetNumberField(TEXT("size_bytes"), Size)) Version.SizeBytes = static_cast<int64>(Size);
			if ((*Entry)->HasTypedField<EJson::Object>(TEXT("latest_job")))
			{
				const TSharedPtr<FJsonObject> Job = (*Entry)->GetObjectField(TEXT("latest_job"));
				Version.JobId = ReadBuildId(Job);
				Job->TryGetStringField(TEXT("status"), Version.JobStatus);
			}
			Project.Versions.Add(MoveTemp(Version));
		}
	}
	return Project;
}

void FConvaiProjectApiClient::List(FProjectsCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("page"), 1);
	Payload->SetNumberField(TEXT("per_page"), 100);
	Post(EndpointList, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		TArray<FConvaiProject> Projects;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (Error.IsEmpty() && Root.IsValid() && Root->TryGetArrayField(TEXT("applications"), Entries))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Entries)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Entry)) Projects.Add(ParseProject(*Entry));
			}
		}
		Callback(MoveTemp(Projects), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::Get(const FString& ProjectId, FProjectCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);
	Post(EndpointGet, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FConvaiProject Project;
		if (Error.IsEmpty() && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("application")))
			Project = ParseProject(Root->GetObjectField(TEXT("application")));
		else if (Error.IsEmpty())
			Error = TEXT("The Convai API did not return the project.");
		Callback(MoveTemp(Project), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::CreateProject(const FString& Name, FProjectCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("name"), Name);
	Payload->SetObjectField(TEXT("metadata"), MakeShared<FJsonObject>());
	Post(EndpointCreate, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FConvaiProject Project;
		if (Error.IsEmpty() && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("application")))
			Project = ParseProject(Root->GetObjectField(TEXT("application")));
		if (Error.IsEmpty() && Project.Id.IsEmpty())
			Error = TEXT("The Convai API did not confirm the new project. Refresh before creating it again.");
		Callback(MoveTemp(Project), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::ReserveVersion(const FString& ProjectId, const FString& Version,
	const FString& ArchivePath, FReservationCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);
	Payload->SetStringField(TEXT("version"), Version);
	Payload->SetStringField(TEXT("file_name"), FPaths::GetCleanFilename(ArchivePath));
	Payload->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*ArchivePath)));
	Payload->SetStringField(TEXT("content_type"), TEXT("application/zip"));

	Post(EndpointReserve, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FConvaiProjectReservation Reservation;
		if (Error.IsEmpty() && Root.IsValid())
		{
			Root->TryGetStringField(TEXT("asset_version_id"), Reservation.AssetVersionId);
			if (Root->HasTypedField<EJson::Object>(TEXT("upload")))
			{
				const TSharedPtr<FJsonObject> Upload = Root->GetObjectField(TEXT("upload"));
				Upload->TryGetStringField(TEXT("url"), Reservation.UploadUrl);
				if (Upload->HasTypedField<EJson::Object>(TEXT("headers")))
				{
					for (const auto& Pair : Upload->GetObjectField(TEXT("headers"))->Values)
					{
						FString HeaderValue;
						// The key type of FJsonObject::Values varies by engine version; dereferencing
						// yields TCHAR* on every one of them.
						if (Pair.Value.IsValid() && Pair.Value->TryGetString(HeaderValue))
							Reservation.UploadHeaders.Add(FString(*Pair.Key), HeaderValue);
					}
				}
			}
		}
		if (Error.IsEmpty() && Reservation.UploadUrl.IsEmpty())
			Error = TEXT("The Convai API did not return an upload URL for this version.");
		Callback(MoveTemp(Reservation), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::UploadArchive(const FConvaiProjectReservation& Reservation, const FString& ArchivePath,
	FProgress Progress, FStringCallback Completion)
{
	check(IsInGameThread());
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Reservation.UploadUrl);
	Request->SetVerb(TEXT("PUT"));
	// A multi-gigabyte archive can outlast any sane API timeout; storage gets its own budget.
	Request->SetTimeout(6 * 60 * 60.0f);
	Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/zip"));
	// The signed URL is bound to the headers it was signed with; those win over our default.
	for (const TPair<FString, FString>& Header : Reservation.UploadHeaders) Request->SetHeader(Header.Key, Header.Value);

	if (!Request->SetContentAsStreamedFile(ArchivePath))
	{
		Completion(FString(), FString::Printf(TEXT("The archive could not be opened for upload: %s"), *ArchivePath));
		return;
	}

	const int64 Total = IFileManager::Get().FileSize(*ArchivePath);
	if (Progress)
	{
		ConvaiEditorHttpCompat::BindProgress(*Request, [Callback = MoveTemp(Progress), Total](FHttpRequestPtr, uint64 Sent, uint64)
		{
			Callback(static_cast<int64>(Sent), Total);
		});
	}

	const uint64 RequestEpoch = Epoch;
	TWeakPtr<FConvaiProjectApiClient> WeakSelf = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, RequestEpoch, Callback = MoveTemp(Completion)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			const TSharedPtr<FConvaiProjectApiClient> Self = WeakSelf.Pin();
			if (!Self || Self->Epoch != RequestEpoch) return;

			if (!bConnected || !Response.IsValid())
			{
				Callback(FString(), TEXT("The upload was interrupted before it finished."));
				return;
			}
			if (!EHttpResponseCodes::IsOk(Response->GetResponseCode()))
			{
				Callback(FString(), FString::Printf(TEXT("The archive upload failed (%d)."), Response->GetResponseCode()));
				return;
			}
			Callback(FString(), FString());
		});

	Pending.Add(Request);
	Request->ProcessRequest();
}

void FConvaiProjectApiClient::StartBuild(const FString& ProjectId, const FString& AssetVersionId, FStringCallback Completion)
{
	const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
	// The container image the cloud host builds around the archive, not the platform the archive
	// was packaged for. The dashboard sends this verbatim for Windows uploads as well.
	Parameters->SetStringField(TEXT("export"), TEXT("linux"));

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("job_type"), TEXT("ps_application"));
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);
	Payload->SetStringField(TEXT("asset_version_id"), AssetVersionId);
	Payload->SetObjectField(TEXT("parameters"), Parameters);

	Post(EndpointJobCreate, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FString JobId;
		if (Error.IsEmpty() && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("job")))
			JobId = ReadBuildId(Root->GetObjectField(TEXT("job")));
		if (Error.IsEmpty() && JobId.IsEmpty())
			Error = TEXT("The build was requested but the Convai API did not return a build id.");
		Callback(MoveTemp(JobId), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::GetBuildStatus(const FString& JobId, FStringCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("job_id"), JobId);
	Post(EndpointJobGet, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FString Status;
		if (Error.IsEmpty() && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("job")))
			Root->GetObjectField(TEXT("job"))->TryGetStringField(TEXT("status"), Status);
		Callback(MoveTemp(Status), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::GetBuildReport(const FString& JobId, TFunction<void(FConvaiBuildReport, FString)> Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("job_id"), JobId);
	Post(EndpointJobGet, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FConvaiBuildReport Report;
		if (Error.IsEmpty() && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("job")))
		{
			const TSharedPtr<FJsonObject> Job = Root->GetObjectField(TEXT("job"));
			Job->TryGetStringField(TEXT("status"), Report.Status);

			const TArray<TSharedPtr<FJsonValue>>* Logs = nullptr;
			if (Job->HasTypedField<EJson::Object>(TEXT("task_metadata"))
				&& Job->GetObjectField(TEXT("task_metadata"))->TryGetArrayField(TEXT("logs"), Logs))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Logs)
				{
					const TSharedPtr<FJsonObject>* Entry = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Entry)) continue;

					FString Message, Timestamp, Level;
					(*Entry)->TryGetStringField(TEXT("message"), Message);
					(*Entry)->TryGetStringField(TEXT("timestamp"), Timestamp);
					(*Entry)->TryGetStringField(TEXT("level"), Level);
					if (Message.IsEmpty()) continue;

					// Keep the level and time when the server sent them; a bare message is still useful.
					FString Line;
					if (!Timestamp.IsEmpty()) Line += FString::Printf(TEXT("[%s] "), *Timestamp);
					if (!Level.IsEmpty()) Line += FString::Printf(TEXT("%s: "), *Level.ToUpper());
					Report.Logs.Add(Line + Message);
				}
			}
		}
		Callback(MoveTemp(Report), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::ListExperiences(const FString& ProjectId,
	TFunction<void(TArray<FConvaiExperienceLink>, FString)> Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("type"), TEXT("user"));
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);

	Post(EndpointExperienceList, Payload,
		[ProjectId, Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		TArray<FConvaiExperienceLink> Pages;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (Error.IsEmpty() && Root.IsValid() && Root->TryGetArrayField(TEXT("experiences"), Entries))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Entries)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry)) continue;

				// The link lives either at the top level or in metadata depending on the response.
				FString Owner;
				if (!(*Entry)->TryGetStringField(TEXT("ps_application_id"), Owner)
					&& (*Entry)->HasTypedField<EJson::Object>(TEXT("metadata")))
				{
					(*Entry)->GetObjectField(TEXT("metadata"))->TryGetStringField(TEXT("ps_application_id"), Owner);
				}
				if (Owner != ProjectId) continue;

				FConvaiExperienceLink Page;
				(*Entry)->TryGetStringField(TEXT("experience_id"), Page.ExperienceId);
				(*Entry)->TryGetStringField(TEXT("experience_name"), Page.Name);
				(*Entry)->TryGetStringField(TEXT("experience_description"), Page.Description);
				(*Entry)->TryGetStringField(TEXT("visibility"), Page.Visibility);
				if (Page.Visibility.IsEmpty()) Page.Visibility = TEXT("draft");
				if (!Page.ExperienceId.IsEmpty()) Pages.Add(MoveTemp(Page));
			}
		}
		Callback(MoveTemp(Pages), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::PublishExperience(const FConvaiExperienceLink& Page, bool bPublish, FStringCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("experience_id"), Page.ExperienceId);
	// "save" keeps the page a draft; "publish" makes it live at the chosen visibility.
	Payload->SetStringField(TEXT("action"), bPublish ? TEXT("publish") : TEXT("save"));
	Payload->SetStringField(TEXT("experience_name"), Page.Name);
	Payload->SetStringField(TEXT("experience_description"), Page.Description);
	Payload->SetStringField(TEXT("visibility"), Page.Visibility);

	Post(EndpointExperiencePublish, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject>, FString Error)
	{
		Callback(FString(), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::SaveHostingDefaults(const FString& ProjectId, FStringCallback Completion)
{
	// One machine that sleeps when unused, with a single ready stream: the smallest configuration
	// that still lets a first activation provision capacity. Anything larger is a billing decision
	// and belongs on the dashboard, not in a one-click upload.
	const TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	Config->SetBoolField(TEXT("on_demand_mode"), true);
	Config->SetNumberField(TEXT("target_free_streams"), 1);
	Config->SetNumberField(TEXT("streams_per_instance"), 1);
	Config->SetNumberField(TEXT("min_instances"), 0);
	Config->SetNumberField(TEXT("max_instances"), 1);
	Config->SetNumberField(TEXT("idle_timeout_minutes"), 10);

	const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetObjectField(TEXT("deployment_config"), Config);

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);
	Payload->SetObjectField(TEXT("metadata"), Metadata);
	Post(EndpointUpdate, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject>, FString Error)
	{
		Callback(FString(), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::Activate(const FString& ProjectId, const FString& BuildId, FStringCallback Completion)
{
	const TSharedRef<FJsonObject> Deployment = MakeShared<FJsonObject>();
	Deployment->SetStringField(TEXT("desired_build_id"), BuildId);

	const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetObjectField(TEXT("deployment"), Deployment);

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);
	Payload->SetObjectField(TEXT("metadata"), Metadata);
	Post(EndpointUpdate, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject>, FString Error)
	{
		Callback(FString(), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::CreateExperience(const FString& ProjectId, FStringCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("ps_application_id"), ProjectId);
	Payload->SetStringField(TEXT("experience_mode"), TEXT("av_sim"));
	Payload->SetBoolField(TEXT("use_edit_mode"), true);
	Payload->SetBoolField(TEXT("allocate_server"), false);
	Post(EndpointCreateWithScene, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FString ExperienceId;
		if (Error.IsEmpty() && Root.IsValid() && Root->HasTypedField<EJson::Object>(TEXT("experience")))
			Root->GetObjectField(TEXT("experience"))->TryGetStringField(TEXT("experience_id"), ExperienceId);
		if (Error.IsEmpty() && ExperienceId.IsEmpty())
			Error = TEXT("The Convai API did not return an experience page for this project.");
		Callback(MoveTemp(ExperienceId), MoveTemp(Error));
	});
}

void FConvaiProjectApiClient::StartStreamSession(const FString& ExperienceId, FStringCallback Completion)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("experience_id"), ExperienceId);
	Payload->SetBoolField(TEXT("allocate_server"), false);
	Post(EndpointEditExperience, Payload, [Callback = MoveTemp(Completion)](TSharedPtr<FJsonObject> Root, FString Error)
	{
		FString SessionId;
		FString Status;
		if (Error.IsEmpty() && Root.IsValid())
		{
			if (Root->TryGetStringField(TEXT("status"), Status) && Status == TEXT("retry"))
			{
				FString Message;
				Root->TryGetStringField(TEXT("message"), Message);
				Error = Message.IsEmpty() ? TEXT("The stream is still starting. Try again in a moment.") : Message;
			}
			else if (Root->HasTypedField<EJson::Object>(TEXT("session")))
			{
				Root->GetObjectField(TEXT("session"))->TryGetStringField(TEXT("session_id"), SessionId);
			}
		}
		if (Error.IsEmpty() && SessionId.IsEmpty())
			Error = TEXT("The Convai API did not return a stream session.");
		Callback(MoveTemp(SessionId), MoveTemp(Error));
	});
}

FString FConvaiProjectApiClient::BuildStreamUrl(const FString& SessionId)
{
	if (SessionId.IsEmpty()) return FString();

	// The player host tracks the API host, so a staging slot opens its own player.
	FString Player;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ConvaiExperienceURL="), Player) || Player.IsEmpty())
		Player = TEXT("https://x.convai.com");
	while (Player.EndsWith(TEXT("/"))) Player.LeftChopInline(1);
	return FString::Printf(TEXT("%s/stream-v2/%s/"), *Player, *FGenericPlatformHttp::UrlEncode(SessionId));
}

FString FConvaiProjectApiClient::NextVersion(const TArray<FConvaiProjectVersion>& Versions)
{
	int32 BestMajor = -1, BestMinor = 0, BestPatch = 0;
	for (const FConvaiProjectVersion& Version : Versions)
	{
		TArray<FString> Parts;
		if (Version.Version.ParseIntoArray(Parts, TEXT("."), false) != 3) continue;
		if (!Parts[0].IsNumeric() || !Parts[1].IsNumeric() || !Parts[2].IsNumeric()) continue;

		const int32 Major = FCString::Atoi(*Parts[0]);
		const int32 Minor = FCString::Atoi(*Parts[1]);
		const int32 Patch = FCString::Atoi(*Parts[2]);
		if (Major > BestMajor || (Major == BestMajor && (Minor > BestMinor || (Minor == BestMinor && Patch > BestPatch))))
		{
			BestMajor = Major; BestMinor = Minor; BestPatch = Patch;
		}
	}
	return BestMajor < 0 ? TEXT("1.0.0") : FString::Printf(TEXT("%d.%d.%d"), BestMajor, BestMinor, BestPatch + 1);
}

bool FConvaiProjectApiClient::IsBuildSucceeded(const FString& Status)
{
	const FString Lower = Status.ToLower();
	return Lower == TEXT("complete_with_success") || Lower == TEXT("succeeded") || Lower == TEXT("success") || Lower == TEXT("ready");
}

bool FConvaiProjectApiClient::IsBuildFailed(const FString& Status)
{
	const FString Lower = Status.ToLower();
	return Lower == TEXT("complete_with_failure") || Lower == TEXT("complete_with_error") || Lower == TEXT("failed")
		|| Lower == TEXT("failure") || Lower == TEXT("error") || Lower == TEXT("cancelled") || Lower == TEXT("canceled");
}

bool FConvaiProjectApiClient::IsBuildInProgress(const FString& Status)
{
	const FString Lower = Status.ToLower();
	return Lower == TEXT("assigned") || Lower == TEXT("created") || Lower == TEXT("queued")
		|| Lower == TEXT("processing") || Lower == TEXT("building") || Lower == TEXT("running");
}
