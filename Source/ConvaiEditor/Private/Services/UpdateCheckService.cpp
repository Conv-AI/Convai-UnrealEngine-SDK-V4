/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * UpdateCheckService.cpp
 *
 * Implementation of the update check service.
 */

#include "Services/UpdateCheckService.h"
#include "ConvaiEditor.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"
#include "Misc/ConfigCacheIni.h"
#include "Async/HttpAsyncOperation.h"

FUpdateCheckService::FUpdateCheckService()
	: Config(FUpdateCheckConfig::Default()), bCheckInProgress(false), bUsingLatestEndpoint(true)
{
	bIsShuttingDown.store(false, std::memory_order_relaxed);
	CurrentVersionCache = LoadCurrentVersion();
	LoadAcknowledgedState();
}

FUpdateCheckService::FUpdateCheckService(const FUpdateCheckConfig &InConfig)
	: Config(InConfig), bCheckInProgress(false), bUsingLatestEndpoint(true)
{
	bIsShuttingDown.store(false, std::memory_order_relaxed);
	CurrentVersionCache = LoadCurrentVersion();
	LoadAcknowledgedState();
}

FUpdateCheckService::~FUpdateCheckService()
{
	Shutdown();
}

void FUpdateCheckService::Startup()
{
	if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
	{
		return;
	}

	ConvaiEditor::FCircuitBreakerConfig CircuitConfig;
	CircuitConfig.Name = TEXT("GitHubAPI");
	CircuitConfig.FailureThreshold = 3;
	CircuitConfig.SuccessThreshold = 2;
	CircuitConfig.OpenTimeoutSeconds = 60.0f;
	CircuitConfig.bEnableLogging = true;
	CircuitBreaker = MakeUnique<ConvaiEditor::FCircuitBreaker>(CircuitConfig);

	ConvaiEditor::FRetryPolicyConfig RetryConfig;
	RetryConfig.Name = TEXT("GitHubAPI");
	RetryConfig.MaxAttempts = 3;
	RetryConfig.BaseDelaySeconds = 2.0f;
	RetryConfig.MaxDelaySeconds = 30.0f;
	RetryConfig.Strategy = ConvaiEditor::ERetryStrategy::Exponential;
	RetryConfig.bEnableJitter = true;
	RetryConfig.bEnableLogging = true;
	RetryConfig.ShouldRetryPredicate = ConvaiEditor::RetryPredicates::OnlyTransientErrors;
	RetryPolicy = MakeUnique<ConvaiEditor::FRetryPolicy>(RetryConfig);

	if (Config.bAutoCheckOnStartup)
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
				  {
			FTimerHandle TimerHandle;
			GEditor->GetTimerManager()->SetTimer(
				TimerHandle,
				[this]()
				{
					CheckForUpdatesAsync(false);
				},
				2.0f,
				false); });
	}
}

void FUpdateCheckService::Shutdown()
{
	UE_LOG(LogConvaiEditor, Log, TEXT("UpdateCheckService: Shutting down..."));

	bIsShuttingDown.store(true, std::memory_order_seq_cst);

	TArray<TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>>> OperationsCopy;
	{
		FScopeLock Lock(&CacheLock);
		OperationsCopy = ActiveOperations;
		ActiveOperations.Empty();
	}

	for (TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> &Op : OperationsCopy)
	{
		if (Op.IsValid())
		{
			Op->Cancel();
		}
	}

	UpdateCheckCompleteDelegate.Clear();
	UpdateAvailabilityChangedDelegate.Clear();

	CircuitBreaker.Reset();
	RetryPolicy.Reset();

	UE_LOG(LogConvaiEditor, Log, TEXT("UpdateCheckService: Shutdown complete"));
}

