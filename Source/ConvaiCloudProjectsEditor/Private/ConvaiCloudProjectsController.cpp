// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiCloudProjectsController.h"

#include "Packaging/ConvaiProjectPrepare.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/IAuthWindowManager.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"

#define LOCTEXT_NAMESPACE "ConvaiCloudProjects"

namespace
{
/** Build polling: every 10s for up to an hour. A cook that overruns that is a dashboard problem. */
constexpr float PollIntervalSeconds = 10.f;
constexpr int32 MaxPollAttempts = 360;

FString StatusFor(const FConvaiProject& Project, bool& bOutLive, bool& bOutWorking, bool& bOutFailed)
{
	bOutLive = bOutWorking = bOutFailed = false;

	const FConvaiProjectVersion* Latest = Project.Versions.IsEmpty() ? nullptr : &Project.Versions.Last();
	const FString BuildId = Latest ? Latest->JobId : FString();

	if (!BuildId.IsEmpty() && BuildId == Project.ActiveBuildId)
	{
		bOutLive = true;
		return TEXT("Live");
	}
	if (Latest && FConvaiProjectApiClient::IsBuildFailed(Latest->JobStatus))
	{
		bOutFailed = true;
		return TEXT("Build failed");
	}
	if (Latest && FConvaiProjectApiClient::IsBuildInProgress(Latest->JobStatus))
	{
		bOutWorking = true;
		return TEXT("Building");
	}
	if (!BuildId.IsEmpty() && BuildId == Project.DesiredBuildId)
	{
		bOutWorking = true;
		return TEXT("Activating");
	}
	if (!Project.ActiveBuildId.IsEmpty())
	{
		bOutLive = true;
		return TEXT("Live");
	}
	return Latest ? TEXT("Uploaded") : TEXT("Empty");
}

FString FormatBytes(int64 Bytes)
{
	if (Bytes < 1024) return FString::Printf(TEXT("%lld B"), Bytes);
	if (Bytes < 1024 * 1024) return FString::Printf(TEXT("%.1f KB"), Bytes / 1024.0);
	if (Bytes < 1024ll * 1024 * 1024) return FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0));
	return FString::Printf(TEXT("%.2f GB"), Bytes / (1024.0 * 1024.0 * 1024.0));
}
}

TSharedRef<SConvaiCloudProjects> FConvaiCloudProjectsController::CreateWidget()
{
	State.LocalProjectName = FConvaiProjectPrepare::GetProjectName();
	State.PackagingDisabledReason = FConvaiProjectPackager::GetPlatformUnavailableReason();

	TSharedRef<SConvaiCloudProjects> New = SNew(SConvaiCloudProjects)
		.OnRefresh_Lambda([this]() { Refresh(); })
		.OnSignIn_Lambda([this]() { SignIn(); })
		.OnCancelJob_Lambda([this]() { Cancel(); })
		.OnSelect_Lambda([this](const FString& Id) { Select(Id); })
		.OnCreate_Lambda([this](const FString& Name) { Create(Name); })
		.OnUploadChanges_Lambda([this](const FString& Id) { UploadChanges(Id); })
		.OnOpenInBrowser_Lambda([this](const FString& Id) { OpenInBrowser(Id); })
		.OnActivateVersion_Lambda([this](const FString& BuildId) { ActivateVersion(BuildId); })
		.OnShowLogs_Lambda([this](const FString& BuildId) { ShowLogs(BuildId); })
		.OnPublish_Lambda([this](const FConvaiProjectPublishState& Page, bool bPublish) { PublishPage(Page, bPublish); });

	Widget = New;
	Publish();
	Refresh();
	return New;
}

void FConvaiCloudProjectsController::Shutdown()
{
	bShuttingDown = true;
	StopPolling();
	if (Packager) Packager->Cancel();
	if (Client) Client->CancelAll();
	Packager.Reset();
	Client.Reset();
	Widget.Reset();
}

void FConvaiCloudProjectsController::Publish()
{
	if (Widget.IsValid() && !bShuttingDown) Widget->SetState(State);
}

bool FConvaiCloudProjectsController::EnsureClient()
{
	if (Client) return true;

	FString Error;
	Client = FConvaiProjectApiClient::Create(Error);
	State.bNeedsSignIn = !Client.IsValid();
	if (!Client)
	{
		State.Error = Error;
		Publish();
		return false;
	}
	State.Error.Reset();
	return true;
}

