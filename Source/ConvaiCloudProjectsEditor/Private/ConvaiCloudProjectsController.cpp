// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiCloudProjectsController.h"
#include "ConvaiCloudProjectsStatus.h"

#include "Packaging/ConvaiProjectPrepare.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/IAuthWindowManager.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"

#define LOCTEXT_NAMESPACE "ConvaiCloudProjects"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiCloudProjects, Log, All);

namespace
{
/** Build polling: every 10s for up to an hour. A cook that overruns that is a dashboard problem. */
constexpr float PollIntervalSeconds = 10.f;
constexpr int32 MaxPollAttempts = 360;

/**
 * The waits before each repeat of a failed upload. The failure carries no status, so every one is retried:
 * a permanent rejection costs two extra attempts.
 */
constexpr float UploadRetryDelaysSeconds[] = { 10.f, 60.f };
constexpr int32 MaxUploadAttempts = static_cast<int32>(UE_ARRAY_COUNT(UploadRetryDelaysSeconds)) + 1;

/** Upload progress arrives on every HTTP tick that moved bytes, compression once per zipped file. */
constexpr double ProgressPublishIntervalSeconds = 0.25;

using ConvaiCloudProjectsStatus::AvailabilityFor;
using ConvaiCloudProjectsStatus::IsRolloutFailed;
using ConvaiCloudProjectsStatus::IsRolloutPending;
using ConvaiCloudProjectsStatus::StatusFor;

/** The project's own content: /Game, or a plugin that lives with the project rather than the engine. */
bool IsProjectPackage(const FString& PackageName)
{
	const FString Root = FPackageName::GetPackageMountPoint(PackageName).ToString();
	if (Root == TEXT("Game")) return true;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Root);
	return Plugin.IsValid() && Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project;
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

	// Bound through the controller's shared pointer: the tab can outlive module shutdown, and an event
	// from it then finds the controller gone instead of calling into freed memory.
	TSharedRef<SConvaiCloudProjects> New = SNew(SConvaiCloudProjects)
		.OnRefresh(this, &FConvaiCloudProjectsController::Refresh)
		.OnSignIn(this, &FConvaiCloudProjectsController::SignIn)
		.OnCancelJob(this, &FConvaiCloudProjectsController::Cancel)
		.OnSelect(this, &FConvaiCloudProjectsController::Select)
		.OnCreate(this, &FConvaiCloudProjectsController::Create)
		.OnUploadChanges(this, &FConvaiCloudProjectsController::UploadChanges)
		.OnOpenInBrowser(this, &FConvaiCloudProjectsController::OpenInBrowser)
		.OnActivateVersion(this, &FConvaiCloudProjectsController::ActivateVersion)
		.OnShowLogs(this, &FConvaiCloudProjectsController::ShowLogs)
		.OnPublish(this, &FConvaiCloudProjectsController::PublishPage)
		.OnSaveSettings(this, &FConvaiCloudProjectsController::SaveSettings);

	Widget = New;
	Publish();
	Refresh();
	return New;
}

void FConvaiCloudProjectsController::Shutdown()
{
	bShuttingDown = true;
	StopPolling();
	StopUploadRetry();
	if (Packager) Packager->Cancel();
	if (Client) Client->CancelAll();
	Packager.Reset();
	Client.Reset();
	Widget.Reset();
}

void FConvaiCloudProjectsController::Publish()
{
#if WITH_DEV_AUTOMATION_TESTS
	++PublishCount;
#endif
	if (Widget.IsValid() && !bShuttingDown) Widget->SetState(State);
}