TFuture<FUpdateCheckResult> FUpdateCheckService::CheckForUpdatesAsync(bool bForceRefresh)
{
	FScopeLock Lock(&CacheLock);

	if (bIsShuttingDown.load(std::memory_order_acquire))
	{
		FUpdateCheckResult ShutdownResult = FUpdateCheckResult::Error(
			EUpdateCheckStatus::NetworkError,
			TEXT("Service is shutting down"));
		return Async(EAsyncExecution::TaskGraphMainThread, [ShutdownResult]()
					 { return ShutdownResult; });
	}

	if (!bForceRefresh && IsCacheValid())
	{
		return Async(EAsyncExecution::TaskGraphMainThread, [this]()
					 { return CachedResult; });
	}

	if (bCheckInProgress)
	{
		FUpdateCheckResult InProgressResult;
		InProgressResult.Status = EUpdateCheckStatus::InProgress;
		InProgressResult.CurrentVersion = CurrentVersionCache;

		return Async(EAsyncExecution::TaskGraphMainThread, [InProgressResult]()
					 { return InProgressResult; });
	}

	if (!FHttpModule::Get().IsHttpEnabled())
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService HTTP error: module not enabled"));

		FUpdateCheckResult ErrorResult = FUpdateCheckResult::Error(
			EUpdateCheckStatus::NetworkError,
			TEXT("HTTP module not available"));

		return Async(EAsyncExecution::TaskGraphMainThread, [ErrorResult]()
					 { return ErrorResult; });
	}

	if (CircuitBreaker && CircuitBreaker->IsOpen())
	{
		UE_LOG(LogConvaiEditor, Warning, TEXT("UpdateCheckService: GitHub API temporarily unavailable - circuit breaker open"));

		FUpdateCheckResult ErrorResult = FUpdateCheckResult::Error(
			EUpdateCheckStatus::NetworkError,
			TEXT("GitHub API circuit breaker is open - service temporarily unavailable"));

		return Async(EAsyncExecution::TaskGraphMainThread, [ErrorResult]()
					 { return ErrorResult; });
	}

	bCheckInProgress = true;

	TSharedPtr<TPromise<FUpdateCheckResult>> Promise = MakeShared<TPromise<FUpdateCheckResult>>();
	TFuture<FUpdateCheckResult> Future = Promise->GetFuture();

	PerformUpdateCheck(Promise, 1);

	return Future;
}