void FConvaiCloudProjectsController::SignIn()
{
	if (State.bBusy || bShuttingDown) return;

	auto Manager = FConvaiDIContainerManager::Get().Resolve<IAuthWindowManager>();
	if (Manager.IsSuccess())
	{
		const auto AuthManager = Manager.GetValue();
		if (AuthManager->GetAuthState() == EAuthFlowState::Authenticating) return;
		if (AuthManager->GetAuthState() != EAuthFlowState::Welcome) AuthManager->OnAuthCancelled();
		AuthManager->StartAuthFlow();
		return;
	}
	auto OAuth = FConvaiDIContainerManager::Get().Resolve<IOAuthAuthenticationService>();
	if (OAuth.IsSuccess()) OAuth.GetValue()->StartLogin();
	else Fail(TEXT("Sign-in could not open. Restart the editor and try again."));
}

void FConvaiCloudProjectsController::Refresh()
{
	if (bShuttingDown || State.bLoading) return;

	// A sign-out between refreshes has to retire the old client, not reuse its credentials.
	Client.Reset();
	if (!EnsureClient()) return;

	State.bLoading = true;
	State.Error.Reset();
	Publish();

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->List([WeakSelf](TArray<FConvaiProject> Found, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		Self->State.bLoading = false;
		if (!Error.IsEmpty())
		{
			Self->Fail(Error);
			return;
		}
		Self->RebuildCards(Found);
		Self->Publish();
	});
}

void FConvaiCloudProjectsController::RebuildCards(const TArray<FConvaiProject>& InProjects)
{
	Projects = InProjects;
	State.Projects.Reset();

	for (const FConvaiProject& Project : Projects)
	{
		FConvaiCloudProjectCard Card;
		Card.Id = Project.Id;
		Card.Name = Project.Name;
		Card.UpdatedAt = Project.UpdatedAt;
		if (!Project.Versions.IsEmpty()) Card.Version = Project.Versions.Last().Version;
		Card.Status = StatusFor(Project, Card.bLive, Card.bWorking, Card.bFailed);
		State.Projects.Add(MoveTemp(Card));
	}

	// A selection that no longer exists would leave the details panel describing nothing.
	if (!State.SelectedId.IsEmpty() && !FindProject(State.SelectedId)) State.SelectedId.Reset();
	Select(State.SelectedId);
}

const FConvaiProject* FConvaiCloudProjectsController::FindProject(const FString& ProjectId) const
{
	return Projects.FindByPredicate([&ProjectId](const FConvaiProject& Project) { return Project.Id == ProjectId; });
}

void FConvaiCloudProjectsController::Select(const FString& ProjectId)
{
	State.SelectedId = ProjectId;
	State.NameMismatchWarning.Reset();

	// The warning the meeting asked for: say that the names differ, never block the update.
	const FConvaiProject* Project = FindProject(ProjectId);
	if (Project && !Project->Name.IsEmpty() && !State.LocalProjectName.IsEmpty()
		&& !Project->Name.Equals(State.LocalProjectName, ESearchCase::IgnoreCase))
	{
		State.NameMismatchWarning = FString::Printf(
			TEXT("This project was uploaded as '%s', but the open Unreal project is '%s'. ")
			TEXT("Uploading changes replaces '%s' with what is open here."),
			*Project->Name, *State.LocalProjectName, *Project->Name);
	}
	RefreshSelectionDetails();
	Publish();
}