bool FConvaiCloudProjectsController::EnsureClient()
{
	if (Client) return true;

	FString Error;
	Client = MakeClient(Error);
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

bool FConvaiCloudProjectsController::RequireClient()
{
	if (Client) return true;
	Fail(TEXT("Sign in to Convai to manage your uploaded projects."));
	return false;
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

	// A sign-out between refreshes has to retire the old client, not reuse its credentials. Work in
	// flight keeps the client it started with, since releasing the client drops that work's
	// completions; a sign-out takes effect at the first Refresh once it is done (CompleteRun's own).
	if (!State.bBusy)
	{
		Client.Reset();
		// Releasing the client drops these requests' completions, the only places that clear their flags.
		State.Publish.bLoading = false;
		State.bLogsLoading = false;
	}
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
			// Only a banner: a run or request still in flight outlives a failed list.
			Self->State.Error = Error;
			Self->Publish();
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
		if (!Project.Versions.IsEmpty()) Card.Version = Project.Versions[0].Version;
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
	if (LogProjectId != State.SelectedId)
	{
		State.LogVersion.Reset();
		State.LogLines.Reset();
		State.bLogsLoading = false;
		LogProjectId = State.SelectedId;
		LogBuildId.Reset();
	}
	// So does the page: another project's must never stand in while this one's loads.
	if (PublishProjectId != State.SelectedId)
	{
		State.Publish = FConvaiProjectPublishState();
		PublishProjectId = State.SelectedId;
	}

	const FConvaiProject* Project = FindProject(State.SelectedId);
	if (!Project)
	{
		State.Publish = FConvaiProjectPublishState();
		Settings = FConvaiProjectHostingSettings();
		State.Settings = Settings;
		return;
	}

	// Hosting and runtime come straight off the record already in hand; no extra request.
	Settings = FConvaiProjectHostingSettings::Read(Project->Metadata);
	State.Settings = Settings;

	// Already newest first from the API; render in the order received.
	for (const FConvaiProjectVersion& Version : Project->Versions)
	{

		FConvaiProjectVersionRow Row;
		Row.Version = Version.Version;
		Row.BuildId = Version.JobId;
		Row.Size = Version.SizeBytes > 0 ? FormatBytes(Version.SizeBytes) : FString();
		Row.Availability = AvailabilityFor(*Project, Version.JobId);
		Row.bActive = Row.Availability == TEXT("Active");
		Row.bHasLogs = !Version.JobId.IsEmpty();

		const bool bBuilt = FConvaiProjectApiClient::IsBuildSucceeded(Version.JobStatus);
		if (bBuilt) Row.Status = TEXT("Built");
		else if (FConvaiProjectApiClient::IsBuildFailed(Version.JobStatus)) Row.Status = TEXT("Build failed");
		else if (FConvaiProjectApiClient::IsBuildInProgress(Version.JobStatus)) Row.Status = TEXT("Building");
		else Row.Status = Version.JobId.IsEmpty() ? TEXT("Uploaded") : Version.JobStatus;

		// A rollout in flight can be superseded by pointing at a different build -- including the
		// one currently serving, which is how an unwanted rollout is called off.
		const bool bRolloutPending = IsRolloutPending(Project->DeploymentStatus) || IsRolloutFailed(Project->DeploymentStatus);
		const bool bSupersedes = bRolloutPending && !Version.JobId.IsEmpty() && Version.JobId != Project->DesiredBuildId;
		const bool bKeepsCurrentActive = bSupersedes && Version.JobId == Project->ActiveBuildId;

		Row.bCanActivate = bBuilt && !Row.BuildId.IsEmpty() && (!Row.bActive || bKeepsCurrentActive);
		Row.bActivateEnabled = !bRolloutPending || bSupersedes;
		Row.ActivateLabel = bKeepsCurrentActive
			? TEXT("Cancel rollout and keep active")
			: (Row.Availability == TEXT("Activation failed") ? TEXT("Retry activation")
				: (Row.Availability == TEXT("Preparing") || Row.Availability == TEXT("Waiting to start")
					? Row.Availability : TEXT("Make active")));

		State.Versions.Add(MoveTemp(Row));
	}

	// The page's name and audience come from the experience API, not the application record.
	if (bShuttingDown) return;
	// No client means signed out since the last list: Refresh asks to sign in again, or, after a sign-in, reads the
	// list anew, since the records shown may be another account's. Its list re-selects, which asks for the page.
	if (!Client)
	{
		Refresh();
		return;
	}
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
	const FConvaiProjectHostingSettings AtClick = Settings;
	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	SetJob(EConvaiProjectStage::Activate, TEXT("Activating the version"), FString());

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	// Settings and activation travel in one request. The settings sent are the project's own, read
	// fresh, so activating from Unreal never overwrites a configuration made on the dashboard.
	Client->Get(ProjectId, [WeakSelf, ProjectId, BuildId, AtClick](FConvaiProject Fresh, FString ReadError)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		// A failed read must not block the activation, and a record without metadata would send defaults over the
		// project's configuration; either way, fall back to what the selection showed at the click.
		const FConvaiProjectHostingSettings Current = ReadError.IsEmpty() && Fresh.Metadata.IsValid()
			? FConvaiProjectHostingSettings::Read(Fresh.Metadata) : AtClick;
		Self->Client->SaveSettings(ProjectId, Current, BuildId, [WeakSelf](FString, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
			if (!Owner || Owner->bShuttingDown) return;

			if (!Error.IsEmpty()) { Owner->Fail(Error); return; }
			Owner->CompleteRun(TEXT("That version is being made active."));
		});
	});
}