void FUpdateCheckService::PerformUpdateCheck(
	TSharedPtr<TPromise<FUpdateCheckResult>> Promise,
	int32 AttemptNumber)
{
	if (!Promise.IsValid())
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: invalid promise in PerformUpdateCheck"));
		return;
	}

	if (bIsShuttingDown.load(std::memory_order_acquire))
	{
		UE_LOG(LogConvaiEditor, Warning, TEXT("UpdateCheckService: skipping update check - service shutting down"));
		{
			FScopeLock Lock(&CacheLock);
			bCheckInProgress = false;
		}
		return;
	}

	FString RequestUrl;
	if (AttemptNumber == 1)
	{
		bUsingLatestEndpoint = true;
		RequestUrl = Config.GitHubLatestApiUrl;
	}
	else
	{
		bUsingLatestEndpoint = false;
		RequestUrl = Config.GitHubAllReleasesApiUrl;
	}

	ConvaiEditor::FHttpAsyncRequest HttpRequest(RequestUrl);
	HttpRequest.WithVerb(TEXT("GET"))
		.WithHeader(TEXT("Accept"), TEXT("application/vnd.github.v3+json"))
		.WithHeader(TEXT("User-Agent"), TEXT("Convai-UnrealEngine-SDK"))
		.WithTimeout(Config.TimeoutSeconds);

	TSharedPtr<ConvaiEditor::FAsyncOperation<ConvaiEditor::FHttpAsyncResponse>> AsyncOp;

	if (CircuitBreaker.IsValid() && RetryPolicy.IsValid())
	{
		TSharedPtr<ConvaiEditor::FCircuitBreaker> SharedCircuitBreaker(CircuitBreaker.Get(), [](ConvaiEditor::FCircuitBreaker *) {});
		TSharedPtr<ConvaiEditor::FRetryPolicy> SharedRetryPolicy(RetryPolicy.Get(), [](ConvaiEditor::FRetryPolicy *) {});

		AsyncOp = ConvaiEditor::FHttpAsyncOperation::CreateWithProtection(
			HttpRequest,
			SharedCircuitBreaker,
			SharedRetryPolicy,
			nullptr);
	}
	else
	{
		AsyncOp = ConvaiEditor::FHttpAsyncOperation::Create(HttpRequest, nullptr);
	}

	// Track active operation for cancellation during shutdown
	{
		FScopeLock Lock(&CacheLock);
		ActiveOperations.Add(AsyncOp);
	}

	// CRITICAL: Use WeakPtr to prevent dangling 'this' if service destroyed before callback
	TWeakPtr<FUpdateCheckService> WeakSelf = AsShared();
	AsyncOp->OnComplete([WeakSelf, Promise, AttemptNumber, AsyncOp](const TConvaiResult<ConvaiEditor::FHttpAsyncResponse> &Result)
						{
		TSharedPtr<FUpdateCheckService> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		
		{
			FScopeLock Lock(&Self->CacheLock);
			Self->ActiveOperations.Remove(AsyncOp);
		}
		
		if (Self->bIsShuttingDown.load(std::memory_order_acquire))
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("UpdateCheckService: ignoring response - service shutting down"));
			{
				FScopeLock Lock(&Self->CacheLock);
				Self->bCheckInProgress = false;
			}
			return;
		}

		if (!Promise.IsValid())
		{
			UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: invalid promise in completion handler"));
			return;
		}

		if (!Result.IsSuccess())
		{
			UE_LOG(LogConvaiEditor, Warning, TEXT("UpdateCheckService HTTP request failed: %s"), *Result.GetError());

			if (Self->bUsingLatestEndpoint && AttemptNumber == 1)
			{
				UE_LOG(LogConvaiEditor, Warning, TEXT("UpdateCheckService: GitHub /latest endpoint failed, falling back to /releases"));
				Self->PerformUpdateCheck(Promise, AttemptNumber + 1);
				return;
			}

			FScopeLock Lock(&Self->CacheLock);
			Self->bCheckInProgress = false;

			FUpdateCheckResult ErrorResult = FUpdateCheckResult::Error(
				EUpdateCheckStatus::NetworkError,
				Result.GetError());

			Self->UpdateCache(ErrorResult);
			Promise->SetValue(ErrorResult);
			Self->NotifyDelegates(ErrorResult);
			return;
		}

		const ConvaiEditor::FHttpAsyncResponse &HttpResponse = Result.GetValue();

		if (HttpResponse.ResponseCode < 200 || HttpResponse.ResponseCode >= 300)
		{
			if (Self->bUsingLatestEndpoint && AttemptNumber == 1)
			{
				UE_LOG(LogConvaiEditor, Warning,
					   TEXT("UpdateCheckService: /latest endpoint returned %d, falling back to /releases"), HttpResponse.ResponseCode);
				Self->PerformUpdateCheck(Promise, AttemptNumber + 1);
				return;
			}

			FScopeLock Lock(&Self->CacheLock);
			Self->bCheckInProgress = false;

			FUpdateCheckResult ErrorResult = FUpdateCheckResult::Error(
				EUpdateCheckStatus::NetworkError,
				FString::Printf(TEXT("HTTP error %d: %s"), HttpResponse.ResponseCode, *HttpResponse.Body));

			Self->UpdateCache(ErrorResult);
			Promise->SetValue(ErrorResult);
			Self->NotifyDelegates(ErrorResult);
			return;
		}

		TArray<FGitHubReleaseInfo> Releases;

		if (!Self->ParseGitHubReleasesJson(HttpResponse.Body, Releases))
		{
			FScopeLock Lock(&Self->CacheLock);
			Self->bCheckInProgress = false;

			FUpdateCheckResult ErrorResult = FUpdateCheckResult::Error(
				EUpdateCheckStatus::ParseError,
				TEXT("Failed to parse GitHub releases JSON"));

			Self->UpdateCache(ErrorResult);
			Promise->SetValue(ErrorResult);
			Self->NotifyDelegates(ErrorResult);
			return;
		}

		FGitHubReleaseInfo LatestRelease = Self->FindLatestRelease(Releases);

		if (!LatestRelease.IsValid())
		{
			FScopeLock Lock(&Self->CacheLock);
			Self->bCheckInProgress = false;

			FUpdateCheckResult ErrorResult = FUpdateCheckResult::Error(
				EUpdateCheckStatus::ParseError,
				TEXT("No valid releases found"));

			Self->UpdateCache(ErrorResult);
			Promise->SetValue(ErrorResult);
			Self->NotifyDelegates(ErrorResult);
			return;
		}

		FUpdateCheckResult FinalResult = Self->CreateResultFromComparison(LatestRelease);

		FScopeLock Lock(&Self->CacheLock);
		Self->bCheckInProgress = false;

		Self->UpdateCache(FinalResult);
		
		// CRITICAL: Nested AsyncTask also needs WeakPtr
		TWeakPtr<FUpdateCheckService> WeakSelfNested = WeakSelf;
		AsyncTask(ENamedThreads::GameThread, [WeakSelfNested, Promise, FinalResult]()
		{
			TSharedPtr<FUpdateCheckService> SelfNested = WeakSelfNested.Pin();
			if (!SelfNested.IsValid())
			{
				return;
			}
			
			Promise->SetValue(FinalResult);
			SelfNested->NotifyDelegates(FinalResult);
		}); });

	AsyncOp->Start();
}