void FConvaiCloudProjectsController::RefreshSelectionDetails()
{
	State.Versions.Reset();
	// A log belongs to the project it was opened from; keep it only while that selection holds.
	if (State.LogVersion.IsEmpty() || State.SelectedId.IsEmpty())
	{
		State.LogVersion.Reset();
		State.LogLines.Reset();
	}

	const FConvaiProject* Project = FindProject(State.SelectedId);
	if (!Project)
	{
		State.Publish = FConvaiProjectPublishState();
		return;
	}

	for (int32 Index = Project->Versions.Num() - 1; Index >= 0; --Index)
	{
		const FConvaiProjectVersion& Version = Project->Versions[Index];

		FConvaiProjectVersionRow Row;
		Row.Version = Version.Version;
		Row.BuildId = Version.JobId;
		Row.Size = Version.SizeBytes > 0 ? FormatBytes(Version.SizeBytes) : FString();
		Row.bActive = !Version.JobId.IsEmpty() && Version.JobId == Project->ActiveBuildId;
		Row.bHasLogs = !Version.JobId.IsEmpty();

		if (FConvaiProjectApiClient::IsBuildSucceeded(Version.JobStatus)) Row.Status = TEXT("Built");
		else if (FConvaiProjectApiClient::IsBuildFailed(Version.JobStatus)) Row.Status = TEXT("Build failed");
		else if (FConvaiProjectApiClient::IsBuildInProgress(Version.JobStatus)) Row.Status = TEXT("Building");
		else Row.Status = Version.JobId.IsEmpty() ? TEXT("Uploaded") : Version.JobStatus;

		// Only a finished build that is not already serving can be switched to.
		Row.bCanActivate = !Row.bActive && !Row.BuildId.IsEmpty()
			&& FConvaiProjectApiClient::IsBuildSucceeded(Version.JobStatus);

		State.Versions.Add(MoveTemp(Row));
	}

	// The page's name and audience come from the experience API, not the application record.
	if (!Client) return;
	const FString RequestedId = State.SelectedId;
	State.Publish.bLoading = true;

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->ListExperiences(RequestedId, [WeakSelf, RequestedId](TArray<FConvaiExperienceLink> Pages, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		// A response for a selection the user has already moved off would overwrite the new one.
		if (!Self || Self->bShuttingDown || Self->State.SelectedId != RequestedId) return;

		Self->State.Publish.bLoading = false;
		if (!Error.IsEmpty() || Pages.IsEmpty())
		{
			Self->State.Publish = FConvaiProjectPublishState();
			Self->Publish();
			return;
		}

		const FConvaiExperienceLink& Page = Pages[0];
		Self->State.Publish.ExperienceId = Page.ExperienceId;
		Self->State.Publish.Name = Page.Name.IsEmpty() ? Self->State.LocalProjectName : Page.Name;
		Self->State.Publish.Description = Page.Description;
		Self->State.Publish.Visibility = Page.Visibility;

		// Remember it so Open in browser reuses this page instead of creating a second one.
		if (FConvaiProject* Cached = Self->Projects.FindByPredicate(
			[&RequestedId](const FConvaiProject& Item) { return Item.Id == RequestedId; }))
		{
			Cached->ExperienceId = Page.ExperienceId;
		}
		Self->Publish();
	});
}