void FConvaiCloudProjectsController::SaveSettings(const FConvaiProjectHostingSettings& InSettings, const FString& BuildId)
{
	if (State.bBusy || bShuttingDown || State.SelectedId.IsEmpty() || !EnsureClient()) return;

	const TMap<FString, FString> Errors = InSettings.Validate();
	if (!Errors.IsEmpty())
	{
		// Report the first problem; the form marks every offending field itself.
		Fail(FString::Printf(TEXT("Settings were not saved: %s"), *Errors.CreateConstIterator()->Value));
		return;
	}

	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	SetJob(EConvaiProjectStage::None,
		BuildId.IsEmpty() ? TEXT("Saving settings") : TEXT("Saving settings and activating"), FString());

	// Remember the revision we are starting from, so the result can say whether a rollout
	// was actually queued. The server accepts an unchanged config without rolling anything.
	const FString ProjectId = State.SelectedId;
	const FConvaiProject* Project = FindProject(ProjectId);
	const int32 PreviousRevision = Project ? Project->RequestRevision : -1;

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->SaveSettings(ProjectId, InSettings, BuildId,
		[WeakSelf, ProjectId, InSettings, BuildId, PreviousRevision](FString Revision, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || Self->bShuttingDown) return;

		if (!Error.IsEmpty()) { Self->Fail(Error); return; }
		// Only a confirmed save replaces the server's copy, which a later Make active falls back to;
		// a rejected draft stays in the form. A save for a project no longer shown changes nothing here.
		if (Self->State.SelectedId == ProjectId)
		{
			Self->Settings = InSettings;
			Self->State.Settings = InSettings;
		}

		if (BuildId.IsEmpty())
		{
			Self->CompleteRun(TEXT("Settings saved. They apply at the next deployment."));
			return;
		}
		const bool bQueued = !Revision.IsEmpty() && FCString::Atoi(*Revision) > PreviousRevision;
		Self->CompleteRun(bQueued
			? TEXT("Settings saved and a redeploy was queued.")
			: TEXT("Settings saved, but nothing changed, so no redeploy was needed."));
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
	LogProjectId = State.SelectedId;
	LogBuildId = BuildId;
	Publish();

	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->GetBuildReport(BuildId, [WeakSelf, BuildId](FConvaiBuildReport Report, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		// An earlier press, or a log a project switch has cleared since, would overwrite what the view shows.
		if (!Self || Self->bShuttingDown || Self->LogBuildId != BuildId) return;

		Self->State.bLogsLoading = false;
		// Report the failure inside the log view; it is the thing the user just opened.
		Self->State.LogLines = Error.IsEmpty() ? Report.Logs : TArray<FString>{ Error };
		Self->Publish();
	});
}

void FConvaiCloudProjectsController::PublishPage(const FConvaiProjectPublishState& Page, bool bPublish)
{
	// Only the selected project's page, once loaded: the request carries State.Publish's id, not the form's.
	if (State.bBusy || bShuttingDown || PublishProjectId != State.SelectedId || State.Publish.bLoading || !EnsureClient()) return;
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
	// Saved before the project is created, so a failed save cannot leave an empty one behind.
	if (State.bBusy || bShuttingDown || !EnsureClient() || !SaveBeforeCook()) return;

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

		// Show the new project in the list and select it before the long run starts. Without this the
		// packaging progress appears against an empty selection and the project itself stays missing
		// until the user presses Refresh, which reads it back from the server.
		Self->State.SelectedId = Created.Id;
		Self->RebuildCards(TArray<FConvaiProject>(Self->Projects));

		Self->BeginRun(Created.Id, true);
	});
}