bool FUpdateCheckService::ParseGitHubReleasesJson(
	const FString &JsonString,
	TArray<FGitHubReleaseInfo> &OutReleases) const
{
	TSharedPtr<FJsonValue> JsonValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonValue) || !JsonValue.IsValid())
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: failed to deserialize JSON"));
		return false;
	}

	auto ParseSingleRelease = [this, &OutReleases](TSharedPtr<FJsonObject> ReleaseObj)
	{
		if (!ReleaseObj.IsValid())
		{
			return;
		}

		FGitHubReleaseInfo Release;
		Release.TagName = ReleaseObj->GetStringField(TEXT("tag_name"));
		Release.ReleaseName = ReleaseObj->GetStringField(TEXT("name"));
		Release.Description = ReleaseObj->GetStringField(TEXT("body"));
		Release.ReleaseUrl = ReleaseObj->GetStringField(TEXT("html_url"));
		Release.bIsPreRelease = ReleaseObj->GetBoolField(TEXT("prerelease"));
		Release.bIsDraft = ReleaseObj->GetBoolField(TEXT("draft"));

		FString PublishedAtStr = ReleaseObj->GetStringField(TEXT("published_at"));
		FDateTime::ParseIso8601(*PublishedAtStr, Release.PublishedAt);

		Release.Version = FGitHubReleaseInfo::ParseVersionFromTag(Release.TagName);

		if (Release.IsValid())
		{
			OutReleases.Add(Release);
		}
	};

	if (JsonValue->Type == EJson::Object)
	{
		TSharedPtr<FJsonObject> ReleaseObj = JsonValue->AsObject();
		ParseSingleRelease(ReleaseObj);
	}
	else if (JsonValue->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>> &ReleasesArray = JsonValue->AsArray();

		for (const TSharedPtr<FJsonValue> &ReleaseValue : ReleasesArray)
		{
			if (!ReleaseValue.IsValid() || ReleaseValue->Type != EJson::Object)
			{
				continue;
			}

			TSharedPtr<FJsonObject> ReleaseObj = ReleaseValue->AsObject();
			ParseSingleRelease(ReleaseObj);
		}
	}
	else
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: unexpected JSON type: %d"), (int32)JsonValue->Type);
		return false;
	}

	return OutReleases.Num() > 0;
}