void FConvaiCloudProjectsController::ActivateVersion(const FString& BuildId)
{
	if (State.bBusy || bShuttingDown || BuildId.IsEmpty() || !EnsureClient()) return;

	const FString ProjectId = State.SelectedId;
	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	SetJob(EConvaiProjectStage::Activate, TEXT("Activating the version"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	// Hosting must exist before a build can serve; saving defaults again is harmless.
	Client->SaveHostingDefaults(ProjectId, [WeakSelf, ProjectId, BuildId](FString, FString HostingError)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		if (!HostingError.IsEmpty())
		{
			Self->Fail(FString::Printf(TEXT("Hosting could not be configured: %s"), *HostingError));
			return;
		}
		Self->Client->Activate(ProjectId, BuildId, [WeakSelf](FString, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
			if (!Owner || Owner->bShuttingDown) return;

			if (!Error.IsEmpty()) { Owner->Fail(Error); return; }
			Owner->CompleteRun(TEXT("That version is being made active."));
		});
	});
}

void FConvaiCloudProjectsController::ShowLogs(const FString& BuildId)
{
	if (bShuttingDown || BuildId.IsEmpty() || !EnsureClient()) return;

	const FConvaiProjectVersionRow* Row = State.Versions.FindByPredicate(
		[&BuildId](const FConvaiProjectVersionRow& Item) { return Item.BuildId == BuildId; });

	State.LogVersion = Row ? Row->Version : BuildId;
	State.LogLines.Reset();
	State.bLogsLoading = true;
	Publish();

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->GetBuildReport(BuildId, [WeakSelf, BuildId](FConvaiBuildReport Report, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		Self->State.bLogsLoading = false;
		// Report the failure inside the log view; it is the thing the user just opened.
		Self->State.LogLines = Error.IsEmpty() ? Report.Logs : TArray<FString>{ Error };
		Self->Publish();
	});
}

void FConvaiCloudProjectsController::PublishPage(const FConvaiProjectPublishState& Page, bool bPublish)
{
	if (State.bBusy || bShuttingDown || !EnsureClient()) return;
	if (State.Publish.ExperienceId.IsEmpty())
	{
		Fail(TEXT("This project has no experience page yet. Use Open in browser once a version is active."));
		return;
	}

	FConvaiExperienceLink Request;
	Request.ExperienceId = State.Publish.ExperienceId;
	Request.Name = Page.Name.TrimStartAndEnd();
	Request.Description = Page.Description.TrimStartAndEnd();
	Request.Visibility = Page.Visibility;

	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	SetJob(EConvaiProjectStage::None, bPublish ? TEXT("Publishing the page") : TEXT("Saving the page"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->PublishExperience(Request, bPublish, [WeakSelf, bPublish, Visibility = Request.Visibility](FString, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		Self->State.bBusy = false;
		Self->ClearJob();
		if (!Error.IsEmpty()) { Self->Fail(Error); return; }

		Self->State.Notice = bPublish
			? FString::Printf(TEXT("Published. The page is now %s."), *Visibility)
			: TEXT("Saved as a draft. The page is not live yet.");
		Self->RefreshSelectionDetails();
		Self->Publish();
	});
}

void FConvaiCloudProjectsController::Create(const FString& Name)
{
	if (State.bBusy || bShuttingDown || !EnsureClient()) return;

	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	SetJob(EConvaiProjectStage::Prepare, TEXT("Creating the project"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->CreateProject(Name, [WeakSelf](FConvaiProject Created, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		if (!Error.IsEmpty())
		{
			// Creation is not idempotent: send the user to Refresh rather than silently retrying.
			Self->Fail(FString::Printf(TEXT("%s Refresh before creating it again."), *Error));
			return;
		}
		Self->Projects.Add(Created);
		Self->BeginRun(Created.Id, true);
	});
}

void FConvaiCloudProjectsController::UploadChanges(const FString& ProjectId)
{
	if (State.bBusy || bShuttingDown || ProjectId.IsEmpty() || !EnsureClient()) return;
	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	BeginRun(ProjectId, false);
}

void FConvaiCloudProjectsController::BeginRun(const FString& ProjectId, bool bCreated)
{
	Run = FRun();
	Run.ProjectId = ProjectId;
	Run.bCreated = bCreated;

	const FConvaiProject* Project = FindProject(ProjectId);
	Run.Version = FConvaiProjectApiClient::NextVersion(Project ? Project->Versions : TArray<FConvaiProjectVersion>());

	SetJob(EConvaiProjectStage::Prepare, TEXT("Preparing the project"), FString());

	const FConvaiProjectPrepareReport Report = FConvaiProjectPrepare::Run();
	State.PrepareNotes = Report.Changes;
	State.PrepareNotes.Append(Report.Warnings);
	if (!Report.IsSuccess())
	{
		Fail(Report.Error);
		return;
	}
	PackageProject();
}

void FConvaiCloudProjectsController::PackageProject()
{
	if (!Packager) Packager = MakeShared<FConvaiProjectPackager>();

	State.bCanCancel = true;
	SetJob(EConvaiProjectStage::Package, TEXT("Packaging the project"),
		TEXT("Unreal cooks every asset on a first package, which commonly takes 30-45 minutes. "
		     "The editor stays usable and the packaging log opens from Unreal's own notification."));

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Packager->Start(
		[WeakSelf](FString ArchivePath, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || Self->bShuttingDown) return;

			if (!Error.IsEmpty())
			{
				Self->Fail(Error);
				return;
			}
			Self->Run.ArchivePath = ArchivePath;
			Self->ReserveVersion();
		},
		[WeakSelf](EConvaiPackageStage Stage, TOptional<float> Fraction)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || Self->bShuttingDown) return;

			if (Stage == EConvaiPackageStage::Compressing)
			{
				Self->SetJob(EConvaiProjectStage::Compress, TEXT("Compressing the build"),
					TEXT("Packing the staged build into a single archive for upload."), Fraction);
			}
			else
			{
				// Keep the packaging detail line intact: it carries the long-wait explanation.
				Self->SetJob(EConvaiProjectStage::Package, Self->State.JobTitle, Self->State.JobDetail, Fraction);
			}
		});
}

void FConvaiCloudProjectsController::ReserveVersion()
{
	SetJob(EConvaiProjectStage::Upload, FString::Printf(TEXT("Preparing version %s"), *Run.Version), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->ReserveVersion(Run.ProjectId, Run.Version, Run.ArchivePath,
		[WeakSelf](FConvaiProjectReservation Reservation, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || Self->bShuttingDown) return;

			if (!Error.IsEmpty())
			{
				Self->Fail(Error);
				return;
			}
			Self->Run.AssetVersionId = Reservation.AssetVersionId;

			const int64 Total = IFileManager::Get().FileSize(*Self->Run.ArchivePath);
			Self->SetJob(EConvaiProjectStage::Upload, TEXT("Uploading the build"), FormatBytes(Total), 0.f);

			TWeakPtr<FConvaiCloudProjectsController> Inner = Self->AsShared();
			Self->Client->UploadArchive(Reservation, Self->Run.ArchivePath,
				[Inner](int64 Sent, int64 TotalBytes)
				{
					const TSharedPtr<FConvaiCloudProjectsController> Owner = Inner.Pin();
					if (!Owner || Owner->bShuttingDown) return;
					Owner->SetJob(EConvaiProjectStage::Upload, TEXT("Uploading the build"),
						FString::Printf(TEXT("%s of %s"), *FormatBytes(Sent), *FormatBytes(TotalBytes)),
						TotalBytes > 0 ? TOptional<float>(static_cast<float>(Sent) / TotalBytes) : TOptional<float>());
				},
				[Inner](FString, FString UploadError)
				{
					const TSharedPtr<FConvaiCloudProjectsController> Owner = Inner.Pin();
					if (!Owner || Owner->bShuttingDown) return;

					if (!UploadError.IsEmpty())
					{
						Owner->Fail(UploadError);
						return;
					}
					Owner->StartBuild();
				});
		});
}

void FConvaiCloudProjectsController::StartBuild()
{
	// The archive is on storage now; the local copy is just disk cost from here on.
	DiscardArchive();
	SetJob(EConvaiProjectStage::Build, TEXT("Starting the cloud build"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->StartBuild(Run.ProjectId, Run.AssetVersionId, [WeakSelf](FString BuildId, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		if (!Error.IsEmpty())
		{
			// The archive is uploaded; a failed build request must not ask for another upload.
			Self->Fail(FString::Printf(TEXT("%s The upload succeeded, so start the build from the Convai dashboard."), *Error));
			return;
		}
		Self->Run.BuildId = BuildId;
		Self->PollAttempts = 0;
		Self->SetJob(EConvaiProjectStage::Build, TEXT("Building in the cloud"),
			TEXT("This runs on Convai and keeps going even if you close this tab."));
		Self->PollBuild();
	});
}

void FConvaiCloudProjectsController::PollBuild()
{
	StopPolling();
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakSelf](float) -> bool
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || Self->bShuttingDown || Self->Run.BuildId.IsEmpty()) return false;

			if (++Self->PollAttempts > MaxPollAttempts)
			{
				Self->Fail(TEXT("The cloud build is taking longer than expected. Check its status on the Convai dashboard."));
				return false;
			}

			Self->Client->GetBuildStatus(Self->Run.BuildId, [WeakSelf](FString Status, FString Error)
			{
				const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
				if (!Owner || Owner->bShuttingDown) return;

				// A single failed poll is not a failed build; keep waiting for the next tick.
				if (!Error.IsEmpty()) return;

				if (FConvaiProjectApiClient::IsBuildFailed(Status))
				{
					Owner->StopPolling();
					Owner->Fail(TEXT("The cloud build failed. Open the project on the Convai dashboard for its build log."));
					return;
				}
				if (FConvaiProjectApiClient::IsBuildSucceeded(Status))
				{
					Owner->StopPolling();
					Owner->ActivateBuild();
				}
			});
			return true;
		}), PollIntervalSeconds);
}