void FConvaiCloudProjectsController::UploadChanges(const FString& ProjectId)
{
	if (State.bBusy || bShuttingDown || ProjectId.IsEmpty() || !EnsureClient() || !SaveBeforeCook()) return;
	State.bBusy = true;
	State.Error.Reset();
	State.Notice.Reset();
	BeginRun(ProjectId, false);
}

bool FConvaiCloudProjectsController::SaveBeforeCook()
{
	// Both callers run from a button click on the game thread, where the save's dialogs are safe.
	const bool bSaved = SaveDirtyPackages();
	// The save's true does not mean everything was written, so what it left dirty decides. Only the project's own
	// packages can: the cook reads engine and other read-only content from disk however it was edited here.
	TArray<FString> Unsaved;
	TArray<FString> Elsewhere;
	for (FString& Name : GetUnsavedPackages()) (IsProjectPackage(Name) ? Unsaved : Elsewhere).Add(MoveTemp(Name));
	if (bSaved && Unsaved.IsEmpty())
	{
		if (!Elsewhere.IsEmpty())
		{
			UE_LOG(LogConvaiCloudProjects, Warning, TEXT("Uploading without %d unsaved change(s) outside this project; ")
				TEXT("the build is cooked from these as they are on disk: %s"), Elsewhere.Num(), *FString::Join(Elsewhere, TEXT(", ")));
		}
		return true;
	}

	FString Error = TEXT("Unsaved changes could not be saved, so the upload did not start. ");
	if (!Unsaved.IsEmpty())
	{
		constexpr int32 NamesShown = 3;
		FString Names = FString::Join(TArrayView<const FString>(Unsaved).Left(NamesShown), TEXT(", "));
		if (Unsaved.Num() > NamesShown) Names += FString::Printf(TEXT(" and %d more"), Unsaved.Num() - NamesShown);
		Error += FString::Printf(TEXT("Still unsaved (%d): %s. "), Unsaved.Num(), *Names);
	}
	Error += TEXT("The build is cooked from the files on disk: save everything with File > Save All, then try again.");
	Fail(Error);
	return false;
}