FGitHubReleaseInfo FUpdateCheckService::FindLatestRelease(const TArray<FGitHubReleaseInfo> &Releases) const
{
	FGitHubReleaseInfo LatestStableRelease;
	FGitHubReleaseInfo LatestPreRelease;
	FSemanticVersion LatestStableVersion;
	FSemanticVersion LatestPreVersion;

	for (const FGitHubReleaseInfo &Release : Releases)
	{
		if (Release.bIsDraft)
		{
			continue;
		}

		if (Release.bIsPreRelease)
		{
			if (!LatestPreVersion.IsValid() || Release.Version > LatestPreVersion)
			{
				LatestPreVersion = Release.Version;
				LatestPreRelease = Release;
			}
		}
		else
		{
			if (!LatestStableVersion.IsValid() || Release.Version > LatestStableVersion)
			{
				LatestStableVersion = Release.Version;
				LatestStableRelease = Release;
			}
		}
	}

	if (Config.bIncludePreReleases)
	{
		if (LatestPreVersion.IsValid() && LatestStableVersion.IsValid())
		{
			return (LatestPreVersion > LatestStableVersion) ? LatestPreRelease : LatestStableRelease;
		}
		else if (LatestPreVersion.IsValid())
		{
			return LatestPreRelease;
		}
		else
		{
			return LatestStableRelease;
		}
	}
	else
	{
		return LatestStableRelease;
	}
}

FUpdateCheckResult FUpdateCheckService::CreateResultFromComparison(const FGitHubReleaseInfo &LatestRelease) const
{
	FPluginVersionInfo LatestVersionInfo(
		LatestRelease.Version,
		LatestRelease.Version.ToString(),
		LatestRelease.ReleaseName);

	if (LatestRelease.Version > CurrentVersionCache.Version)
	{
		return FUpdateCheckResult::UpdateAvailable(
			CurrentVersionCache,
			LatestVersionInfo,
			LatestRelease);
	}
	else
	{
		return FUpdateCheckResult::UpToDate(CurrentVersionCache);
	}
}

FPluginVersionInfo FUpdateCheckService::LoadCurrentVersion() const
{
	TSharedPtr<IPlugin> ConvaiPlugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));

	if (!ConvaiPlugin.IsValid())
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: failed to find Convai plugin"));
		return FPluginVersionInfo();
	}

	const FPluginDescriptor &Descriptor = ConvaiPlugin->GetDescriptor();
	FSemanticVersion Version = FSemanticVersion::Parse(Descriptor.VersionName);

	return FPluginVersionInfo(Version, Descriptor.VersionName, Descriptor.FriendlyName);
}

bool FUpdateCheckService::IsCacheValid() const
{
	if (!CachedResult.IsValid())
	{
		return false;
	}

	if (!LastCheckTimestamp.GetTicks())
	{
		return false;
	}

	double SecondsSinceCheck = (FDateTime::UtcNow() - LastCheckTimestamp).GetTotalSeconds();
	return SecondsSinceCheck < Config.CacheTTLSeconds;
}

void FUpdateCheckService::UpdateCache(const FUpdateCheckResult &Result)
{
	CachedResult = Result;
	LastCheckTimestamp = FDateTime::UtcNow();
}

void FUpdateCheckService::NotifyDelegates(const FUpdateCheckResult &Result)
{
	AsyncTask(ENamedThreads::GameThread, [this, Result]()
			  {
		UpdateCheckCompleteDelegate.Broadcast(Result);
		
		if (Result.IsSuccess())
		{
			UpdateAvailabilityChangedDelegate.Broadcast(
				Result.bUpdateAvailable,
				Result.LatestVersion.VersionString);
		} });
}