void FConvaiCloudProjectsController::ActivateBuild()
{
	SetJob(EConvaiProjectStage::Activate, TEXT("Activating the build"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	// Hosting has to exist before a build can be made active; saving defaults first is idempotent.
	Client->SaveHostingDefaults(Run.ProjectId, [WeakSelf](FString, FString HostingError)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		if (!HostingError.IsEmpty())
		{
			Self->Fail(FString::Printf(TEXT("The build is ready but hosting could not be configured: %s"), *HostingError));
			return;
		}

		TWeakPtr<FConvaiCloudProjectsController> Inner = Self->AsShared();
		Self->Client->Activate(Self->Run.ProjectId, Self->Run.BuildId, [Inner](FString, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Owner = Inner.Pin();
			if (!Owner || Owner->bShuttingDown) return;

			if (!Error.IsEmpty())
			{
				Owner->Fail(FString::Printf(TEXT("The build is ready but activation failed: %s"), *Error));
				return;
			}
			Owner->CompleteRun(TEXT("Upload complete. The build is being made active — use Open in browser once it is live."));
		});
	});
}

void FConvaiCloudProjectsController::CompleteRun(const FString& Notice)
{
	StopPolling();
	DiscardArchive();
	Run = FRun();
	State.bBusy = false;
	State.bCanCancel = false;
	ClearJob();
	State.Notice = Notice;
	Publish();
	Refresh();
}

void FConvaiCloudProjectsController::OpenInBrowser(const FString& ProjectId)
{
	if (State.bBusy || bShuttingDown || !EnsureClient()) return;

	const FConvaiProject* Project = FindProject(ProjectId);
	if (!Project) return;

	State.bBusy = true;
	State.Error.Reset();
	SetJob(EConvaiProjectStage::None, TEXT("Opening the stream"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();

	// One experience page per project; reuse the recorded one rather than creating another.
	auto OpenSession = [WeakSelf](const FString& ExperienceId)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		Self->Client->StartStreamSession(ExperienceId, [WeakSelf](FString SessionId, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
			if (!Owner || Owner->bShuttingDown) return;

			Owner->State.bBusy = false;
			Owner->ClearJob();
			if (!Error.IsEmpty())
			{
				Owner->Fail(Error);
				return;
			}
			FPlatformProcess::LaunchURL(*FConvaiProjectApiClient::BuildStreamUrl(SessionId), nullptr, nullptr);
			Owner->State.Notice = TEXT("Opened the stream in your browser.");
			Owner->Publish();
		});
	};

	if (!Project->ExperienceId.IsEmpty())
	{
		OpenSession(Project->ExperienceId);
		return;
	}

	Client->CreateExperience(ProjectId, [WeakSelf, ProjectId, OpenSession](FString ExperienceId, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		if (!Error.IsEmpty())
		{
			Self->State.bBusy = false;
			Self->ClearJob();
			Self->Fail(Error);
			return;
		}
		// Remember it locally so a second Open does not create a duplicate page.
		if (FConvaiProject* Cached = Self->Projects.FindByPredicate(
			[&ProjectId](const FConvaiProject& Item) { return Item.Id == ProjectId; }))
		{
			Cached->ExperienceId = ExperienceId;
		}
		OpenSession(ExperienceId);
	});
}

void FConvaiCloudProjectsController::Cancel()
{
	if (!State.bBusy) return;

	// Only the local stages can be stopped from here. A cloud build keeps running by design.
	if (Packager) Packager->Cancel();
	if (Client) Client->CancelAll();
	Client.Reset();
	StopPolling();
	DiscardArchive();

	Run = FRun();
	State.bBusy = false;
	State.bCanCancel = false;
	ClearJob();
	State.Notice = TEXT("Upload cancelled.");
	Publish();
}

void FConvaiCloudProjectsController::Fail(const FString& Error)
{
	StopPolling();
	DiscardArchive();
	Run = FRun();
	State.bBusy = false;
	State.bCanCancel = false;
	ClearJob();
	State.Error = Error;
	Publish();
}

void FConvaiCloudProjectsController::SetJob(EConvaiProjectStage Stage, const FString& Title,
	const FString& Detail, TOptional<float> Progress)
{
	const double Now = FPlatformTime::Seconds();
	if (State.JobStartedAt <= 0.0) State.JobStartedAt = Now;
	if (State.Stage != Stage)
	{
		State.Stage = Stage;
		State.StageStartedAt = Now;
	}
	State.JobTitle = Title;
	State.JobDetail = Detail;
	State.JobProgress = Progress;
	Publish();
}

void FConvaiCloudProjectsController::ClearJob()
{
	State.JobTitle.Reset();
	State.JobDetail.Reset();
	State.JobProgress.Reset();
	State.Stage = EConvaiProjectStage::None;
	State.JobStartedAt = 0.0;
	State.StageStartedAt = 0.0;
}

void FConvaiCloudProjectsController::StopPolling()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
}

void FConvaiCloudProjectsController::DiscardArchive()
{
	if (Run.ArchivePath.IsEmpty()) return;
	IFileManager::Get().Delete(*Run.ArchivePath, false, true, true);
	Run.ArchivePath.Reset();
}

#undef LOCTEXT_NAMESPACE