void FConvaiCloudProjectsController::BeginRun(const FString& ProjectId, bool bCreated)
{
	++RunGeneration;
	Run = FRun();
	Run.ProjectId = ProjectId;
	Run.bCreated = bCreated;

	const FConvaiProject* Project = FindProject(ProjectId);
	if (Project) Run.KnownVersions = Project->Versions;
	Run.Version = FConvaiProjectApiClient::NextVersion(Run.KnownVersions);
	// Read from the record, not copied from Settings: that follows the selection, which can move
	// mid-run, and shares its FrontendConfig with the settings form.
	Run.Settings = FConvaiProjectHostingSettings::Read(Project ? Project->Metadata : nullptr);

	SetJob(EConvaiProjectStage::Prepare, TEXT("Preparing the project"), FString());

	const FConvaiProjectPrepareReport Report = PrepareProject();
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
	if (!Packager) Packager = MakeShared<FConvaiPackagerStep>();

	State.bCanCancel = true;
	SetJob(EConvaiProjectStage::Package, TEXT("Packaging the project"),
		TEXT("Unreal cooks every asset on a first package, which commonly takes 30-45 minutes. "
		     "The editor stays usable; the packaging log is the Output Log's LogConvaiCloudPackaging lines."));

	const uint64 Generation = RunGeneration;
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Packager->Start(
		[WeakSelf, Generation](FString ArchivePath, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || !Self->IsCurrentRun(Generation)) return;

			if (!Error.IsEmpty())
			{
				Self->Fail(Error);
				return;
			}
			Self->Run.ArchivePath = ArchivePath;
			// Checked before the reservation, so no version is reserved for an archive that cannot arrive.
			const int64 ArchiveBytes = IFileManager::Get().FileSize(*ArchivePath);
			if (!IsUploadableSize(ArchiveBytes))
			{
				Self->Fail(FString::Printf(TEXT("The archive is %s, but an upload from Windows must be smaller than 2 GB, ")
					TEXT("so nothing was uploaded. Reduce the packaged size and upload again."), *FormatBytes(ArchiveBytes)));
				return;
			}
			Self->ReserveVersion();
		},
		[WeakSelf, Generation](EConvaiPackageStage Stage, TOptional<float> Fraction)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || !Self->IsCurrentRun(Generation)) return;

			if (Stage == EConvaiPackageStage::Compressing)
			{
				Self->SetJobProgress(EConvaiProjectStage::Compress, TEXT("Compressing the build"),
					TEXT("Packing the staged build into a single archive for upload."), Fraction);
			}
			else
			{
				// Keep the packaging detail line intact: it carries the long-wait explanation.
				Self->SetJobProgress(EConvaiProjectStage::Package, Self->State.JobTitle, Self->State.JobDetail, Fraction);
			}
		});
}

void FConvaiCloudProjectsController::ReserveVersion()
{
	if (!RequireClient()) return;
	SetJob(EConvaiProjectStage::Upload, FString::Printf(TEXT("Preparing version %s"), *Run.Version), FString());

	const uint64 Generation = RunGeneration;
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	// The version was picked before a cook that can take most of an hour; a teammate may have uploaded since.
	Client->Get(Run.ProjectId, [WeakSelf, Generation](FConvaiProject Fresh, FString ReadError)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || !Self->IsCurrentRun(Generation) || !Self->RequireClient()) return;

		// A failed read keeps the version picked when the run began, and a record that lists fewer versions never lowers it.
		if (ReadError.IsEmpty())
		{
			Self->Run.KnownVersions.Append(Fresh.Versions);
			Self->Run.Version = FConvaiProjectApiClient::NextVersion(Self->Run.KnownVersions);
		}
		Self->SetJob(EConvaiProjectStage::Upload, FString::Printf(TEXT("Preparing version %s"), *Self->Run.Version), FString());
		Self->Client->ReserveVersion(Self->Run.ProjectId, Self->Run.Version, Self->Run.ArchivePath,
			[WeakSelf, Generation](FConvaiProjectReservation Reservation, FString Error)
			{
				const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
				if (!Owner || !Owner->IsCurrentRun(Generation)) return;

				if (!Error.IsEmpty())
				{
					Owner->Fail(Error);
					return;
				}
				Owner->Run.Reservation = MoveTemp(Reservation);
				Owner->UploadArchive();
			});
	});
}