FUpdateCheckResult FUpdateCheckService::GetLastCheckResult() const
{
	FScopeLock Lock(&CacheLock);
	return CachedResult;
}

bool FUpdateCheckService::IsUpdateAvailable() const
{
	FScopeLock Lock(&CacheLock);

	if (!CachedResult.bUpdateAvailable)
	{
		return false;
	}

	if (!LastAcknowledgedVersion.IsEmpty() &&
		CachedResult.LatestVersion.VersionString == LastAcknowledgedVersion)
	{
		return false;
	}

	return true;
}

FString FUpdateCheckService::GetLatestVersionString() const
{
	FScopeLock Lock(&CacheLock);
	return CachedResult.LatestVersion.VersionString;
}

FPluginVersionInfo FUpdateCheckService::GetCurrentVersion() const
{
	return CurrentVersionCache;
}

void FUpdateCheckService::OpenReleasesPage() const
{
	FPlatformProcess::LaunchURL(*Config.GitHubReleasesUrl, nullptr, nullptr);
}

double FUpdateCheckService::GetTimeSinceLastCheck() const
{
	FScopeLock Lock(&CacheLock);

	if (!LastCheckTimestamp.GetTicks())
	{
		return -1.0;
	}

	return (FDateTime::UtcNow() - LastCheckTimestamp).GetTotalSeconds();
}

void FUpdateCheckService::AcknowledgeUpdate(const FString &VersionString)
{
	FScopeLock Lock(&CacheLock);
	LastAcknowledgedVersion = VersionString;
	SaveAcknowledgedState();

	const bool bStillAvailable = IsUpdateAvailable();
	UpdateAvailabilityChangedDelegate.Broadcast(bStillAvailable, GetLatestVersionString());
}

void FUpdateCheckService::ClearCache()
{
	FScopeLock Lock(&CacheLock);

	CachedResult = FUpdateCheckResult();
	LastCheckTimestamp = FDateTime(0);
}

const TCHAR *FUpdateCheckService::ConfigSection = TEXT("Update");
const TCHAR *FUpdateCheckService::ConfigKeyLastAcknowledged = TEXT("LastAcknowledgedVersion");

FString FUpdateCheckService::GetPluginConfigPath()
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Convai"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: failed to find Convai plugin"));
		return FString();
	}

	FString ConfigPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("ConvaiEditorSettings.ini"));
	ConfigPath = FPaths::ConvertRelativePathToFull(ConfigPath);

	return ConfigPath;
}

void FUpdateCheckService::LoadAcknowledgedState()
{
	const FString ConfigPath = GetPluginConfigPath();
	if (ConfigPath.IsEmpty())
	{
		return;
	}

	GConfig->GetString(ConfigSection, ConfigKeyLastAcknowledged, LastAcknowledgedVersion, ConfigPath);
}

void FUpdateCheckService::SaveAcknowledgedState() const
{
	const FString ConfigPath = GetPluginConfigPath();
	if (ConfigPath.IsEmpty())
	{
		UE_LOG(LogConvaiEditor, Error, TEXT("UpdateCheckService: cannot save - config path is empty"));
		return;
	}

	const FString ConfigDir = FPaths::GetPath(ConfigPath);
	if (!FPaths::DirectoryExists(ConfigDir))
	{
		IFileManager::Get().MakeDirectory(*ConfigDir, true);
	}

	if (!FPaths::FileExists(ConfigPath))
	{
		FFileHelper::SaveStringToFile(TEXT(""), *ConfigPath);
	}

	FConfigFile *ConfigFile = GConfig->FindConfigFile(ConfigPath);
	if (!ConfigFile)
	{
		GConfig->LoadFile(ConfigPath);
	}

	GConfig->SetString(ConfigSection, ConfigKeyLastAcknowledged, *LastAcknowledgedVersion, ConfigPath);

	GConfig->Flush(false, ConfigPath);
}