void FConvaiCloudProjectsController::UploadArchive()
{
	if (!RequireClient()) return;
	const int32 Attempt = ++Run.UploadAttempts;
	const FString AttemptNote = Attempt > 1 ? FString::Printf(TEXT(", attempt %d of %d"), Attempt, MaxUploadAttempts) : FString();
	const int64 Total = IFileManager::Get().FileSize(*Run.ArchivePath);
	SetJob(EConvaiProjectStage::Upload, TEXT("Uploading the build"), FormatBytes(Total) + AttemptNote, 0.f);

	const uint64 Generation = RunGeneration;
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->UploadArchive(Run.Reservation, Run.ArchivePath,
		[WeakSelf, Generation, AttemptNote](int64 Sent, int64 TotalBytes)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || !Self->IsCurrentRun(Generation)) return;
			Self->SetJobProgress(EConvaiProjectStage::Upload, TEXT("Uploading the build"),
				FString::Printf(TEXT("%s of %s%s"), *FormatBytes(Sent), *FormatBytes(TotalBytes), *AttemptNote),
				TotalBytes > 0 ? TOptional<float>(static_cast<float>(Sent) / TotalBytes) : TOptional<float>());
		},
		[WeakSelf, Generation, Attempt](FString, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
			if (!Self || !Self->IsCurrentRun(Generation)) return;

			if (Error.IsEmpty())
			{
				Self->StartBuild();
				return;
			}
			UE_LOG(LogConvaiCloudProjects, Warning, TEXT("Upload attempt %d of %d failed: %s"), Attempt, MaxUploadAttempts, *Error);
			if (Attempt >= MaxUploadAttempts)
			{
				Self->Fail(Error);
				return;
			}
			const float Delay = UploadRetryDelaysSeconds[Attempt - 1];
			Self->SetJob(EConvaiProjectStage::Upload, TEXT("Uploading the build"),
				FString::Printf(TEXT("Attempt %d of %d failed: %s Trying again in %.0f s."), Attempt, MaxUploadAttempts, *Error, Delay));
			Self->RetryHandle = Self->RetryTicker->AddTicker(FTickerDelegate::CreateLambda([WeakSelf, Generation](float)
			{
				if (const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin()) Owner->UploadRetryTick(Generation);
				return false;
			}), Delay);
		});
}

void FConvaiCloudProjectsController::UploadRetryTick(uint64 Generation)
{
	if (IsCurrentRun(Generation)) UploadArchive();
}

void FConvaiCloudProjectsController::StartBuild()
{
	if (!RequireClient()) return;
	// The archive is on storage now; the local copy is just disk cost from here on.
	DiscardArchive();
	SetJob(EConvaiProjectStage::Build, TEXT("Starting the cloud build"), FString());

	const uint64 Generation = RunGeneration;
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->StartBuild(Run.ProjectId, Run.Reservation.AssetVersionId, [WeakSelf, Generation](FString BuildId, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || !Self->IsCurrentRun(Generation)) return;

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
			return Self && Self->PollTick();
		}), PollIntervalSeconds);
}

bool FConvaiCloudProjectsController::PollTick()
{
	if (bShuttingDown || Run.BuildId.IsEmpty()) return false;

	// Counted before the in-flight check, so the budget stays an hour of wall time however slow polls get.
	if (++PollAttempts > MaxPollAttempts)
	{
		Fail(TEXT("The cloud build is taking longer than expected. Check its status on the Convai dashboard."));
		return false;
	}
	if (Run.bPollInFlight) return true;
	if (!RequireClient()) return false;

	Run.bPollInFlight = true;
	const uint64 Generation = RunGeneration;
	const FString BuildId = Run.BuildId;
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	Client->GetBuildStatus(BuildId, [WeakSelf, Generation, BuildId](FString Status, FString Error)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
		if (!Owner || !Owner->IsCurrentRun(Generation) || Owner->Run.BuildId != BuildId) return;
		Owner->Run.bPollInFlight = false;

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
}

void FConvaiCloudProjectsController::ActivateBuild()
{
	if (Run.bActivating || !RequireClient()) return;
	Run.bActivating = true;
	SetJob(EConvaiProjectStage::Activate, TEXT("Activating the build"), FString());

	const uint64 Generation = RunGeneration;
	const FString ProjectId = Run.ProjectId;
	const FString BuildId = Run.BuildId;
	TWeakPtr<FConvaiCloudProjectsController> WeakSelf = AsShared();
	// Hosting must exist before a build can serve. Sending the project's current settings back
	// alongside the activation provisions capacity on a first upload without discarding anything
	// an existing project already had configured, here or on the dashboard during the run.
	Client->Get(ProjectId, [WeakSelf, Generation, ProjectId, BuildId](FConvaiProject Fresh, FString ReadError)
	{
		const TSharedPtr<FConvaiCloudProjectsController> Self = WeakSelf.Pin();
		if (!Self || !Self->IsCurrentRun(Generation)) return;

		// A failed read must not block a finished build, and a record without metadata would send defaults over the
		// project's configuration; the run's own snapshot is still this project's.
		const FConvaiProjectHostingSettings Current = ReadError.IsEmpty() && Fresh.Metadata.IsValid()
			? FConvaiProjectHostingSettings::Read(Fresh.Metadata) : Self->Run.Settings;
		Self->Client->SaveSettings(ProjectId, Current, BuildId, [WeakSelf, Generation](FString, FString Error)
		{
			const TSharedPtr<FConvaiCloudProjectsController> Owner = WeakSelf.Pin();
			if (!Owner || !Owner->IsCurrentRun(Generation)) return;

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
	StopUploadRetry();
	DiscardArchive();
	Run = FRun();
	++RunGeneration;
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
		if (!Self || Self->bShuttingDown || !Self->RequireClient()) return;

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
			Owner->LaunchBrowser(Owner->Client->BuildStreamUrl(SessionId));
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

	// Ended before the packager hears of it: during Package it completes inside its own Cancel.
	++RunGeneration;
	// Only the local stages can be stopped from here. A cloud build keeps running by design.
	if (Packager) Packager->Cancel();
	if (Client) Client->CancelAll();
	// CancelAll also drops these requests' completions, the only places that clear their flags.
	State.bLoading = false;
	State.bLogsLoading = false;
	State.Publish.bLoading = false;
	StopPolling();
	StopUploadRetry();
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
	StopUploadRetry();
	DiscardArchive();
	Run = FRun();
	++RunGeneration;
	State.bBusy = false;
	State.bCanCancel = false;
	ClearJob();
	State.Error = Error;
	Publish();
}

bool FConvaiCloudProjectsController::IsUploadableSize(int64 ArchiveBytes)
{
#if PLATFORM_WINDOWS
	// The engine's curl PUT passes the size as CURLOPT_INFILESIZE, a 32-bit long on Win64: from 2 GiB the body
	// goes out chunked beside a full Content-Length, or cut to the size modulo 4 GiB.
	return ArchiveBytes < (int64(1) << 31);
#else
	return true;
#endif
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

void FConvaiCloudProjectsController::SetJobProgress(EConvaiProjectStage Stage, const FString& Title,
	const FString& Detail, TOptional<float> Progress)
{
	const double Now = FPlatformTime::Seconds();
	if (Stage != State.Stage || Now - LastProgressPublishAt >= ProgressPublishIntervalSeconds)
	{
		LastProgressPublishAt = Now;
		SetJob(Stage, Title, Detail, Progress);
		return;
	}
	State.JobTitle = Title;
	State.JobDetail = Detail;
	State.JobProgress = Progress;
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

void FConvaiCloudProjectsController::StopUploadRetry()
{
	FTSTicker::RemoveTicker(RetryHandle);
	RetryHandle.Reset();
}

void FConvaiCloudProjectsController::DiscardArchive()
{
	if (Run.ArchivePath.IsEmpty()) return;
	IFileManager::Get().Delete(*Run.ArchivePath, false, true, true);
	Run.ArchivePath.Reset();
}

#undef LOCTEXT_NAMESPACE
