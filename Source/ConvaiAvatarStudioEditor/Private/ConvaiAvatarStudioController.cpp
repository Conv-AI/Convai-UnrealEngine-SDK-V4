// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarStudioController.h"
#include "Services/ConvaiAvatarNativeRuntimeContent.h"
#include "ConvaiAvatarErrorPresentation.h"
#include "ConvaiAvatarSaveReview.h"
#include "Capture/ConvaiAvatarPortraitCapture.h"
#include "Workspace/ConvaiAvatarBlueprintSetup.h"
#include "Workspace/ConvaiAvatarSourceRevision.h"
#include "Workspace/ConvaiAvatarUnboundBindings.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Diorama/ConvaiAvatarDiorama.h"
#include "Archive/ConvaiAvatarSourceArchive.h"
#include "Archive/ConvaiAvatarSourceArchivePrivate.h"
#include "Packaging/ConvaiAvatarPackagingPrivate.h"
#include "Services/ConvaiAvatarDownloadService.h"
#include "Services/ConvaiAvatarDownloadServicePrivate.h"
#include "Services/ConvaiAvatarUploadDefaults.h"
#include "Services/ConvaiAvatarArtifactHealth.h"
#include "Services/ConvaiAvatarAssetsClientPrivate.h"
#include "Services/ConvaiAvatarSourceSelection.h"
#include "Services/ConvaiAvatarDependencies.h"
#include "Async/Async.h"
#include "ConvaiUtils.h"
#include "RestAPI/ConvaiURL.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/EngineVersion.h"
#include "Misc/MessageDialog.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "Framework/Docking/TabManager.h"
#include "Editor.h"
#include "Services/ConvaiDIContainer.h"
#include "Services/ConfigurationService.h"
#include "Services/IAuthWindowManager.h"
#include "Services/OAuth/IOAuthAuthenticationService.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/App.h"
#include "Config/ConvaiAvatarProjectConfiguration.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/PlatformProcess.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

/** Game-thread publish state. Archive workers receive only copied filesystem data. */
struct FConvaiAvatarPublishRun
{
    FConvaiAvatarPreparedAsset Prepared;
    FConvaiAvatarPackageResult Package;
    FConvaiAvatarStudioUploadOptions Options;
    FString AssetId;
    FString Gender;
    FString Thumbnail;
    TArray<TPair<FString,FString>> Artifacts;
    TArray<FString> Completed;
    int32 NextArtifact = 0;
};

namespace
{
    FConvaiAvatarPackageOptions PackageOptions(const FConvaiAvatarStudioUploadOptions& Options)
    {
        FConvaiAvatarPackageOptions Result;
        Result.bIncludeSource=Options.bIncludeSource;Result.bIncludeWindows=Options.bIncludeWindows;Result.bIncludeLinux=Options.bIncludeLinux;
        return Result;
    }

    FString ArtifactLabel(const FString& Platform) { return Platform==TEXT("Raw") ? TEXT("Source") : Platform; }

    const TCHAR* const ThumbnailCaptureTitle=TEXT("Capture thumbnail");
    const TCHAR* const ThumbnailCaptureDetail=TEXT("Preparing the portrait and letting the avatar look at the camera...");

    FString DioramaSourceLevel(const FConvaiAvatarPreparedAsset& Prepared)
    {
        if(!Prepared.Diorama)return {};
        const FName* Source=Prepared.SourceToDestinationPackages.FindKey(FName(*Prepared.Diorama->Level));
        return Source?Source->ToString():Prepared.Diorama->Level;
    }

    FString DioramaNotice(const FConvaiAvatarPreparedAsset& Prepared)
    {
        if(!Prepared.Diorama)return {};
        return FString::Printf(TEXT("%s is generated on every upload - edit %s, not the copy."),
            *Prepared.Diorama->Level.Mid(Prepared.PluginName.Len()+2),*FPackageName::GetShortName(DioramaSourceLevel(Prepared)));
    }

    /** True once the pair needs no retry: its cloud avatar is bound in the draft's plugin. */
    bool RetryUnboundBinding(const FConvaiAvatarUnboundBinding& Pair,FString& OutError)
    {
        FConvaiAvatarPreparedAsset Record;bool bReady=false;
        if(FConvaiAvatarWorkspace::FindLocalAssetRecord(Pair.DraftId,Record,bReady,OutError))
            return FConvaiAvatarWorkspace::RebindPreparedAsset(Record,Pair.CloudId,OutError);
        // A receipt the failed bind did write is reconciled by lookups, and then the draft ID finds nothing.
        FString BoundError;
        return FConvaiAvatarWorkspace::FindLocalAssetRecord(Pair.CloudId,Record,bReady,BoundError) && Record.PluginName==Pair.PluginName;
    }
}

namespace ConvaiAvatarStudioControllerPrivate
{
    /** Archive worker. Nothing reads the zip after Stage, so it goes whatever Stage returned. */
    bool StageDownloadedZip(const FString& Zip,const FConvaiAvatarDownloadSource& Source,bool bReplace,FConvaiAvatarPendingDownload& OutJob,FString& OutError,
        const std::atomic<bool>& Cancelled)
    {
        const bool bStaged=ConvaiAvatarDownloadServicePrivate::Stage(Zip,Source,bReplace,OutJob,OutError,Cancelled);
        IFileManager::Get().Delete(*Zip,false,false,true);
        return bStaged;
    }

    bool StageDownloadedZip(const FString& Zip,const FConvaiAvatarDownloadSource& Source,bool bReplace,FConvaiAvatarPendingDownload& OutJob,FString& OutError)
    {
        static const std::atomic<bool> NeverCancelled{false};
        return StageDownloadedZip(Zip,Source,bReplace,OutJob,OutError,NeverCancelled);
    }

    /** Cancelled and crashed downloads leave their zip behind. No download takes a day, so a zip
     * that another editor is still staging is never this old. A link at the folder or between it and
     * the project would aim the sweep at someone else's files, so then it deletes nothing. */
    void DeleteStaleDownloadZips(const FString& Directory)
    {
        FString LinkError;
        if(!ConvaiAvatarDownloadServicePrivate::NoLinksBelow(Directory,FPaths::ProjectDir(),LinkError))return;
        TArray<FString> Names;
        IFileManager::Get().FindFiles(Names,*(Directory/TEXT("*.zip")),true,false);
        const FDateTime Cutoff=FDateTime::UtcNow()-FTimespan::FromDays(1);
        for(const FString& Name:Names)
        {
            const FDateTime Written=IFileManager::Get().GetTimeStamp(*(Directory/Name));
            if(Written!=FDateTime::MinValue() && Written<Cutoff)IFileManager::Get().Delete(*(Directory/Name),false,false,true);
        }
    }
}

TSharedRef<SConvaiAvatarStudio> FConvaiAvatarStudioController::CreateWidget()
{
    TSharedRef<SConvaiAvatarStudio> View = SNew(SConvaiAvatarStudio)
        .OnRefresh(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::Refresh))
        .OnSelect(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::Select))
        .OnCreate(FOnConvaiAvatarStudioCreate::CreateSP(this, &FConvaiAvatarStudioController::Create))
        .OnScanDiorama(FOnConvaiAvatarStudioCreate::CreateSP(this, &FConvaiAvatarStudioController::ScanDiorama))
        .OnSaveMetadata(FOnConvaiAvatarStudioMetadata::CreateSP(this, &FConvaiAvatarStudioController::SaveMetadata))
        .OnCaptureCreate(FOnConvaiAvatarStudioCreate::CreateSP(this, &FConvaiAvatarStudioController::CaptureCreatePortrait))
        .OnCaptureMetadata(FOnConvaiAvatarStudioMetadata::CreateSP(this, &FConvaiAvatarStudioController::CaptureMetadataPortrait))
        .OnBrowseContent(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::BrowseContent))
        .OnOpenLog(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::OpenLog))
        .OnOpenDocumentation(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::OpenDocumentation))
        .OnUploadChanges(FOnConvaiAvatarStudioUpload::CreateSP(this, &FConvaiAvatarStudioController::UploadChanges))
        .OnDownload(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::Download))
        .OnDelete(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::Delete))
        .OnResumeLocalDraft(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::ResumeLocalDraft))
        .OnDiscardLocalDraft(FOnConvaiAvatarStudioAssetAction::CreateSP(this, &FConvaiAvatarStudioController::DiscardLocalDraft))
        .OnCancelJob(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::Cancel))
        .OnApplyConfiguration(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::ApplyConfiguration))
        .OnRestoreConfiguration(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::RestoreConfiguration))
        .OnContinueEngineMismatch(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::ContinueEngineMismatch))
        .OnSignIn(FSimpleDelegate::CreateSP(this, &FConvaiAvatarStudioController::SignIn));
    Widget = View;
    auto Configuration = FConvaiDIContainerManager::Get().Resolve<IConfigurationService>();
    // Reopening the tab reuses this controller and its existing subscription.
    if (Configuration.IsSuccess() && !AuthenticationChangedHandle.IsValid())
    {
        AuthenticationConfiguration = Configuration.GetValue();
        AuthenticationChangedHandle = AuthenticationConfiguration->OnAuthenticationChanged().AddLambda([Weak=AsWeak()]
        {
            AsyncTask(ENamedThreads::GameThread, [Weak]
            {
                if (auto Self=Weak.Pin(); Self && !Self->bShuttingDown) Self->bAuthenticationRefreshPending=true;
            });
        });
        // Coalesce OAuth credential updates. An active upload keeps its original
        // client until completion; then the current account's library replaces it.
        AuthenticationRefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak=AsWeak()](float)
        {
            const auto Self=Weak.Pin();if(!Self || Self->bShuttingDown)return false;
            if(Self->bAuthenticationRefreshPending && !Self->State.bBusy)
            {Self->bAuthenticationRefreshPending=false;Self->Refresh();}
            return true;
        }),0.25f);
    }
    const auto Capabilities=FConvaiAvatarPackaging::GetCapabilities();
    State.bSourceAvailable=Capabilities.bSourceAvailable;State.bWindowsAvailable=Capabilities.bWindowsAvailable;State.bLinuxAvailable=Capabilities.bLinuxAvailable;
    State.SourceDisabledReason=Capabilities.SourceReason;State.WindowsDisabledReason=Capabilities.WindowsReason;State.LinuxDisabledReason=Capabilities.LinuxReason;
    State.bDefaultIncludeSource=Capabilities.bSourceAvailable;State.bDefaultIncludeWindows=Capabilities.bWindowsAvailable;
    State.bDefaultIncludeLinux=false;
    // A tab reopened during a job: Refresh would return without showing it.
    if (State.bBusy) Publish(); else Refresh();
    return View;
}

bool FConvaiAvatarStudioController::EnsureClient()
{
    const auto Auth = UConvaiUtils::GetAuthHeaderAndKey();
    State.bNeedsSignIn = Auth.Value.IsEmpty();
    State.bCanCreate = !State.bNeedsSignIn;
    if (State.bNeedsSignIn)
    {
        ++Generation;if(RuntimeContractCheck)RuntimeContractCheck->Cancel();if(Client)Client->CancelAll();Client.Reset();AuthFingerprint.Reset();
        Assets.Reset();bLibraryListed=false;LibraryState.Reset();ResetLibraryDetails();State.SelectedAssetId.Empty();State.bLoading=false;
        State.CreateDisabledReason = TEXT("Sign in to Convai to access your avatar library.");
        Fail(State.CreateDisabledReason); return false;
    }
    const FString Fingerprint = FMD5::HashAnsiString(*(Auth.Key + Auth.Value));
    if (!Client || Fingerprint != AuthFingerprint)
    {
        // Retire every response from the previous authentication context before cancellation.
        if (!ensure(!State.bBusy)) AbandonJob(); else ++Generation;
        if (RuntimeContractCheck) RuntimeContractCheck->Cancel();
        if (Client) Client->CancelAll();
        Assets.Reset();bLibraryListed=false;LibraryState.Reset();ResetLibraryDetails();State.SelectedAssetId.Empty();
        Client = MakeShared<FConvaiAvatarAssetsClient>(Auth.Value, UConvaiURL::GetFullURL(TEXT("assets"), true), Auth.Key);
        AuthFingerprint = Fingerprint;
    }
    State.CreateDisabledReason.Empty();
    return true;
}

void FConvaiAvatarStudioController::SignIn()
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

void FConvaiAvatarStudioController::Publish()
{
    if (bShuttingDown) return;
    State.bCanApplyConfiguration=!State.bBusy && !State.bRemoteConfigurationLoading;
    State.bCanRestoreConfiguration=!State.bBusy && FConvaiAvatarProjectConfiguration::HasBackup(FPaths::ProjectDir());
    State.bCanCapturePortrait=FConvaiAvatarPortraitCapture::IsAvailable(State.CaptureDisabledReason);
    const bool bEngineReviewPending=State.bRemoteConfigurationLoading || RemoteConfigurationData.CurrentEngineVersion.IsEmpty() || (State.bEngineMismatch && !State.bEngineMismatchAcknowledged);
    const FString EngineReviewReason=!bEngineReviewPending?FString():
        (State.bRemoteConfigurationLoading || State.EngineWarning.IsEmpty())?TEXT("Checking current engine support..."):State.EngineWarning;
    State.bCanCreate=!State.bNeedsSignIn && !bEngineReviewPending;
    if(!State.bNeedsSignIn)State.CreateDisabledReason=EngineReviewReason;
    State.Avatars.Reset();
    for (const auto& Asset : Assets)
    {
        FConvaiAvatarStudioCard Card;
        Card.AssetId = Asset.AssetId; Card.Name = Asset.Name; Card.Gender=Asset.Gender;Card.ThumbnailUrl = Asset.ThumbnailUrl;
        Card.SquareThumbnailUrl = Asset.SquareThumbnailUrl;
        Card.ThumbnailLocalPath=LocalThumbnails.FindRef(Asset.AssetId);
        const auto SourcePresence=Asset.SourcePresence();
        Card.bSourceStatusKnown=SourcePresence!=EConvaiAvatarArtifactPresence::Unknown;
        Card.bHasSource=SourcePresence==EConvaiAvatarArtifactPresence::Available ||
            (SourcePresence==EConvaiAvatarArtifactPresence::Unknown && Asset.HasDeclaredSource());
        Card.SourceStatus=Card.bSourceStatusKnown
            ? (Card.bHasSource?TEXT("Editable source available in cloud"):TEXT("Editable source not uploaded"))
            : (Card.bHasSource?TEXT("Editable source is listed. Its availability is checked when downloading."):TEXT("Editable source availability has not been checked."));
        Card.bDetailsLoading=Asset.AssetId==State.SelectedAssetId && PendingDetailsId==Asset.AssetId && DetailsOperation==Generation && PendingDetailsAccount==AuthFingerprint;
        if(Asset.AssetId==State.SelectedAssetId)Card.DetailsError=SelectedDetailsError;
        const auto Health=ConvaiAvatarArtifactHealth::Evaluate(Asset,RemoteConfigurationData.CurrentEngineVersion);
        Card.HealthWarning=Health.Warning;
        Card.bHealthUnknown=Health.Status==EConvaiAvatarArtifactHealth::Unknown;
        if(!Asset.bHasArtifactDetails)Card.HealthWarning.Empty();
        Card.SourceBytes=Health.SourceBytes; Card.WindowsBytes=Health.WindowsBytes; Card.LinuxBytes=Health.LinuxBytes;
        Card.SourceVersion=Health.SourceVersion; Card.WindowsVersion=Health.WindowsVersion; Card.LinuxVersion=Health.LinuxVersion;
        const auto Unconfirmed=LibraryState.UnconfirmedUploads(Asset.AssetId);
        if(!State.bBusy && !Unconfirmed.IsEmpty())
        {
            TArray<FString> Labels;
            for(FString Version:Unconfirmed)
            {
                Version.RemoveFromStart(TEXT("ue-"));
                const int32 Separator=Version.Find(TEXT("-"),ESearchCase::CaseSensitive,ESearchDir::FromEnd);
                Labels.Add(Separator==INDEX_NONE?Version:ArtifactLabel(Version.Mid(Separator+1))+TEXT(" (Unreal ")+Version.Left(Separator)+TEXT(")"));
            }
            if(!Card.HealthWarning.IsEmpty())Card.HealthWarning+=TEXT("\n\n");
            Card.HealthWarning+=TEXT("The last upload did not confirm completion for: ")+FString::Join(Labels,TEXT(", "))+
                TEXT(". Older uploaded files may still be available. Choose Upload changes and select these files to retry.");
            Card.bHealthUnknown=false;
        }
        Card.EngineVersion = Asset.FindSourceVersion();
        Card.EngineVersion.RemoveFromStart(TEXT("ue-"));Card.EngineVersion.RemoveFromEnd(TEXT("-Raw"));
        if(Card.EngineVersion.Equals(TEXT("raw"),ESearchCase::IgnoreCase))Card.EngineVersion.Empty();
        Card.bIsLocal = LocalAssetIds.Contains(Asset.AssetId);
        Card.Status = Card.bIsLocal ? TEXT("In this project") : (Card.bSourceStatusKnown
            ? (Card.bHasSource?TEXT("Source available"):TEXT("Source not uploaded"))
            : (Card.bHasSource?TEXT("Source listed"):TEXT("Details not checked")));
        if(IncompleteLocalIds.Contains(Asset.AssetId))Card.Status=TEXT("Preparation needs retry");
        Card.bCanDownload = (Card.bHasSource || !Card.bSourceStatusKnown) && !State.bBusy;
        Card.DownloadDisabledReason = TEXT("The owner must upload source before this avatar can be added to a project.");
        Card.bCanUpload = Card.bIsLocal && !State.bBusy && !bEngineReviewPending;
        Card.UploadDisabledReason = bEngineReviewPending?EngineReviewReason:(Card.bIsLocal ? FString() : TEXT("Download this avatar's source into this project first."));
        Card.bCanDelete = !State.bBusy && Asset.IsPrivate();
        Card.bCanEditMetadata=!State.bBusy && Asset.IsPrivate();
        Card.MetadataDisabledReason=TEXT("Refresh your library before editing this avatar's details.");
        if(const auto* Local=LocalRecords.Find(Asset.AssetId))
        {
            Card.BlueprintPath=ConvaiAvatarSourceSelection::EditableBlueprint(*Local).ToString();
            Card.bIncludeConvaiContent=Local->bIncludeConvaiContent;
        }
        Card.bCanBrowseContent=!State.bBusy && Card.bIsLocal && !IncompleteLocalIds.Contains(Asset.AssetId) && !Card.BlueprintPath.IsEmpty();
        Card.bCanCapturePortrait=Card.bCanBrowseContent && State.bCanCapturePortrait;
        Card.BrowseDisabledReason=Card.bIsLocal?TEXT("Finish preparing this avatar before browsing its Blueprint."):TEXT("Download this avatar's source into this project first.");
        State.Avatars.Add(MoveTemp(Card));
    }
    for(const auto& Pair:LocalDrafts)
    {
        const auto& Draft=Pair.Value;
        FConvaiAvatarStudioCard Card;
        Card.AssetId=Draft.AssetId;Card.Name=Draft.DisplayName;Card.bIsLocal=true;Card.bIsLocalDraft=true;
        Card.bIncludeConvaiContent=Draft.bIncludeConvaiContent;
        Card.bIsIncompleteLocalDraft=IncompleteLocalIds.Contains(Draft.AssetId);
        Card.Status=Card.bIsIncompleteLocalDraft?TEXT("Local draft — preparation incomplete"):TEXT("Local draft — upload not completed");
        Card.HealthWarning=TEXT("This draft has not finished uploading. Resume the draft to create its cloud avatar.");
        Card.bCanResumeDraft=!State.bBusy;Card.bCanDiscardDraft=!State.bBusy;
        State.Avatars.Add(MoveTemp(Card));
    }
    if (auto View = Widget.Pin()) View->SetState(State);
}

void FConvaiAvatarStudioController::RefreshLocalRecords()
{
    TArray<FString> Problems,UnreadablePairs;
    // Before listing: until its cloud avatar is bound, a draft stays out of LocalDrafts, or Resume draft would create another.
    // A pair this session could not save is retried with the saved ones, and saved again.
    TSet<FString> SavedPairs;
    bool bPairFolderUnreadable=false;
    for(FConvaiAvatarUnboundBinding& Pair:ConvaiAvatarWorkspacePrivate::LoadUnboundBindings(&UnreadablePairs,&bPairFolderUnreadable))
    {SavedPairs.Add(Pair.DraftId);UnboundBindings.Add(Pair.DraftId,MoveTemp(Pair));}
    for(auto It=UnboundBindings.CreateIterator();It;++It)
    {
        const FConvaiAvatarUnboundBinding& Pair=It.Value();
        // A pair goes only once its cloud avatar is known to be gone: this session deleted it, or a lookup found nothing
        // (CompleteBindingLookup). Dropped while the avatar exists, Resume draft would create a second one.
        const bool bListed=bLibraryListed && Assets.ContainsByPredicate([&Pair](const FConvaiAvatarAsset& Asset){return Asset.AssetId==Pair.CloudId;});
        const bool bDraftGone=!IFileManager::Get().DirectoryExists(*(FConvaiAvatarWorkspace::GetAvatarsDirectory()/Pair.PluginName));
        FString Error;
        if(LibraryState.IsDeleted(Pair.CloudId) || bDraftGone || (bListed && RetryUnboundBinding(Pair,Error)))
        {ConvaiAvatarWorkspacePrivate::RemoveUnboundBinding(Pair.DraftId);It.RemoveCurrent();continue;}
        if(bListed)Problems.Add(FString::Printf(TEXT("Cloud avatar %s is not linked to its local draft yet: %s"),*Pair.CloudId,*Error));
        // Only the next restart forgets a pair deleted from disk: a refresh saves the one it holds again.
        else if(bLibraryListed && !IsCurrentCredential(Pair))
            Problems.Add(FString::Printf(TEXT("Cloud avatar %s was created from a local draft while Convai used a different API key or sign-in token, so it cannot be checked and the draft is not offered to resume. Sign in with that key or token again, then refresh. If that cloud avatar no longer exists, close Unreal, delete %s, and reopen the project."),
                *Pair.CloudId,*FPaths::ConvertRelativePathToFull(ConvaiAvatarWorkspacePrivate::UnboundBindingFile(Pair.DraftId))));
        if(!SavedPairs.Contains(Pair.DraftId) && !ConvaiAvatarWorkspacePrivate::SaveUnboundBinding(Pair,Error))
            Problems.Add(FString::Printf(TEXT("The link between cloud avatar %s and its local draft could not be saved, so restarting the editor loses it: %s"),*Pair.CloudId,*Error));
    }
    TSet<FString> UnreadableNames;
    for(const FString& File:UnreadablePairs)
    {
        UnreadableNames.Add(FPaths::GetCleanFilename(File));
        Problems.Add(TEXT("A local draft's link to the cloud avatar it created could not be read, so that draft is not offered to resume: ")+File+TEXT(". Close any program using it, then refresh."));
    }
    if(bPairFolderUnreadable)
        Problems.Add(TEXT("The folder that links local drafts to the cloud avatars they created points outside this project, so it is not read and no local draft is offered to resume: ")+
            FPaths::ConvertRelativePathToFull(ConvaiAvatarWorkspacePrivate::UnboundBindingsDirectory())+TEXT(". Make it an ordinary folder in this project, then refresh."));
    TArray<FConvaiAvatarPreparedAsset> Records;FString Error;
    LocalAssetIds.Reset();LocalDrafts.Reset();IncompleteLocalIds.Reset();LocalRecords.Reset();
    FConvaiAvatarWorkspace::ListLocalAssetRecords(Records,IncompleteLocalIds,Error);
    for(const auto& Record:Records)
    {
        LocalRecords.Add(Record.AssetId,Record);
        // An unreadable pair file is named after its draft and may hold a cloud avatar that draft already created; with the
        // folder unreadable, any draft may have one.
        const bool bUnbound=bPairFolderUnreadable || UnboundBindings.Contains(Record.AssetId) ||
            UnreadableNames.Contains(ConvaiAvatarWorkspacePrivate::UnboundBindingFileName(Record.AssetId));
        if(Record.bIsDraft && !bUnbound)LocalDrafts.Add(Record.AssetId,Record);
        else LocalAssetIds.Add(Record.AssetId);
    }
    if(!Error.IsEmpty())Problems.Add(Error);
    if(!Problems.IsEmpty())State.Notice=FString::Join(Problems,TEXT("\n"));
}

void FConvaiAvatarStudioController::RetryUnboundBindings()
{
    if(UnboundBindings.IsEmpty())return;
    RefreshLocalRecords();
    if(!CanRequestAsset())return;
    // Copied first: a lookup can complete before the next starts and change the pending pairs.
    TArray<FConvaiAvatarUnboundBinding> Unlisted;
    for(const auto& Entry:UnboundBindings)
    {
        const FConvaiAvatarUnboundBinding& Pair=Entry.Value;
        if(IsCurrentCredential(Pair) && !Assets.ContainsByPredicate([&Pair](const FConvaiAvatarAsset& Asset){return Asset.AssetId==Pair.CloudId;}))Unlisted.Add(Pair);
    }
    for(const FConvaiAvatarUnboundBinding& Pair:Unlisted)
        RequestAsset(Pair.CloudId,[Weak=AsWeak(),Op=Generation,Pair](FConvaiAvatarAsset Asset,FString Error)
        {
            if(const auto Self=Weak.Pin();Self && !Self->bShuttingDown && Self->Generation==Op)Self->CompleteBindingLookup(Pair,Asset,Error);
        });
}

bool FConvaiAvatarStudioController::IsCurrentCredential(const FConvaiAvatarUnboundBinding& Pair) const
{
    // Another account's lookup answers not-found for an avatar that exists. A pair saved before credentials were recorded was
    // written on this machine by the previous build, so it is taken to be the current credential's.
    return Pair.Account.IsEmpty() || Pair.Account==AuthFingerprint;
}

void FConvaiAvatarStudioController::CompleteBindingLookup(const FConvaiAvatarUnboundBinding& Pair,const FConvaiAvatarAsset& Asset,const FString& Error)
{
    const FConvaiAvatarUnboundBinding* Pending=UnboundBindings.Find(Pair.DraftId);
    if(!Pending || Pending->CloudId!=Pair.CloudId)return;
    // Added to the notice, which may hold the startup one.
    const auto Report=[this](const FString& Line)
    {
        if(!State.Notice.Contains(Line))State.Notice=State.Notice.IsEmpty()?Line:State.Notice+TEXT("\n")+Line;
        Publish();
    };
    FString BindError;
    // An avatar that exists but is not listed yet, such as one created moments ago, is bound.
    if(Error.IsEmpty() && Asset.AssetId==Pair.CloudId)
    {
        if(!RetryUnboundBinding(Pair,BindError))
        {Report(FString::Printf(TEXT("Cloud avatar %s is not linked to its local draft yet: %s"),*Pair.CloudId,*BindError));return;}
    }
    else if(!ConvaiAvatarAssetsClientPrivate::IsNotFound(Error))
    {Report(FString::Printf(TEXT("Cloud avatar %s could not be checked, so its local draft stays held back. Refresh to retry: %s"),*Pair.CloudId,*Error));return;}
    ConvaiAvatarWorkspacePrivate::RemoveUnboundBinding(Pair.DraftId);UnboundBindings.Remove(Pair.DraftId);
    RefreshLocalRecords();Publish();
}

void FConvaiAvatarStudioController::ResumeLocalDraft(const FString& InId)
{
    const FString Id=InId;
    if(State.bBusy)return;
    const auto* Draft=LocalDrafts.Find(Id);
    if(!Draft)return;
    FConvaiAvatarStudioCreateRequest Request;
    Request.DraftId=Draft->AssetId;Request.DisplayName=Draft->DisplayName;
    Request.BlueprintPath=ConvaiAvatarSourceSelection::EditableBlueprint(*Draft).ToString();
    Request.bIsMetaHuman=Draft->bIsMetaHuman;
    Request.bIncludeConvaiContent=Draft->bIncludeConvaiContent;
    Request.bIncludeDiorama=Draft->Diorama.IsSet();
    if(Request.bIncludeDiorama)
    {
        const FString Level=DioramaSourceLevel(*Draft);
        Request.LevelPath=Level+TEXT(".")+FPackageName::GetShortName(Level);
    }
    if(auto View=Widget.Pin())View->ShowDraft(Request);
}

void FConvaiAvatarStudioController::DiscardLocalDraft(const FString& InId)
{
    const FString Id=InId;
    // The view confirms the named draft. The workspace verifies ownership and refuses loaded content.
    if(State.bBusy || !LocalDrafts.Contains(Id))return;
    FString Error;
    if(!FConvaiAvatarWorkspace::DiscardLocalDraft(Id,Error)){Fail(Error);return;}
    RefreshLocalRecords();State.Notice=TEXT("Local draft discarded. The original avatar is unchanged.");
    if(State.SelectedAssetId==Id)State.SelectedAssetId.Empty();
    Publish();
}
void FConvaiAvatarStudioController::WriteDiagnosticLog(const FString& Error)
{
    const FString SafeError=ConvaiAvatarErrorPresentation::RedactDiagnostic(Error,UConvaiUtils::GetAuthHeaderAndKey().Value);
    const FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("ConvaiAvatarStudio/Logs"));
    const FString Log=Directory/(TEXT("cloud-avatars-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".log"));
    if(IFileManager::Get().MakeDirectory(*Directory,true) && FFileHelper::SaveStringToFile(
        FDateTime::UtcNow().ToIso8601()+TEXT("\n")+State.JobTitle+TEXT("\n")+State.JobDetail+TEXT("\n")+SafeError+TEXT("\n"),*Log))
    {
        DiagnosticPaths.Add(TEXT("latest"),Log);State.bCanOpenLog=true;
        State.DiagnosticReference=FPaths::GetCleanFilename(Log);
    }
}
void FConvaiAvatarStudioController::Fail(const FString& Error, const FString& FriendlyError)
{
    WriteDiagnosticLog(Error);
    CurrentPublish.Reset();
    // Outside a job the notice is no job's, and nothing rebuilds the startup one: it waits under the error until a refresh clears that.
    EndJob(State.bBusy?FString():State.Notice,ConvaiAvatarErrorPresentation::ForDisplay(Error,FriendlyError,UConvaiUtils::GetAuthHeaderAndKey().Value));
}
void FConvaiAvatarStudioController::FailLocal(const FString& Error)
{
    // Summarize only local recovery instructions. Fail still logs the complete redacted detail.
    // Redact before shortening so the summary cannot expose a truncated credential or URL.
    const auto Auth=UConvaiUtils::GetAuthHeaderAndKey();
    const FString SafeError=ConvaiAvatarErrorPresentation::RedactDiagnostic(Error,Auth.Value);
    Fail(Error,ConvaiAvatarErrorPresentation::LocalRecoveryMessage(SafeError));
}
void FConvaiAvatarStudioController::SetJob(const FString& Title, const FString& Detail, bool bCancellable)
{
    ++SelectionGeneration;PendingDetailsId.Empty();PendingDetailsAccount.Empty();SelectedDetailsError.Empty();
    State.bBusy = true; State.bLoading = false; State.bCanCancel = bCancellable; State.Error.Empty(); State.Notice.Empty();
    State.JobTitle = Title; State.JobDetail = Detail; State.JobProgress.Reset(); State.JobBytesCompleted.Reset(); State.JobBytesTotal.Reset(); Publish();
}
uint64 FConvaiAvatarStudioController::BeginJob(const FString& Title, const FString& Detail, bool bCancellable)
{
    const uint64 Job=++Generation;
    SetJob(Title,Detail,bCancellable);
    return Job;
}
void FConvaiAvatarStudioController::EndJob(const FString& Notice, const FString& Error)
{
    State.bBusy=false;State.bCanCancel=false;State.bLoading=false;
    State.JobTitle.Empty();State.JobDetail.Empty();State.JobProgress.Reset();State.JobBytesCompleted.Reset();State.JobBytesTotal.Reset();
    bLocalWorkActive=false;bDioramaScanPending=false;bRemoteActionPending=false;
    if(PortraitCapture){PortraitCapture->Cancel();PortraitCapture.Reset();}
    State.Notice=Notice;State.Error=Error;
    Publish();
}
void FConvaiAvatarStudioController::AbandonJob(const FString& Notice)
{
    ++Generation;
    if (RuntimeContractCheck) RuntimeContractCheck->Cancel();
    EndJob(Notice,FString());
}
void FConvaiAvatarStudioController::ResetLibraryDetails()
{
    ++SelectionGeneration;
    DetailedAssetIds.Reset();PendingDetailsId.Empty();PendingDetailsAccount.Empty();SelectedDetailsError.Empty();
}

void FConvaiAvatarStudioController::Select(const FString& InId)
{
    const FString Id=InId;
    if(State.bBusy || bShuttingDown || LibraryState.IsDeleted(Id))return;
    if(!Assets.ContainsByPredicate([&Id](const auto& Asset){return Asset.AssetId==Id && Asset.IsPrivate();}) && !LocalDrafts.Contains(Id))return;
    if(State.SelectedAssetId!=Id)
    {
        ++SelectionGeneration;PendingDetailsId.Empty();PendingDetailsAccount.Empty();SelectedDetailsError.Empty();
    }
    State.SelectedAssetId=Id;
    RequestSelectedDetails(Id);
    Publish();
}

void FConvaiAvatarStudioController::RequestSelectedDetails(const FString& InId)
{
    const FString Id=InId;
    if(Id.IsEmpty() || State.bBusy || State.bLoading || bShuttingDown || DetailedAssetIds.Contains(Id) || LibraryState.IsDeleted(Id))return;
    if(!Assets.ContainsByPredicate([&Id](const auto& Asset){return Asset.AssetId==Id && Asset.IsPrivate();}))return;
    if(PendingDetailsId==Id && DetailsOperation==Generation && PendingDetailsAccount==AuthFingerprint)return;
    if(!CanRequestAsset())return;
    PendingDetailsId=Id;PendingDetailsAccount=AuthFingerprint;DetailsOperation=Generation;SelectedDetailsError.Empty();
    const uint64 Selection=++SelectionGeneration;
    FConvaiAvatarAssetsClient::FAssetCallback Completion=[Weak=AsWeak(),Id,Operation=Generation,Selection,Account=AuthFingerprint](FConvaiAvatarAsset Asset,FString Error)
    {
        if(const auto Self=Weak.Pin())Self->CompleteSelectedDetails(Id,Operation,Selection,Account,MoveTemp(Asset),MoveTemp(Error));
    };
    RequestAsset(Id,MoveTemp(Completion));
}

bool FConvaiAvatarStudioController::CanRequestAsset() const
{
#if WITH_DEV_AUTOMATION_TESTS
    if(AssetRequestOverride)return true;
#endif
    return Client.IsValid();
}

void FConvaiAvatarStudioController::RequestAsset(const FString& Id,FConvaiAvatarAssetsClient::FAssetCallback Completion)
{
#if WITH_DEV_AUTOMATION_TESTS
    if(AssetRequestOverride){AssetRequestOverride(Id,MoveTemp(Completion));return;}
#endif
    Client->GetAsset(Id,MoveTemp(Completion));
}

void FConvaiAvatarStudioController::CompleteSelectedDetails(const FString& Id,uint64 Operation,uint64 Selection,const FString& Account,FConvaiAvatarAsset Asset,FString Error)
{
    if(bShuttingDown || Generation!=Operation || SelectionGeneration!=Selection || AuthFingerprint!=Account ||
        State.SelectedAssetId!=Id || PendingDetailsId!=Id || LibraryState.IsDeleted(Id))return;
    PendingDetailsId.Empty();PendingDetailsAccount.Empty();
    auto* Existing=Assets.FindByPredicate([&Id](const auto& Item){return Item.AssetId==Id && Item.IsPrivate();});
    if(!Existing)return;
    if(!Error.IsEmpty() || Asset.AssetId!=Id || !Asset.IsPrivate() || !Asset.bHasArtifactDetails)
    {
        SelectedDetailsError=TEXT("Avatar details could not be checked. Select this avatar again or refresh to retry.");
        Publish();return;
    }
    FConvaiAvatarAssetsClient::PreserveOptionalSquareThumbnail(*Existing,Asset);
    *Existing=MoveTemp(Asset);DetailedAssetIds.Add(Id);SelectedDetailsError.Empty();
    Publish();
}

void FConvaiAvatarStudioController::CompleteAvatarImages(uint64 Operation,const FString& Account,TArray<FConvaiAvatarImageRecord> Images,FString Error)
{
    if(bShuttingDown || Generation!=Operation || AuthFingerprint!=Account || !Error.IsEmpty())return;
    LibraryState.RemoveDeleted(Assets);
    FConvaiAvatarAssetsClient::ApplyAvatarImages(Images,Assets);
    Publish();
}

bool FConvaiAvatarStudioController::ValidateUploadOptions(const FConvaiAvatarStudioUploadOptions& Options)
{
    FString Error;
    const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("ConvAI"));
    if(!Plugin || !ConvaiAvatarDependencies::ValidateInstallation(Plugin->GetBaseDir(),Error))
    {
        Fail(Error.IsEmpty()?TEXT("The Convai plugin could not be found."):Error,
            TEXT("Cloud Avatars is missing required files. Reinstall this version of the Convai plugin, including its Resources folder."));
        return false;
    }
    if(State.bRemoteConfigurationLoading || RemoteConfigurationData.CurrentEngineVersion.IsEmpty() || (State.bEngineMismatch && !State.bEngineMismatchAcknowledged))
    {Fail(TEXT("Review the engine version notice and choose Continue with this engine before uploading."));return false;}
    const auto Capabilities=FConvaiAvatarPackaging::GetCapabilities();
    State.bSourceAvailable=Capabilities.bSourceAvailable;State.bWindowsAvailable=Capabilities.bWindowsAvailable;State.bLinuxAvailable=Capabilities.bLinuxAvailable;
    State.SourceDisabledReason=Capabilities.SourceReason;State.WindowsDisabledReason=Capabilities.WindowsReason;State.LinuxDisabledReason=Capabilities.LinuxReason;
    if(!FConvaiAvatarPackaging::ValidateOptions(PackageOptions(Options),Capabilities,Error)){Fail(Error);return false;}
    return true;
}

void FConvaiAvatarStudioController::RefreshUploadDefaults()
{
    if(State.bUploadDefaultsLoading)return;
    if(!UploadDefaults)UploadDefaults=MakeShared<FConvaiAvatarUploadDefaults>();
    State.bUploadDefaultsLoading=true;
    CachedDioramaLimits={}; bHasDioramaLimits=false;
    State.DioramaStatus.Empty(); State.DioramaIssues.Reset();
    State.DioramaErrorCount=0; State.DioramaWarningCount=0; State.bDioramaLimitsRead=false;
    State.UploadDefaultsStatus=TEXT("Reading Convai's published upload defaults...");
    Publish();
    UploadDefaults->Fetch([Weak=AsWeak()](FConvaiAvatarPublishedDefaults Defaults,FString Error)
    {
        const auto C=Weak.Pin();if(!C || C->bShuttingDown)return;
        C->State.bUploadDefaultsLoading=false;
        if(Error.IsEmpty())
        {
            C->CachedDioramaLimits=MoveTemp(Defaults.DioramaLimits);
            C->bHasDioramaLimits=Defaults.bHasDioramaLimits;
            C->State.bDefaultIncludeSource=Defaults.bIncludeSource && C->State.bSourceAvailable;
            C->State.bDefaultIncludeWindows=Defaults.bIncludeWindows && C->State.bWindowsAvailable;
            C->State.bDefaultIncludeLinux=Defaults.bIncludeLinux && C->State.bLinuxAvailable;
            C->State.UploadDefaultsStatus=TEXT("Convai's published defaults are loaded. You can change the upload choices.");
            C->State.UploadDefaultsStatus+=C->bHasDioramaLimits?TEXT(" Diorama limits loaded."):TEXT(" No diorama limits published.");
            if((Defaults.bIncludeWindows && !C->State.bWindowsAvailable) || (Defaults.bIncludeLinux && !C->State.bLinuxAvailable))
                C->State.UploadDefaultsStatus+=TEXT(" Platforms unavailable on this installation are left off.");
        }
        else
        {
            C->State.bDefaultIncludeSource=C->State.bSourceAvailable;
            C->State.bDefaultIncludeWindows=C->State.bWindowsAvailable;
            C->State.bDefaultIncludeLinux=false;
            C->State.UploadDefaultsStatus=TEXT("Published defaults are unavailable. Using local defaults; review the upload choices or refresh to try again.");
        }
        // SetState updates default values only; the view preserves any active create/update selection.
        C->Publish();
        if(auto View=C->Widget.Pin())View->RescanDiorama();
    });
}

void FConvaiAvatarStudioController::ScanDiorama(const FConvaiAvatarStudioCreateRequest& Request)
{
    check(IsInGameThread());
    if(bShuttingDown || State.bBusy)return;
    State.DioramaStatus.Empty(); State.DioramaIssues.Reset();
    State.DioramaErrorCount=0; State.DioramaWarningCount=0; State.bDioramaLimitsRead=false;
    if(!Request.bIncludeDiorama){Publish();return;}

    ShowDioramaReport(InspectDiorama(Request.BlueprintPath,Request.LevelPath,bHasDioramaLimits?&CachedDioramaLimits:nullptr));
}

void FConvaiAvatarStudioController::ShowDioramaReport(const FConvaiAvatarDioramaReport& Report)
{
    State.DioramaStatus=ConvaiAvatarDiorama::StatusLine(Report).ToString();
    State.bDioramaLimitsRead=Report.bLimitsRead;
    State.DioramaErrorCount=Report.Refusal.IsEmpty()?Report.Count(EConvaiAvatarDioramaSeverity::Error):1;
    State.DioramaWarningCount=Report.Count(EConvaiAvatarDioramaSeverity::Warning);
    State.DioramaIssues=Report.Issues;
    State.DioramaIssues.StableSort([](const FConvaiAvatarDioramaIssue& A,const FConvaiAvatarDioramaIssue& B)
    { return A.Severity>B.Severity; });
    Publish();
}

FConvaiAvatarDioramaReport FConvaiAvatarStudioController::InspectDiorama(const FString& BlueprintPath,const FString& LevelPath,const FConvaiAvatarDioramaLimits* Limits) const
{
    FConvaiAvatarDioramaReport Report;
    UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
    if(LevelPath.IsEmpty())Report.Refusal=TEXT("Choose the level your avatar is placed in");
    else if(!World || World->GetOutermost()->GetName()!=FPackageName::ObjectPathToPackageName(LevelPath))
        Report.Refusal=FString::Printf(TEXT("Open %s in the editor, then rescan. The selected level is not currently open."),*FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(LevelPath)));
    else if(World->IsPartitionedWorld())Report.Refusal=TEXT("World Partition levels are not supported. Choose a level without World Partition.");
    else if(BlueprintPath.IsEmpty())Report.Refusal=TEXT("Choose the Blueprint that represents your avatar before scanning the level.");
    else
    {
        UBlueprint* Blueprint=Cast<UBlueprint>(FSoftObjectPath(BlueprintPath).TryLoad());
        if(!Blueprint || !Blueprint->GeneratedClass)
            Report.Refusal=TEXT("The avatar Blueprint could not be read or has no generated class. Compile the Blueprint, then rescan.");
        else
        {
            Report.Facts=ConvaiAvatarDiorama::Inspect(World,Blueprint->GeneratedClass);
            Report.bLimitsRead=Limits!=nullptr;
            if(Limits)Report.Issues=ConvaiAvatarDiorama::Evaluate(Report.Facts,*Limits);
        }
    }
    return Report;
}

void FConvaiAvatarStudioController::WithFreshDioramaScan(FConvaiAvatarPrepareRequest Request,TFunction<void(FConvaiAvatarPrepareRequest)> Completion)
{
    if(!Request.bIncludeDiorama){Completion(MoveTemp(Request));return;}
    if(!UploadDefaults)UploadDefaults=MakeShared<FConvaiAvatarUploadDefaults>();
    const uint64 Op=Generation;
    bDioramaScanPending=true;State.bUploadDefaultsLoading=true;
    State.JobDetail=TEXT("Checking the level against Convai's latest diorama limits");Publish();
    if(Generation!=Op)return;
    UploadDefaults->Fetch([Weak=AsWeak(),Op,Request=MoveTemp(Request),Completion=MoveTemp(Completion)](FConvaiAvatarPublishedDefaults Defaults,FString Error) mutable
    {
        const auto Self=Weak.Pin();if(!Self || Self->bShuttingDown || Self->Generation!=Op)return;
        Self->bDioramaScanPending=false;Self->State.bUploadDefaultsLoading=false;
        if(!Error.IsEmpty()){Self->Fail(Error);return;}
        if(!Defaults.bHasDioramaLimits)
        {Self->Fail(TEXT("Convai has not published diorama limits, so a diorama cannot be checked"));return;}
        const auto Report=Self->InspectDiorama(Request.Blueprint.ToString(),Request.DioramaSourceLevel,&Defaults.DioramaLimits);
        Self->ShowDioramaReport(Report);
        if(!Report.Refusal.IsEmpty() || ConvaiAvatarDiorama::HasErrors(Report))
        {Self->Fail(ConvaiAvatarDiorama::StatusLine(Report).ToString());return;}
        if(Report.Facts.bDirty)
        {
            Self->Fail(FString::Printf(TEXT("Save %s before uploading - the diorama copy is made from what is on disk."),
                *FPackageName::GetShortName(Request.DioramaSourceLevel)));return;
        }
        if(Self->Generation!=Op || Self->bCancelRequested)return;
        Request.DioramaFacts=Report.Facts;
        Completion(MoveTemp(Request));
    });
}

void FConvaiAvatarStudioController::SaveMetadata(const FConvaiAvatarStudioMetadataRequest& InRequest)
{
    const auto Request=InRequest;
    if(State.bBusy || !EnsureClient())return;
    // Editing details never depends on a local Blueprint, a cook, or a source archive.
    const uint64 Op=BeginJob(TEXT("Save avatar details"),TEXT("Saving the name, Gender, and thumbnail"),false);
    FConvaiAvatarMetadataEdit Edit;Edit.Name=Request.DisplayName;Edit.Gender=Request.Gender;Edit.ThumbnailPath=Request.ThumbnailPath;
    Client->UpdateMetadata(Request.AssetId,Edit,[Weak=AsWeak(),Op,Request](FConvaiAvatarAssetWriteResult Result,FString Error)
    {
        auto C=Weak.Pin();if(!C || C->bShuttingDown || C->Generation!=Op)return;
        if(!Error.IsEmpty()){C->Fail(Error);return;}
        if(Result.Asset.AssetId!=Request.AssetId){C->Fail(TEXT("The save response did not identify the selected avatar. Refresh the library before trying again."));return;}
        for(auto& Asset:C->Assets)if(Asset.AssetId==Request.AssetId)
        {
            Asset.Name=Request.DisplayName.TrimStartAndEnd();Asset.Gender=Request.Gender;Asset.Metadata=Result.Asset.Metadata;
            if(!Request.ThumbnailPath.IsEmpty())Asset.SquareThumbnailUrl.Reset();
        }
        if(!Request.ThumbnailPath.IsEmpty())C->LocalThumbnails.Add(Request.AssetId,Request.ThumbnailPath);
        // Before the view sees the saved record: it invalidates the thumbnail URLs it still shows.
        if(auto View=C->Widget.Pin())View->FinishMetadataEdit(Request.AssetId);
        C->EndJob(TEXT("Avatar details saved. Uploaded files are unchanged."),FString());
        C->Refresh();
    });
}

void FConvaiAvatarStudioController::BrowseContent(const FString& InId)
{
    const FString Id=InId;
    if(State.bBusy)return;
    FConvaiAvatarPreparedAsset Prepared;FString Error;bool bReady=false;
    if(!FConvaiAvatarWorkspace::FindLocalAssetRecord(Id,Prepared,bReady,Error)){Fail(Error);return;}
    FSoftObjectPath Blueprint;
    if(!ConvaiAvatarSourceSelection::ResolveEditableBlueprint(Prepared,Blueprint,Error)){Fail(Error);return;}
    const FAssetData Asset=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(Blueprint);
    if(!Asset.IsValid()){Fail(TEXT("This avatar's Blueprint could not be found. Refresh the library and finish its preparation or download first."));return;}
    FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{Asset});
}

void FConvaiAvatarStudioController::OpenLog(const FString& Kind)
{
    FString Path=DiagnosticPaths.FindRef(Kind);
    if(Path.IsEmpty())Path=DiagnosticPaths.FindRef(TEXT("latest"));
    if(Path.IsEmpty())return;
    Path=FPaths::ConvertRelativePathToFull(Path);
    if(IFileManager::Get().FileExists(*Path))FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
}

void FConvaiAvatarStudioController::OpenDocumentation()
{
    FPlatformProcess::LaunchURL(TEXT("https://docs.convai.com/api-docs/plugins-and-integrations/asset-uploader"), nullptr, nullptr);
}

void FConvaiAvatarStudioController::RefreshPublishedAsset(const FString& AssetId)
{
    if (AssetId.IsEmpty() || !Client) return;
    const uint64 Op = Generation;
    Client->GetAsset(AssetId, [Weak=AsWeak(), Op, AssetId](FConvaiAvatarAsset Asset, FString Error)
    {
        const auto Self = Weak.Pin();
        if (!Self || Self->bShuttingDown || Self->Generation != Op || !Error.IsEmpty() || Self->LibraryState.IsDeleted(AssetId)) return;
        if (auto* Existing = Self->Assets.FindByPredicate([&](const FConvaiAvatarAsset& Item) { return Item.AssetId == AssetId; }))
        {
            FConvaiAvatarAssetsClient::PreserveOptionalSquareThumbnail(*Existing,Asset);
            *Existing = MoveTemp(Asset);
            Self->DetailedAssetIds.Add(AssetId);
        }
        Self->Publish();
    });
}

void FConvaiAvatarStudioController::Refresh()
{
    if (State.bBusy || !EnsureClient()) return;
    if (ResumePendingDownloads()) return;
    bLibraryListed = false;
    RefreshLocalRecords();
    RefreshUploadDefaults();
    RefreshRemoteConfiguration();
    const uint64 Op = ++Generation;
    ResetLibraryDetails();
    State.bLoading = true; State.Error.Empty(); Publish();
    Client->ListAvatars([Weak=AsWeak(),Op](TArray<FConvaiAvatarAsset> Records, FString Error)
    {
        auto Self=Weak.Pin(); if (!Self || Self->bShuttingDown || Self->Generation!=Op) return;
        if (!Error.IsEmpty()) { Self->Fail(Error); return; }
        TMap<FString,const FConvaiAvatarAsset*> Previous;
        for(const auto& Existing:Self->Assets)Previous.Add(Existing.AssetId,&Existing);
        for(auto& Record:Records)if(const auto* Existing=Previous.Find(Record.AssetId))
            FConvaiAvatarAssetsClient::PreserveOptionalSquareThumbnail(**Existing,Record);
        Self->Assets=MoveTemp(Records);
        Self->LibraryState.RemoveDeleted(Self->Assets);
        Self->bLibraryListed=true;
        Self->RetryUnboundBindings();
        Self->State.bLoading=false; Self->Publish();
        // Images arrive independently; only the existing private asset allowlist
        // may receive the one batched Avatar API response's presentation fields.
        Self->Client->ListAvatarImages(Self->Assets,[Weak,Op,Account=Self->AuthFingerprint](TArray<FConvaiAvatarImageRecord> Images,FString ImageError)
        {
            if(const auto Owner=Weak.Pin())Owner->CompleteAvatarImages(Op,Account,MoveTemp(Images),MoveTemp(ImageError));
        });
        Self->RequestSelectedDetails(Self->State.SelectedAssetId);
        Self->Publish();
    });
}

void FConvaiAvatarStudioController::Create(const FConvaiAvatarStudioCreateRequest& InRequest)
{
    if(State.bBusy || State.bRemoteConfigurationLoading || !EnsureClient())return;
    const auto Request=InRequest;
    // The view whose Submit this is: a tab reopened during the engine check must not stand in for it.
    RefreshRemoteConfiguration(false,[Weak=AsWeak(),Request,View=Widget]{if(auto Self=Weak.Pin())Self->CreateReviewed(Request,View);});
}

bool FConvaiAvatarStudioController::ReviewUnsavedPackages(FConvaiAvatarPrepareRequest& Request,FString& Error)
{
    Request.NativeRuntimeContract = Client && (Client->GetAssetsRoot() == TEXT("https://beta.convai.com/assets") || Client->GetAssetsRoot() == TEXT("https://beta.convai.com/assets/")) ? RemoteConfigurationData.NativeRuntimeContract : FString();
    const uint64 Op=Generation;
    TArray<UPackage*> Packages;
    const bool bCollected=FConvaiAvatarWorkspace::CollectDirtyPackages(Request,Packages,Error);
    if(bShuttingDown || Generation!=Op){Error.Reset();return false;}
    if(!bCollected)return false;
    if(Packages.IsEmpty())return true;
    // Unreal can add owner external packages to its save list. Never expand consent beyond this review.
    TArray<UPackage*> UnreviewedExternal;
    for(UPackage* Package:Packages)
        for(UPackage* External:Package->GetExternalPackages())
            if(External && !Packages.Contains(External))UnreviewedExternal.AddUnique(External);
    if(!UnreviewedExternal.IsEmpty())
    {Error=TEXT("This avatar uses external asset files that need to be saved in their editor before uploading:\n")+ConvaiAvatarSaveReview::PackageList(UnreviewedExternal);return false;}
    // Unreal's unattended-script path bypasses its save dialog, so refuse it explicitly.
    if(FApp::IsUnattended() || GIsRunningUnattendedScript)
    {Error=TEXT("Save these avatar files in the editor before uploading:\n")+ConvaiAvatarSaveReview::PackageList(Packages);return false;}

    TArray<TStrongObjectPtr<UPackage>> KeepAlive;
    for(UPackage* Package:Packages)KeepAlive.Emplace(Package);
    TArray<UPackage*> FailedPackages;
    FEditorFileUtils::FPromptForCheckoutAndSaveParams Params;
    // The collector already filters this list, including never-saved assets whose dirty flag may be clear.
    Params.bCheckDirty=false;Params.bPromptToSave=true;Params.bCanBeDeclined=false;Params.bIsExplicitSave=true;
    Params.Title=FText::FromString(TEXT("Save avatar changes"));
    Params.Message=FText::FromString(TEXT("Save these avatar files to continue uploading. Cancel keeps your unsaved changes."));
    Params.OutFailedPackages=&FailedPackages;
    FEditorFileUtils::EPromptReturnCode Result;
    {
        TGuardValue<bool> LocalWork(bLocalWorkActive,true);
        Result=FEditorFileUtils::PromptForCheckoutAndSave(Packages,Params);
    }
    const auto Outcome=ConvaiAvatarSaveReview::Evaluate(Result,Packages,FailedPackages,!bShuttingDown && Generation==Op,
        bCancelRequested || (Request.IsCancelled && Request.IsCancelled()),Error);
    if(Outcome==ConvaiAvatarSaveReview::EOutcome::Superseded)return false;
    if(Outcome==ConvaiAvatarSaveReview::EOutcome::Cancelled)
    {EndJob(TEXT("Upload cancelled. Your current changes have been kept."),FString());return false;}
    if(Outcome!=ConvaiAvatarSaveReview::EOutcome::Continue)return false;
    ConvaiAvatarSaveReview::ScanSaved(Packages);
    // Saving can change the dependency graph. Recollect once; do not open a recursive save prompt.
    TArray<UPackage*> Remaining;
    const bool bRecollected=FConvaiAvatarWorkspace::CollectDirtyPackages(Request,Remaining,Error);
    if(bShuttingDown || Generation!=Op){Error.Reset();return false;}
    if(!bRecollected)return false;
    if(!Remaining.IsEmpty())
    {Error=TEXT("More avatar files have unsaved changes. Save them, then retry the upload.\n")+ConvaiAvatarSaveReview::PackageList(Remaining);return false;}
    return !bShuttingDown && Generation==Op && !bCancelRequested;
}

bool FConvaiAvatarStudioController::ReviewMissingPackages(FConvaiAvatarPrepareRequest& Request,FString& Error)
{
    FConvaiAvatarDependencyReview Review;
    if(!FConvaiAvatarWorkspace::ReviewDependencies(Request,Review,Error))return false;
    if(!Review.bRequiresAcknowledgement)return true;
    TArray<FString> Paths;
    for(const FName Package:Review.MissingPackages)Paths.Add(Package.ToString());
    bool bInteractiveTest=false;
#if WITH_DEV_AUTOMATION_TESTS
    FString BlacksmithVariant;
    FParse::Value(FCommandLine::Get(),TEXT("AvatarStudioTestCloudBlacksmith="),BlacksmithVariant);
    const FString BlacksmithBlueprint=BlacksmithVariant==TEXT("NPC")
        ? TEXT("/Game/MetaHumans/Blacksmith/BP_Blacksmith_NPC.BP_Blacksmith_NPC")
        : BlacksmithVariant==TEXT("ENV") ? TEXT("/Game/MetaHumans/Blacksmith/BP_Blacksmith_NPC_ENV.BP_Blacksmith_NPC_ENV") : FString();
    const bool bBlacksmithTest=!BlacksmithBlueprint.IsEmpty()
        && FString(FApp::GetProjectName())==TEXT("Viking_Assetuploader")
        && FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),
            TEXT("F:/Work/Convai/Mohamed_ConvaiShared/AssetUploader_Limited ENV/Viking_Assetuploader/Viking_Assetuploader.uproject"))
        && Request.Blueprint.ToString()==BlacksmithBlueprint;
    const bool bTutorialTest=FParse::Param(FCommandLine::Get(),TEXT("AvatarStudioTestCloudTutorial"))
        && FString(FApp::GetProjectName())==TEXT("TourGuideTutorial")
        && Request.Blueprint.ToString()==TEXT("/Game/MetaHumans/Female1/BP_Female1_NPC.BP_Female1_NPC");
    FString DioramaMode;
    FParse::Value(FCommandLine::Get(),TEXT("AvatarStudioTestDiorama="),DioramaMode);
    const bool bDioramaTest=DioramaMode==TEXT("on")
        && FString(FApp::GetProjectName())==TEXT("Tut_Assetuploader")
        && FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),
            TEXT("E:/Perforce/Anmol_ConvaiShared/AssetUploader_Limited ENV/Factory_Assetuploader/Tut_Assetuploader.uproject"))
        && Request.Blueprint.ToString()==TEXT("/Game/MetaHumans/FactoryWorker_M/BP_FactoryWorker_M_NPC.BP_FactoryWorker_M_NPC");
    FString PhysicsBlueprint;
    const bool bPhysicsTest=FParse::Value(FCommandLine::Get(),TEXT("AvatarStudioTestCloudPhysics="),PhysicsBlueprint)
        && Request.Blueprint.ToString()==PhysicsBlueprint
        && Request.Blueprint.GetLongPackageName().StartsWith(TEXT("/Game/AvatarAnimatedDioramaQA_"))
        && FPaths::FileExists(FPaths::ProjectDir()/TEXT(".codex/avatar-studio-fixture-project.json"));
    bInteractiveTest=FParse::Param(FCommandLine::Get(),TEXT("AvatarStudioTestCloudJourney"))
        && FParse::Param(FCommandLine::Get(),TEXT("RenderOffscreen"))
        && !FParse::Param(FCommandLine::Get(),TEXT("unattended"))
        && (bTutorialTest || bBlacksmithTest || bDioramaTest || bPhysicsTest);
#endif
    if(FApp::IsUnattended() && !bInteractiveTest)
    {Error=TEXT("Review the missing files in Cloud Avatars before uploading:\n")+FString::Join(Paths,TEXT("\n"));return false;}

    bool bContinue=false;
    const auto Dialog=SNew(SWindow).Title(FText::FromString(TEXT("Review missing avatar files")))
        .ClientSize(FVector2D(760,460)).SupportsMinimize(false).SupportsMaximize(false);
    const TWeakPtr<SWindow> WeakDialog=Dialog;
    Dialog->SetContent(SNew(SBox).Padding(20)
        [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)
        [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(FString::Printf(
            TEXT("This avatar refers to %d files that are no longer in the project. They may be old references, or files the avatar still needs. Review the list below."),Paths.Num())))]
        +SVerticalBox::Slot().FillHeight(1)
        [SNew(SScrollBox)+SScrollBox::Slot()
            [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(FString::Join(Paths,TEXT("\n\n")))).Tag(TEXT("AvatarStudio.MissingPackages.List"))]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,16,0,16)
        [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(
            TEXT("Continue to upload without these files, or cancel to fix the references first. Missing files can affect the avatar's appearance or prevent packaging.")))]
        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
        [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)
            [SNew(SButton).ContentPadding(FMargin(16,8)).Text(FText::FromString(TEXT("Cancel")))
                .OnClicked_Lambda([WeakDialog]{if(auto Window=WeakDialog.Pin())Window->RequestDestroyWindow();return FReply::Handled();})]
            +SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).ContentPadding(FMargin(16,8)).Text(FText::FromString(TEXT("Continue without these files")))
                .OnClicked_Lambda([WeakDialog,&bContinue]{bContinue=true;if(auto Window=WeakDialog.Pin())Window->RequestDestroyWindow();return FReply::Handled();})]]]);
    TSharedPtr<SWindow> Parent;
    if(auto View=Widget.Pin())Parent=FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef());
    FSlateApplication::Get().AddModalWindow(Dialog,Parent);
    if(!bContinue){Error.Reset();EndJob(TEXT("Upload cancelled. Your avatar is unchanged."),FString());return false;}
    for(FName Package:Review.MissingPackages)Request.AcknowledgedMissingPackages.AddUnique(Package);
    return true;
}

bool FConvaiAvatarStudioController::ReviewSourceChanges(FConvaiAvatarPrepareRequest& Request,FString& Error)
{
    FConvaiAvatarSourceReview Review;
    if(!FConvaiAvatarWorkspace::ReviewSourceChanges(Request,Review,Error))return false;
    if(!Review.bRequiresConfirmation)return true;
    if(FApp::IsUnattended())
    {Error=TEXT("Review the changed avatar files in Cloud Avatars before uploading. The project Blueprint and prepared files were kept.");return false;}
    if(Review.ConflictFingerprint.IsEmpty())
    {Error=TEXT("The changed avatar files could not be verified. Retry the upload to review them again.");return false;}

    const uint64 Op=Generation;
    bool bUseOriginal=false;
    const FString Explanation=TEXT("Review changes to the generated avatar before refreshing it from your project Blueprint.");
    FString Files;
    if(!Review.ChangedOriginalFiles.IsEmpty())Files=TEXT("Changed project files\n")+FString::Join(Review.ChangedOriginalFiles,TEXT("\n"));
    if(!Review.ChangedPreparedFiles.IsEmpty())
    {
        if(!Files.IsEmpty())Files+=TEXT("\n\n");
        Files+=TEXT("Changed generated avatar files\n")+FString::Join(Review.ChangedPreparedFiles,TEXT("\n"));
    }
    const auto Dialog=SNew(SWindow).Title(FText::FromString(TEXT("Review avatar changes")))
        .ClientSize(FVector2D(780,500)).SupportsMinimize(false).SupportsMaximize(false);
    const TWeakPtr<SWindow> WeakDialog=Dialog;
    Dialog->SetContent(SNew(SBox).Padding(20)
        [SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)
        [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Explanation))]
        +SVerticalBox::Slot().FillHeight(1)
        [SNew(SScrollBox)+SScrollBox::Slot()
            [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Files)).Tag(TEXT("AvatarStudio.SourceConflict.Files"))]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,16,0,16)
        [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(
            TEXT("Use the project Blueprint to refresh the generated avatar. This can overwrite edits made only to the generated copy. Your project Blueprint stays unchanged.")))]
        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
        [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)
            [SNew(SButton).ContentPadding(FMargin(16,8)).Text(FText::FromString(TEXT("Cancel")))
                .Tag(TEXT("AvatarStudio.SourceConflict.Cancel"))
                .OnClicked_Lambda([WeakDialog]{if(auto Window=WeakDialog.Pin())Window->RequestDestroyWindow();return FReply::Handled();})]
            +SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).ContentPadding(FMargin(16,8)).Text(FText::FromString(TEXT("Use project Blueprint")))
                .Tag(TEXT("AvatarStudio.SourceConflict.UseOriginal"))
                .OnClicked_Lambda([WeakDialog,&bUseOriginal]{bUseOriginal=true;if(auto Window=WeakDialog.Pin())Window->RequestDestroyWindow();return FReply::Handled();})]]]);
    TSharedPtr<SWindow> Parent;
    if(auto View=Widget.Pin())Parent=FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef());
    FSlateApplication::Get().AddModalWindow(Dialog,Parent);
    if(bShuttingDown || Generation!=Op || bCancelRequested || (Request.IsCancelled && Request.IsCancelled()))
    {Error=TEXT("Avatar preparation was cancelled.");return false;}
    if(!bUseOriginal){Error.Reset();EndJob(TEXT("Upload cancelled. Your avatar files were kept."),FString());return false;}
    Request.AcknowledgedSourceConflict=Review.ConflictFingerprint;
    return true;
}

void FConvaiAvatarStudioController::CreateReviewed(const FConvaiAvatarStudioCreateRequest& InRequest,const TWeakPtr<SConvaiAvatarStudio>& RequestingView)
{
    FConvaiAvatarStudioCreateRequest Request=InRequest;
    if (State.bBusy || !EnsureClient()) return;
    Request.bIncludeConvaiContent |= ConvaiAvatarStudio::BlueprintSetup::HasCustomAnimationBlueprint(Cast<UBlueprint>(FSoftObjectPath(Request.BlueprintPath).TryLoad()));
    FConvaiAvatarStudioUploadOptions Options;Options.bIncludeSource=Request.bIncludeSource;Options.bIncludeWindows=Request.bIncludeWindows;Options.bIncludeLinux=Request.bIncludeLinux;Options.bIncludeConvaiContent=Request.bIncludeConvaiContent;
    if(!ValidateUploadOptions(Options))return;
    if(const FConvaiAvatarUnboundBinding* Known=UnboundBindings.Find(Request.DraftId))
    {
        Fail(TEXT("This draft already created cloud avatar ")+Known->CloudId+TEXT(". Its local record could not be saved. Resolve the reported file error and refresh the library before retrying."));return;
    }
    if (GEditor && GEditor->PlayWorld) { Fail(TEXT("Stop Play mode before creating and uploading an avatar.")); return; }
    FConvaiAvatarAssetWrite Preflight;
    Preflight.Name=Request.DisplayName;Preflight.ThumbnailPath=Request.ThumbnailPath;Preflight.Gender=Request.Gender;
    Preflight.Version=FConvaiAvatarAssetsClient::MakeVersion(Options.bIncludeWindows?TEXT("Windows"):(Options.bIncludeLinux?TEXT("Linux"):TEXT("Raw")));
    Preflight.Tags={TEXT("Pak"),TEXT("Avatar")};
    TArray<uint8> CheckedRequest;FString PreflightError;TSharedPtr<FJsonObject> CheckedMetadata;
    if(!FConvaiAvatarAssetsClient::BuildWriteMetadata(Preflight,CheckedMetadata,PreflightError)
        || (!Request.ThumbnailPath.IsEmpty() && !FConvaiAvatarAssetsClient::BuildMultipart(Preflight,TEXT("ConvaiThumbnailPreflight"),CheckedRequest,PreflightError)))
    {Fail(PreflightError);return;}
    if(Request.ThumbnailPath.IsEmpty() && !FConvaiAvatarPortraitCapture::IsAvailable(PreflightError)){Fail(PreflightError);return;}
    bCancelRequested=false;
    const uint64 Op=BeginJob(TEXT("Create avatar"), TEXT("Preparing the Blueprint and its dependencies"),true);
    if(Generation!=Op)return;
    FConvaiAvatarPrepareRequest Prep;
    Prep.AssetId=Request.DraftId.IsEmpty()?FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens):Request.DraftId;
    Prep.DisplayName=Request.DisplayName;
    Prep.Blueprint=FSoftObjectPath(Request.BlueprintPath);
    Prep.bRefreshFromSource=true;
    Prep.bIsNewDraft=true;
    Prep.bIsMetaHuman=Request.bIsMetaHuman;
    Prep.bIncludeConvaiContent=Request.bIncludeConvaiContent;
    Prep.bIncludeDiorama=Request.bIncludeDiorama;
    if(Prep.bIncludeDiorama)Prep.DioramaSourceLevel=FSoftObjectPath(Request.LevelPath).GetLongPackageName();
    Prep.IsCancelled=[Weak=AsWeak(),Op]{const auto C=Weak.Pin();return !C || C->bShuttingDown || C->bCancelRequested || C->Generation!=Op;};
    WithFreshDioramaScan(MoveTemp(Prep),[Weak=AsWeak(),Op,Request,Options,Preflight,RequestingView](FConvaiAvatarPrepareRequest Prep) mutable
    {
        const auto Self=Weak.Pin();if(!Self || Self->bShuttingDown || Self->Generation!=Op)return;
        // The reviews and Prepare read the same files: each is hashed once unless it changes in between.
        const ConvaiAvatarSourceRevision::FHashMemoScope HashMemo;
        FConvaiAvatarPreparedAsset Prepared; FString Error;
        if(!Self->ReviewUnsavedPackages(Prep,Error)){if(!Error.IsEmpty())Self->FailLocal(Error);return;}
        if(Self->Generation!=Op)return;
        if(!Self->ReviewSourceChanges(Prep,Error)){if(!Error.IsEmpty())Self->FailLocal(Error);return;}
        if(Self->Generation!=Op)return;
        if(!Self->ReviewMissingPackages(Prep,Error)){if(!Error.IsEmpty())Self->FailLocal(Error);return;}
        if(Self->Generation!=Op)return;
        Self->bLocalWorkActive=true;
        const bool bPrepared=FConvaiAvatarWorkspace::Prepare(Prep,Prepared,Error);
        Self->bLocalWorkActive=false;
        Self->RefreshLocalRecords();
        if(!bPrepared || Self->bCancelRequested){Self->FailLocal(Self->bCancelRequested?TEXT("Preparation cancelled. Your original avatar is unchanged."):Error);return;}
        if(Self->Generation!=Op)return;
        FConvaiAvatarAsset NewAsset;NewAsset.Gender=Request.Gender;
        if(!Request.ThumbnailPath.IsEmpty())
        {
            Self->BeginPublish(Prepared,NewAsset,Request.ThumbnailPath,Options);
            return;
        }
        FSoftObjectPath CaptureBlueprint;
        if(!ConvaiAvatarSourceSelection::ResolveEditableBlueprint(Prepared,CaptureBlueprint,Error)){Self->Fail(Error);return;}
        Self->CapturePortrait(CaptureBlueprint,Prepared.bIsMetaHuman,[Weak,Op,Request,Prepared,NewAsset,Options,Preflight,RequestingView](FString File) mutable
        {
            const auto Owner=Weak.Pin();if(!Owner || Owner->bShuttingDown || Owner->Generation!=Op)return;
            Preflight.ThumbnailPath=File;
            TArray<uint8> Checked;FString Error;
            if(!FConvaiAvatarAssetsClient::BuildMultipart(Preflight,TEXT("ConvaiThumbnailPreflight"),Checked,Error)){Owner->Fail(Error);return;}
            if(!Owner->AcceptCapturedCreateThumbnail(RequestingView,Request,File))
            {Owner->Fail(TEXT("The avatar selection changed during capture. Review it before uploading."));return;}
            Owner->BeginPublish(Prepared,NewAsset,File,Options);
        });
    });
}

bool FConvaiAvatarStudioController::AcceptCapturedCreateThumbnail(const TWeakPtr<SConvaiAvatarStudio>& RequestingView,const FConvaiAvatarStudioCreateRequest& Request,const FString& File)
{
    const auto View=RequestingView.Pin();
    // A closed or replaced tab leaves nobody to review the capture, as with the tab closed during the create.
    return !View || View!=Widget.Pin() || View->SetCapturedCreateThumbnail(Request,File);
}

void FConvaiAvatarStudioController::CapturePortrait(const FSoftObjectPath& Blueprint,bool bIsMetaHuman,TFunction<void(FString)> Completion)
{
    FString Reason;
    if(!FConvaiAvatarPortraitCapture::IsAvailable(Reason)){Fail(Reason);return;}
    const uint64 Op=Generation;
    State.bLoading=false; // Any older library request belongs to the previous operation.
    SetJob(ThumbnailCaptureTitle,ThumbnailCaptureDetail);
    PortraitCapture=MakeShared<FConvaiAvatarPortraitCapture>();
    // Start can report a setup error immediately. Keep the service alive through that callback.
    const auto Capture=PortraitCapture;
    Capture->Start(Blueprint,bIsMetaHuman,[Weak=AsWeak(),Op,Completion=MoveTemp(Completion)](FString File,FString Error) mutable
    {
        const auto Self=Weak.Pin();if(!Self || Self->bShuttingDown || Self->Generation!=Op)return;
        const auto CompletedCapture=Self->PortraitCapture;
        Self->PortraitCapture.Reset();
        if(!Error.IsEmpty()){Self->Fail(Error);return;}
        Completion(MoveTemp(File));
    });
}

void FConvaiAvatarStudioController::CaptureCreatePortrait(const FConvaiAvatarStudioCreateRequest& InRequest)
{
    if(State.bBusy || bShuttingDown || InRequest.BlueprintPath.IsEmpty())return;
    const auto Request=InRequest;
    bCancelRequested=false;BeginJob(ThumbnailCaptureTitle,ThumbnailCaptureDetail,true);
    CapturePortrait(FSoftObjectPath(Request.BlueprintPath),Request.bIsMetaHuman,[Weak=AsWeak(),Request](FString File)
    {
        if(const auto Self=Weak.Pin())Self->CompleteCreateCapture(Request,File);
    });
}

void FConvaiAvatarStudioController::CompleteCreateCapture(const FConvaiAvatarStudioCreateRequest& Request,const FString& File)
{
    bool bAccepted=false;
    if(auto View=Widget.Pin())bAccepted=View->SetCapturedCreateThumbnail(Request,File);
    EndJob(bAccepted?TEXT("Thumbnail captured. Review it before uploading."):TEXT("The selection changed. Capture again for the current avatar."),FString());
}

void FConvaiAvatarStudioController::CaptureMetadataPortrait(const FConvaiAvatarStudioMetadataRequest& InRequest)
{
    if(State.bBusy || bShuttingDown)return;
    const auto Request=InRequest;
    const auto* Asset=Assets.FindByPredicate([&](const auto& Item){return Item.AssetId==Request.AssetId && Item.IsPrivate();});
    const auto* Local=LocalRecords.Find(Request.AssetId);
    if(!Asset || !Local || IncompleteLocalIds.Contains(Request.AssetId))
    {Fail(TEXT("Download this avatar's source into this project before capturing its thumbnail."));return;}
    FSoftObjectPath Blueprint;FString Error;
    if(!ConvaiAvatarSourceSelection::ResolveEditableBlueprint(*Local,Blueprint,Error)){Fail(Error);return;}
    const bool bIsMetaHuman=Local->bHasMetaHumanChoice?Local->bIsMetaHuman:
        ConvaiAvatarStudio::BlueprintSetup::IsMetaHuman(Cast<UBlueprint>(Blueprint.TryLoad()));
    bCancelRequested=false;BeginJob(ThumbnailCaptureTitle,ThumbnailCaptureDetail,true);
    CapturePortrait(Blueprint,bIsMetaHuman,[Weak=AsWeak(),Request](FString File)
    {
        if(const auto Self=Weak.Pin())Self->CompleteMetadataCapture(Request,File);
    });
}

void FConvaiAvatarStudioController::CompleteMetadataCapture(const FConvaiAvatarStudioMetadataRequest& Request,const FString& File)
{
    bool bAccepted=false;
    if(auto View=Widget.Pin())bAccepted=View->SetCapturedMetadataThumbnail(Request,File);
    EndJob(bAccepted?TEXT("Thumbnail captured. Save details to update it in your library."):TEXT("The selection changed. Capture again for the current avatar."),FString());
}

void FConvaiAvatarStudioController::UploadChanges(const FString& InId, const FConvaiAvatarStudioUploadOptions& InOptions)
{
    if(State.bBusy || State.bRemoteConfigurationLoading || !EnsureClient())return;
    const FString Id=InId;const auto Options=InOptions;
    RefreshRemoteConfiguration(false,[Weak=AsWeak(),Id,Options]{if(auto Self=Weak.Pin())Self->UploadChangesReviewed(Id,Options);});
}

void FConvaiAvatarStudioController::UploadChangesReviewed(const FString& InId, const FConvaiAvatarStudioUploadOptions& InOptions)
{
    const FString Id=InId;
    auto Options=InOptions;
    if (State.bBusy || !EnsureClient()) return;
    if(!ValidateUploadOptions(Options))return;
    if (GEditor && GEditor->PlayWorld) { Fail(TEXT("Stop Play mode before uploading changes.")); return; }
    bCancelRequested=false;
    const uint64 Op=BeginJob(TEXT("Upload changes"), TEXT("Checking the current avatar record"),true);
    if(Generation!=Op)return;
    Client->GetAsset(Id,[Weak=AsWeak(),Op,Options](FConvaiAvatarAsset Asset,FString Error) mutable
    {
        auto Self=Weak.Pin(); if (!Self || Self->Generation!=Op) return;
        if (!Error.IsEmpty()) { Self->Fail(Error); return; }
        FConvaiAvatarPreparedAsset Prepared;
        bool bReady=false;
        if (!FConvaiAvatarWorkspace::FindLocalAssetRecord(Asset.AssetId,Prepared,bReady,Error)) { Self->FailLocal(Error); return; }
        FConvaiAvatarPrepareRequest Request;
        if(!ConvaiAvatarSourceSelection::BuildUploadRequest(Prepared,bReady,Request,Error)){Self->FailLocal(Error);return;}
        Options.bIncludeConvaiContent |= ConvaiAvatarStudio::BlueprintSetup::HasCustomAnimationBlueprint(Cast<UBlueprint>(Request.Blueprint.TryLoad()));
        Request.bIncludeConvaiContent=Options.bIncludeConvaiContent;
        Request.DisplayName=Asset.Name;
        Request.bIncludeDiorama=Prepared.Diorama.IsSet();
        if(Request.bIncludeDiorama)Request.DioramaSourceLevel=DioramaSourceLevel(Prepared);
        Request.IsCancelled=[Weak,Op]{const auto C=Weak.Pin();return !C || C->bShuttingDown || C->bCancelRequested || C->Generation!=Op;};
        Self->WithFreshDioramaScan(MoveTemp(Request),[Weak,Op,Asset,Options](FConvaiAvatarPrepareRequest Request)
        {
            const auto Owner=Weak.Pin();if(!Owner || Owner->bShuttingDown || Owner->Generation!=Op)return;
            const ConvaiAvatarSourceRevision::FHashMemoScope HashMemo;
            FConvaiAvatarPreparedAsset Prepared;FString Error;
            if(!Owner->ReviewUnsavedPackages(Request,Error)){if(!Error.IsEmpty())Owner->FailLocal(Error);return;}
            if(Owner->Generation!=Op)return;
            if(!Owner->ReviewSourceChanges(Request,Error)){if(!Error.IsEmpty())Owner->FailLocal(Error);return;}
            if(Owner->Generation!=Op)return;
            if(!Owner->ReviewMissingPackages(Request,Error)){if(!Error.IsEmpty())Owner->FailLocal(Error);return;}
            if(Owner->Generation!=Op)return;
            Owner->bLocalWorkActive=true;
            const bool bPrepared=FConvaiAvatarWorkspace::Prepare(Request,Prepared,Error);
            Owner->bLocalWorkActive=false;
            if(!bPrepared || Owner->bCancelRequested){Owner->FailLocal(Owner->bCancelRequested?TEXT("Preparation cancelled."):Error);return;}
            if(Owner->Generation!=Op)return;
            Owner->BeginPublish(Prepared,Asset,FString(),Options);
        });
    });
}

TSharedPtr<FJsonObject> FConvaiAvatarStudioController::MetadataFor(const FConvaiAvatarPreparedAsset& Prepared,const FConvaiAvatarAsset& Existing)
{
    auto Meta=MakeShared<FJsonObject>();
    if (Existing.Metadata) Meta->Values=Existing.Metadata->Values;
    Meta->SetStringField(TEXT("asset_type"),TEXT("avatar"));
    Meta->SetBoolField(TEXT("is_metahuman"),Prepared.bIsMetaHuman);
    ConvaiAvatarDiorama::ApplyRecordTo(Meta,Prepared.Diorama);
    Meta->SetStringField(TEXT("project_name"),TEXT("AvatarStudioUploader"));
    Meta->SetStringField(TEXT("plugin_name"),Prepared.PluginName);
    Meta->SetStringField(TEXT("root_path"),TEXT("/")+Prepared.PluginName+TEXT("/"));
    Meta->SetStringField(TEXT("content_path"),TEXT("../../../AvatarStudioUploader/Plugins/ConvaiAvatars/")+Prepared.PluginName+TEXT("/Content/"));
    Meta->SetStringField(TEXT("blueprint_class_path"),Prepared.EntryPoint.GetLongPackageName());
    Meta->SetStringField(TEXT("blueprint_class"),TEXT("/Script/Engine.BlueprintGeneratedClass'")+Prepared.EntryPoint.ToString()+TEXT("_C'"));
    Meta->SetStringField(TEXT("asset_name"),Prepared.DisplayName);
    if (!Prepared.NativeRuntimeContract.IsEmpty()) Meta->SetStringField(TEXT("native_runtime_contract"),Prepared.NativeRuntimeContract);
    else Meta->RemoveField(TEXT("native_runtime_contract"));
    if(!Meta->HasField(TEXT("asset_description")))Meta->SetStringField(TEXT("asset_description"),TEXT("Uploaded from Convai Cloud Avatars"));
    if(!Meta->HasField(TEXT("level_name")))Meta->SetStringField(TEXT("level_name"),FString());
    return Meta;
}
void FConvaiAvatarStudioController::BeginPublish(FConvaiAvatarPreparedAsset Prepared,FConvaiAvatarAsset Existing,const FString& Thumbnail,const FConvaiAvatarStudioUploadOptions& Options)
{
    if (!Packaging) Packaging=MakeShared<FConvaiAvatarPackaging,ESPMode::ThreadSafe>();
    CurrentPublish=MakeShared<FConvaiAvatarPublishRun,ESPMode::ThreadSafe>();
    CurrentPublish->Prepared=Prepared;CurrentPublish->Options=Options;CurrentPublish->AssetId=Existing.AssetId;
    CurrentPublish->Gender=Existing.Gender;CurrentPublish->Thumbnail=Thumbnail;
    const uint64 Op=Generation;
    SetJob(Existing.AssetId.IsEmpty()?TEXT("Create avatar"):TEXT("Upload changes"),
        (Options.bIncludeWindows || Options.bIncludeLinux)?TEXT("Preparing the selected packages"):TEXT("Preparing source without packaging"));
    if(Prepared.Diorama){State.Notice=DioramaNotice(Prepared);Publish();}
    bLocalWorkActive=true;
    auto PackagingOptions=PackageOptions(Options);
    PackagingOptions.AssetsRoot=Client ? Client->GetAssetsRoot() : FString();
    Packaging->Start(Prepared,PackagingOptions,[Weak=AsWeak(),Op](FConvaiAvatarPackageResult Result,FString Error)
    {
        auto Self=Weak.Pin(); if (!Self || Self->bShuttingDown || Self->Generation!=Op || !Self->CurrentPublish) return;
        Self->bLocalWorkActive=false;
        if(!Result.LogPath.IsEmpty())
        {
            Self->DiagnosticPaths.Add(TEXT("packaging"),Result.LogPath);
            Self->DiagnosticPaths.Add(TEXT("latest"),Result.LogPath);Self->State.bCanOpenLog=true;
            Self->State.DiagnosticReference=FPaths::GetCleanFilename(Result.LogPath);
        }
        if(Self->bCancelRequested){Self->Fail(TEXT("Preparation cancelled."));return;}
        if (!Error.IsEmpty()) { Self->Fail(Error,TEXT("The selected upload files could not be prepared. Open the latest log for details, then retry.")); return; }
        Self->CurrentPublish->Package=MoveTemp(Result);
        Self->PreparePublishArtifacts();
    },[Weak=AsWeak(),Op](FString Detail)
    {
        if (auto Self=Weak.Pin(); Self && Self->Generation==Op) { Self->State.JobDetail=Detail; Self->Publish(); }
    });
}

void FConvaiAvatarStudioController::PreparePublishArtifacts()
{
    if(!CurrentPublish)return;
    auto& Run=*CurrentPublish;
    for(const FString& Platform:{FString(TEXT("Windows")),FString(TEXT("Linux"))})
    {
        const bool bSelected=Platform==TEXT("Windows")?Run.Options.bIncludeWindows:Run.Options.bIncludeLinux;
        if(!bSelected)continue;
        const FString* Path=Run.Package.PakPaths.Find(Platform);
        if(!Path || IFileManager::Get().FileSize(**Path)<=0){FailPublish(TEXT("A requested package is missing from the verified packaging results."),TEXT("Preparing upload files"));return;}
        Run.Artifacts.Emplace(Platform,*Path);
    }
    if(!Run.Options.bIncludeSource){UploadNextArtifact();return;}
    FString Error;
    if(!FConvaiAvatarSourceArchive::Initialize(Error)){FailPublish(Error,TEXT("Preparing source"));return;}
    State.JobDetail=TEXT("Archiving this avatar's source");State.JobProgress.Reset(); State.JobBytesCompleted.Reset(); State.JobBytesTotal.Reset();State.bCanCancel=true;Publish();
    const uint64 Op=Generation;
    const FString Zip=Run.Package.ArtifactDirectory/TEXT("source.zip");
    const auto Cancelled=BeginArchiveWorker();
    // A worker must not retain the run's game-thread JSON record or a Slate widget.
    ArchiveWorker=Async(EAsyncExecution::Thread,[Weak=AsWeak(),Op,Cancelled,Prepared=Run.Prepared,Package=Run.Package,Zip]()
    {
        FString ArchiveError;
        ConvaiAvatarSourceArchivePrivate::Create(Prepared,Package.ProjectPath,Zip,ArchiveError,Package.BaseContentManifestHash,*Cancelled);
        if(ArchiveError.IsEmpty())ConvaiAvatarPackagingPrivate::ValidatePackagedInputs(Prepared,Package.InputFingerprint,ArchiveError,&Cancelled.Get());
        AsyncTask(ENamedThreads::GameThread,[Weak,Op,Cancelled,Zip,ArchiveError]()
        {
            auto C=Weak.Pin();if(!C)return;
            if(C->ArchiveCancelled.Get()==&Cancelled.Get())C->ArchiveCancelled.Reset();
            if(C->bShuttingDown || C->Generation!=Op || !C->CurrentPublish)return;
            if(!ArchiveError.IsEmpty()){C->FailPublish(ArchiveError,TEXT("Preparing source"));return;}
            C->CurrentPublish->Artifacts.Emplace(TEXT("Raw"),Zip);
            C->UploadNextArtifact();
        });
    });
}

void FConvaiAvatarStudioController::UploadNextArtifact(bool bRuntimeContractChecked)
{
    if(!CurrentPublish)return;
    if (CurrentPublish->NextArtifact>=CurrentPublish->Artifacts.Num()) { CompletePublish(CurrentPublish->AssetId); return; }
    if (!bRuntimeContractChecked && !CurrentPublish->Prepared.NativeRuntimeContract.IsEmpty())
    {
        const uint64 Op = Generation;
        State.JobDetail=TEXT("Checking groom physics support");State.bCanCancel=true;Publish();
        RuntimeContractCheck = MakeShared<FConvaiAvatarRemoteConfiguration>();
        RuntimeContractCheck->Fetch([Weak=AsWeak(),Op](FConvaiAvatarRemoteConfigurationData Data,FString Error)
        {
            auto Self=Weak.Pin();
            if (!Self || Self->bShuttingDown || Self->Generation!=Op || !Self->CurrentPublish) return;
            const bool bProduction = Self->Client && (Self->Client->GetAssetsRoot()==TEXT("https://beta.convai.com/assets") || Self->Client->GetAssetsRoot()==TEXT("https://beta.convai.com/assets/"));
            if (Error.IsEmpty()) ConvaiAvatarNativeRuntimeContent::CheckTarget(Self->CurrentPublish->Prepared.NativeRuntimeContract,
                bProduction ? Data.NativeRuntimeContract : FString(), Self->CurrentPublish->Options.bIncludeLinux, Error);
            if (!Error.IsEmpty()) { Self->FailPublish(Error,TEXT("Checking groom physics support")); return; }
            Self->UploadNextArtifact(true);
        });
        return;
    }
    auto& Run=*CurrentPublish;
    if(Run.NextArtifact>=Run.Artifacts.Num()){CompletePublish(Run.AssetId);return;}
    State.JobProgress.Reset(); State.JobBytesCompleted.Reset(); State.JobBytesTotal.Reset();State.bCanCancel=false;
    State.JobDetail=TEXT("Preparing the ")+ArtifactLabel(Run.Artifacts[Run.NextArtifact].Key)+TEXT(" upload");Publish();
    if(Run.AssetId.IsEmpty())
    {
        FConvaiAvatarAsset NewAsset;NewAsset.Gender=Run.Gender;
        WriteArtifact(MoveTemp(NewAsset));
        return;
    }
    const uint64 Op=Generation;
    Client->GetAsset(Run.AssetId,[Weak=AsWeak(),Op](FConvaiAvatarAsset Current,FString Error)
    {
        const auto C=Weak.Pin();if(!C || C->bShuttingDown || C->Generation!=Op || !C->CurrentPublish)return;
        if(!Error.IsEmpty()){C->FailPublish(Error,TEXT("Refreshing the avatar"));return;}
        C->WriteArtifact(MoveTemp(Current));
    });
}

void FConvaiAvatarStudioController::WriteArtifact(FConvaiAvatarAsset Current)
{
    if(!CurrentPublish)return;
    auto& Run=*CurrentPublish;
    if(Current.AssetId!=Run.AssetId){FailPublish(TEXT("The service returned a different avatar record."),TEXT("Preparing the upload"));return;}
    const FString Platform=Run.Artifacts[Run.NextArtifact].Key;
    const FString Path=Run.Artifacts[Run.NextArtifact].Value;
    FConvaiAvatarAssetWrite Write;
    Write.AssetId=Run.AssetId;Write.Name=Current.AssetId.IsEmpty()?Run.Prepared.DisplayName:Current.Name;
    Write.Gender=Current.AssetId.IsEmpty()?Run.Gender:Current.Gender;
    Write.Version=FConvaiAvatarAssetsClient::MakeVersion(Platform);
    Write.ThumbnailPath=Run.AssetId.IsEmpty()?Run.Thumbnail:FString();
    Write.Tags=FConvaiAvatarAssetsClient::MakeArtifactTags(Current.Tags,Platform);
    Write.Metadata=MetadataFor(Run.Prepared,Current);
    Write.bArtifactReservationOnly=!Run.AssetId.IsEmpty();
    if(Platform!=TEXT("Raw"))Write.Metadata->SetNumberField(Platform+TEXT("_PakSize"),static_cast<double>(IFileManager::Get().FileSize(*Path)));
    const uint64 Op=Generation;
    Client->CreateOrUpdate(Write,[Weak=AsWeak(),Op,Platform,Path](FConvaiAvatarAssetWriteResult Result,FString Error)
    {
        auto Self=Weak.Pin();if(!Self || Self->bShuttingDown || Self->Generation!=Op || !Self->CurrentPublish)return;
        auto& Active=*Self->CurrentPublish;
        if(!Result.Asset.AssetId.IsEmpty())
        {
            if(!Active.AssetId.IsEmpty() && Active.AssetId!=Result.Asset.AssetId){Self->FailPublish(TEXT("The upload response identified a different avatar."),TEXT("Saving the avatar record"));return;}
            Active.AssetId=Result.Asset.AssetId;
            Self->State.SelectedAssetId=Active.AssetId;
            if(!Active.Thumbnail.IsEmpty())Self->LocalThumbnails.Add(Active.AssetId,Active.Thumbnail);
            if(!Self->Assets.ContainsByPredicate([&Active](const FConvaiAvatarAsset& A){return A.AssetId==Active.AssetId;}))
            {
                // Creation omits visibility and thumbnail_url. The request explicitly created private metadata;
                // keep the chosen local thumbnail until a later Get/List returns its signed preview URL.
                Result.Asset.Visibility=TEXT("private");
                Result.Asset.Gender=Active.Gender;
                Self->Assets.Add(Result.Asset);
            }
            if(auto View=Self->Widget.Pin())View->ShowLibrary();
            // Fail drops the publish run, and Active with it.
            if(!Self->BindCreatedAvatar(Active.Prepared,Active.AssetId))return;
        }
        if(!Error.IsEmpty()){Self->FailPublish(Error,TEXT("Saving the avatar record"));return;}
        if(Active.AssetId.IsEmpty()){Self->Fail(TEXT("The server returned no asset ID. Refresh the library before retrying."));return;}
        Self->LibraryState.BeginArtifactUpload(Active.AssetId, FConvaiAvatarAssetsClient::MakeVersion(Platform));
        Self->State.bCanCancel=true;Self->State.JobDetail=TEXT("Uploading ")+ArtifactLabel(Platform);
        Self->State.JobBytesCompleted=0;
        const int64 UploadBytes=IFileManager::Get().FileSize(*Path);
        if(UploadBytes>0)Self->State.JobBytesTotal=UploadBytes;
        Self->Publish();
        Self->Client->UploadFile(Result.UploadUrl,Path,[Weak,Op,Platform,Path](FString UploadError)
        {
            const auto C=Weak.Pin();if(!C || C->bShuttingDown || C->Generation!=Op || !C->CurrentPublish)return;
            if(!UploadError.IsEmpty()){C->FailPublish(UploadError,TEXT("Uploading ")+ArtifactLabel(Platform));return;}
            if(Platform==TEXT("Windows"))
            {
                C->State.bCanCancel=false;C->State.JobDetail=TEXT("Saving the uploaded Windows avatar setup");
                C->State.JobProgress.Reset();C->State.JobBytesCompleted.Reset();C->State.JobBytesTotal.Reset();C->Publish();
                const FString Id=C->CurrentPublish->AssetId;
                C->Client->CommitWindowsMetadata(Id,FConvaiAvatarAssetsClient::MakeVersion(Platform),IFileManager::Get().FileSize(*Path),
                    MetadataFor(C->CurrentPublish->Prepared,FConvaiAvatarAsset()),[Weak,Op,Id,Platform](FConvaiAvatarAssetWriteResult Committed,FString CommitError)
                {
                    const auto Owner=Weak.Pin();if(!Owner || Owner->bShuttingDown || Owner->Generation!=Op || !Owner->CurrentPublish)return;
                    if(!CommitError.IsEmpty()){Owner->FailPublish(CommitError,TEXT("The Windows file uploaded, but saving its avatar setup"));return;}
                    if(Committed.Asset.AssetId!=Id){Owner->FailPublish(TEXT("The setup response identified a different avatar."),TEXT("Saving Windows avatar setup"));return;}
                    if(auto* Existing=Owner->Assets.FindByPredicate([&](const FConvaiAvatarAsset& A){return A.AssetId==Id;}))Existing->Metadata=Committed.Asset.Metadata;
                    Owner->CompleteUploadedArtifact(Platform);
                });
            }
            else C->CompleteUploadedArtifact(Platform);
        },{},Active.Package.DependencyResolutionFile,[Weak,Op](const FConvaiAvatarTransferProgress& Progress)
        {
            if(const auto C=Weak.Pin();C && !C->bShuttingDown && C->Generation==Op && C->CurrentPublish)
            {
                C->State.JobBytesCompleted=Progress.CompletedBytes;
                if(Progress.TotalBytes.IsSet())C->State.JobBytesTotal=Progress.TotalBytes;
                C->State.JobProgress=Progress.Fraction();C->Publish();
            }
        });
    });
}

bool FConvaiAvatarStudioController::BindCreatedAvatar(FConvaiAvatarPreparedAsset& Prepared,const FString& CloudId)
{
    const FString DraftId=Prepared.AssetId;
    FString BindingError;
    if(DraftId!=CloudId && !FConvaiAvatarWorkspace::RebindPreparedAsset(Prepared,CloudId,BindingError))
    {
        const FConvaiAvatarUnboundBinding& Pair=UnboundBindings.Add(DraftId,FConvaiAvatarUnboundBinding{DraftId,CloudId,Prepared.PluginName,AuthFingerprint});LocalDrafts.Remove(DraftId);
        FString Friendly=TEXT("The cloud avatar was created, but its local record could not be saved. Open the latest log, resolve the file error, then refresh. Creating another avatar would duplicate it.");
        FString PairError;
        // A refresh retries every pair; only a saved one outlives an editor restart.
        if(!ConvaiAvatarWorkspacePrivate::SaveUnboundBinding(Pair,PairError))
        {BindingError+=TEXT(" ")+PairError;Friendly+=TEXT(" Its link to this draft could not be saved either; a refresh tries again, but restarting the editor first loses it.");}
        Fail(BindingError,Friendly);return false;
    }
    LocalAssetIds.Add(CloudId);LocalRecords.Add(CloudId,Prepared);
    LocalDrafts.Remove(DraftId);IncompleteLocalIds.Remove(DraftId);UnboundBindings.Remove(DraftId);
    return true;
}

void FConvaiAvatarStudioController::CompleteUploadedArtifact(const FString& Platform)
{
    if(!CurrentPublish)return;
    LibraryState.CompleteArtifactUpload(CurrentPublish->AssetId,FConvaiAvatarAssetsClient::MakeVersion(Platform));
    CurrentPublish->Completed.Add(ArtifactLabel(Platform));
    ++CurrentPublish->NextArtifact;
    UploadNextArtifact();
}

void FConvaiAvatarStudioController::FailPublish(const FString& Error,const FString& Step)
{
    const FString PublishedId=CurrentPublish?CurrentPublish->AssetId:FString();
    FString Friendly=Step+TEXT(" could not finish. Open the latest log for details.");
    if(CurrentPublish)
    {
        if(!CurrentPublish->Completed.IsEmpty())Friendly+=TEXT(" Uploaded successfully: ")+FString::Join(CurrentPublish->Completed,TEXT(", "))+TEXT(".");
        if(!CurrentPublish->AssetId.IsEmpty())Friendly+=TEXT(" The avatar record is saved. Use Upload changes and select only the files still needed to retry.");
        else if(Step==TEXT("Saving the avatar record"))Friendly+=TEXT(" The create request may already have reached the server. Refresh the library before creating another avatar.");
    }
    Fail(Error,Friendly);
    RefreshPublishedAsset(PublishedId);
}
void FConvaiAvatarStudioController::CompletePublish(const FString& Id)
{
    const FString CompletedId=Id;
    const FString GeneratedNotice=CurrentPublish?DioramaNotice(CurrentPublish->Prepared):FString();
    CurrentPublish.Reset();
    FString Notice=TEXT("Avatar uploaded successfully.");State.SelectedAssetId=CompletedId;
    if(!GeneratedNotice.IsEmpty())Notice+=TEXT(" ")+GeneratedNotice;
    if (auto View=Widget.Pin()) View->ShowLibrary();
    EndJob(Notice,FString());
    Refresh();
}

void FConvaiAvatarStudioController::Download(const FString& InId)
{
    const FString Id=InId;
    if (State.bBusy) return;
    if (GEditor && GEditor->PlayWorld) { Fail(TEXT("Stop Play mode before adding or updating an avatar.")); return; }
    FConvaiAvatarPendingDownload Pending; FString PendingError;
    if (FConvaiAvatarDownloadService::FindPending(Id, Pending, PendingError))
    {
        const uint64 Op=BeginJob(TEXT("Get latest avatar"),TEXT("Applying the downloaded source"),false);
        FString Notice, Error;
        const auto Result = FConvaiAvatarDownloadService::TryApply(Pending, true, Notice, Error, true, true);
        if(Generation!=Op || bShuttingDown)return;
        if (Result == EConvaiAvatarDownloadApplyResult::Failed || !Error.IsEmpty()) { Fail(Error); return; }
        EndJob(Notice, FString());
        if (Result == EConvaiAvatarDownloadApplyResult::Applied) Refresh();
        return;
    }
    if(!PendingError.IsEmpty()){Fail(PendingError);return;}
    if (!EnsureClient()) return;
    FConvaiAvatarPreparedAsset Local; FString LocalError;
    const bool bReplace = FConvaiAvatarWorkspace::FindPreparedAsset(Id, Local, LocalError);
    const uint64 Op = BeginJob(bReplace ? TEXT("Get latest avatar") : TEXT("Download avatar"), TEXT("Checking source availability"), true);
    Client->GetAsset(Id, [Weak=AsWeak(), Op, bReplace, ExpectedPlugin=Local.PluginName](FConvaiAvatarAsset Asset, FString Error)
    {
        auto C=Weak.Pin(); if(!C || C->bShuttingDown || C->Generation!=Op) return;
        if(!Error.IsEmpty()) { C->Fail(Error); return; }
        const FString Version=Asset.FindSourceVersion();
        const auto* Source=Asset.VersionInfo.Find(Version);
        if(!Source || Source->Url.IsEmpty()) { C->Fail(TEXT("This avatar has no uploaded source. Upload source first to make it available in a project.")); return; }
        if(Version!=FConvaiAvatarAssetsClient::MakeVersion(TEXT("Raw")) && Version!=TEXT("raw")) { C->Fail(TEXT("This avatar's source uses a different Unreal Engine version. Upload source for this engine before downloading.")); return; }
        FConvaiAvatarDownloadSource Snapshot;
        if(!FConvaiAvatarDownloadService::SnapshotSource(Asset,Snapshot,Error) || !FConvaiAvatarSourceArchive::Initialize(Error)) { C->Fail(Error); return; }
        if(bReplace && Snapshot.PluginName!=ExpectedPlugin) { C->Fail(TEXT("The uploaded source uses a different plugin mount name. Import it into a separate project to avoid breaking references to the local avatar.")); return; }
        if(bReplace)
        {
            C->State.bCanCancel=false;C->Publish();
            EAppReturnType::Type Choice=EAppReturnType::No;
            {
#if WITH_DEV_AUTOMATION_TESTS
                // Automation normally returns No without displaying this dialog. The
                // opt-in rendered journey must exercise the actual dialog and its Yes
                // button; it never supplies an approval result here.
                const bool bRenderedJourney = Asset.Name.StartsWith(TEXT("Cloud Avatars QA ")) &&
                    FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudJourney")) &&
                    FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen"));
                const TGuardValue<bool> DialogAutomationGuard(GIsAutomationTesting, bRenderedJourney ? false : GIsAutomationTesting);
#endif
                Choice=FMessageDialog::Open(EAppMsgType::YesNo,FText::FromString(
                    TEXT("Replace local changes to ")+Asset.Name+TEXT(" with its latest uploaded source?\n\nThe existing files will be kept in a backup. Asset editors for this avatar may close; if its packages cannot unload, the update will finish after restarting Unreal.")));
            }
            if(C->Generation!=Op || C->bShuttingDown)return;
            if(Choice!=EAppReturnType::Yes){C->EndJob(FString(),FString());return;}
            C->State.bCanCancel=true;
        }
        const FString Downloads=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("ConvaiAvatarStudio/Downloads"));
        ConvaiAvatarStudioControllerPrivate::DeleteStaleDownloadZips(Downloads);
        const FString Zip=Downloads/(FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".zip"));
        C->State.JobDetail=TEXT("Downloading avatar source"); C->Publish();
        C->Client->DownloadFile(Source->Url,Zip,[Weak,Op,Snapshot,Zip,bReplace](FString DownloadError)
        {
            // A transfer can fail its host-side check after the worker already moved the file here.
            auto S=Weak.Pin(); if(!S || S->bShuttingDown || S->Generation!=Op) { IFileManager::Get().Delete(*Zip,false,false,true); return; }
            if(!DownloadError.IsEmpty()) { IFileManager::Get().Delete(*Zip,false,false,true); S->Fail(DownloadError); return; }
            S->State.bCanCancel=true; S->State.JobDetail=TEXT("Validating the source and checking required plugins"); S->State.JobProgress.Reset(); S->State.JobBytesCompleted.Reset(); S->State.JobBytesTotal.Reset(); S->Publish();
            const auto Cancelled=S->BeginArchiveWorker();
            S->ArchiveWorker=Async(EAsyncExecution::Thread,[Weak,Op,Cancelled,Snapshot,Zip,bReplace]()
            {
                FConvaiAvatarPendingDownload Job; FString Error;
                ConvaiAvatarStudioControllerPrivate::StageDownloadedZip(Zip,Snapshot,bReplace,Job,Error,*Cancelled);
                AsyncTask(ENamedThreads::GameThread,[Weak,Op,Cancelled,Job=MoveTemp(Job),Error=MoveTemp(Error)]() mutable
                {
                    auto Owner=Weak.Pin(); if(!Owner) return;
                    if(Owner->ArchiveCancelled.Get()==&Cancelled.Get())Owner->ArchiveCancelled.Reset();
                    Owner->CompleteStaging(Op,Job,Error);
                });
            });
        },[Weak,Op](float P){ if(auto S=Weak.Pin();S && !S->bShuttingDown && S->Generation==Op){ S->State.JobProgress=P; S->Publish(); }});
    });
}

void FConvaiAvatarStudioController::CompleteStaging(uint64 Op,FConvaiAvatarPendingDownload& Job,const FString& Error)
{
    if(bShuttingDown || Generation!=Op)
    {
        // A Cancel can land after Stage saved job.json and before this ran. Without its job.json the folder is inert, so the
        // extracted avatar, gigabytes for a MetaHuman, goes off the game thread.
        if(!Error.IsEmpty())return;
        FString LinkError;
        if(ConvaiAvatarDownloadServicePrivate::NoLinksBelow(Job.JobDirectory,FConvaiAvatarDownloadService::PendingRoot(),LinkError))
            IFileManager::Get().Delete(*(Job.JobDirectory/TEXT("job.json")),false,false,true);
        Async(EAsyncExecution::ThreadPool,[JobDirectory=Job.JobDirectory]
            {ConvaiAvatarPackagingPrivate::WarnScratchKept(ConvaiAvatarPackagingPrivate::DeleteOwnedTree(JobDirectory,FConvaiAvatarDownloadService::PendingRoot()));});
        return;
    }
    if(!Error.IsEmpty()) { Fail(Error); return; }
    FString Notice,ApplyError;
    const auto Result=FConvaiAvatarDownloadService::TryApply(Job,true,Notice,ApplyError,true,true);
    if(Result==EConvaiAvatarDownloadApplyResult::Failed || !ApplyError.IsEmpty()) { Fail(ApplyError); return; }
    EndJob(Notice,FString());
    Refresh();
}

bool FConvaiAvatarStudioController::ResumePendingDownloads()
{
    if (State.bBusy || bShuttingDown) return false;
    // Best effort: recovery problems become notices and Refresh still lists the library.
    // Download and Get latest keep the hard stop through FindPending.
    TArray<FConvaiAvatarPendingDownload> Jobs; FString Error;
    FConvaiAvatarDownloadService::GetPending(Jobs, Error);
    TArray<FString> Notices, Failures;
    if (!Error.IsEmpty()) Failures.Add(Error); // Each line already names its job folder.
    for (auto& Job : Jobs)
    {
        if (ConvaiAvatarDownloadServicePrivate::IsDeadInstalledJob(Job))
        {
            Failures.Add(Job.AvatarName+TEXT(": an earlier download cannot finish because its avatar folder is missing or changed. Download starts over; keep its job folder ")+Job.JobDirectory);
            continue;
        }
        if(Job.Phase!=TEXT("installed"))
        {
            Notices.Add(TEXT("A source update for ")+Job.AvatarName+TEXT(" is pending. Select the avatar and choose Get latest to continue, or restart Unreal."));
            continue;
        }
        FString Notice;
        const auto Result = FConvaiAvatarDownloadService::TryApply(Job, false, Notice, Error);
        if (!Notice.IsEmpty()) Notices.Add(Notice);
        if (Result == EConvaiAvatarDownloadApplyResult::Failed || !Error.IsEmpty()) Failures.Add(Job.AvatarName+TEXT(" (")+Job.JobDirectory+TEXT("): ")+Error);
    }
    if (!Failures.IsEmpty())
    {
        const FString Detail = FString::Join(Failures, TEXT("\n"));
        // Every Refresh retries recovery, so log a failure once, not once per Refresh.
        if (!Detail.Equals(LastRecoveryDiagnostic, ESearchCase::CaseSensitive)) { WriteDiagnosticLog(Detail); LastRecoveryDiagnostic = Detail; }
        Notices.Append(Failures);
    }
    if (!Notices.IsEmpty()) { State.Notice = FString::Join(Notices, TEXT("\n")); Publish(); }
    return false;
}
void FConvaiAvatarStudioController::Delete(const FString& InId)
{
    const FString Id=InId;
    // The presentation layer confirms the named avatar before invoking this action.
    if(State.bBusy || !EnsureClient())return;
    if(!Assets.ContainsByPredicate([&Id](const auto& Asset){return Asset.AssetId==Id && Asset.IsPrivate();}))
    {Fail(TEXT("Refresh your library before deleting this avatar."));return;}
    const uint64 Op=BeginJob(TEXT("Delete avatar"),TEXT("Removing the avatar from your cloud library"),false);
    Client->DeleteAsset(Id,[Weak=AsWeak(),Op,Id](FString Error)
    {
        auto C=Weak.Pin();if(!C || C->Generation!=Op)return;
        if(!Error.IsEmpty()){C->Fail(Error);return;}
        C->LibraryState.RecordDeleteResult(Id,Error);
        C->LibraryState.RemoveDeleted(C->Assets);C->LocalThumbnails.Remove(Id);
        C->EndJob(TEXT("Avatar deleted from the cloud library. Local source files are still available."),FString());
        C->Refresh();
    });
}
TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> FConvaiAvatarStudioController::BeginArchiveWorker()
{
    // Shutdown waits only for the newest worker, so an older one must be gone first. Its operation ended, so stop it.
    if(ArchiveCancelled)ArchiveCancelled->store(true);
    if(ArchiveWorker.IsValid())ArchiveWorker.Wait();
    const auto Cancelled=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    ArchiveCancelled=Cancelled;
    return Cancelled;
}

void FConvaiAvatarStudioController::Cancel()
{
    if(!State.bCanCancel)return;
    if(bDioramaScanPending)
    {
        UploadDefaults->Cancel();State.bUploadDefaultsLoading=false;
        AbandonJob(TEXT("Upload cancelled before avatar preparation."));return;
    }
    // AbandonJob stops the capture.
    if(PortraitCapture){AbandonJob(TEXT("Thumbnail capture cancelled. Your selected image is unchanged."));return;}
    if(bRemoteActionPending)
    {
        if(RemoteConfiguration)RemoteConfiguration->Cancel();
        State.bRemoteConfigurationLoading=false;UpdateEngineNotice();
        EndJob(TEXT("Upload cancelled before avatar preparation."),FString());return;
    }
    if(bLocalWorkActive)
    {
        bCancelRequested=true;State.bCanCancel=false;State.JobDetail=TEXT("Stopping the current operation...");
        if(Packaging)Packaging->Cancel();Publish();return;
    }
    if(ArchiveCancelled)
    {
        // Archiving and staging run before any upload starts, and staging installs nothing.
        ArchiveCancelled->store(true);ArchiveCancelled.Reset();
        const bool bPublishing=CurrentPublish.IsValid();
        CurrentPublish.Reset();
        AbandonJob(bPublishing?TEXT("Operation cancelled. Nothing was uploaded."):TEXT("Operation cancelled. Nothing was installed."));return;
    }
    FString Notice=TEXT("Operation cancelled.");
    if(CurrentPublish && !CurrentPublish->Completed.IsEmpty())
        Notice+=TEXT(" Uploaded successfully: ")+FString::Join(CurrentPublish->Completed,TEXT(", "))+TEXT(".");
    Notice+=TEXT(" The transfer in progress may already have reached the server. Refresh the library to check its outcome before retrying.");
    const FString PublishedId=CurrentPublish?CurrentPublish->AssetId:FString();
    CurrentPublish.Reset();
    // Before cancelling the requests, so any completion they raise is already stale.
    AbandonJob(Notice);
    if(Client)Client->CancelAll();if(Packaging)Packaging->Cancel();
    RefreshPublishedAsset(PublishedId);
}
void FConvaiAvatarStudioController::Shutdown()
{
    if(AuthenticationConfiguration && AuthenticationChangedHandle.IsValid())AuthenticationConfiguration->OnAuthenticationChanged().Remove(AuthenticationChangedHandle);
    AuthenticationConfiguration.Reset();AuthenticationChangedHandle.Reset();
    FTSTicker::GetCoreTicker().RemoveTicker(AuthenticationRefreshTicker);AuthenticationRefreshTicker.Reset();
    bShuttingDown=true;AbandonJob();if(RemoteConfiguration)RemoteConfiguration->Cancel();if(UploadDefaults)UploadDefaults->Cancel();if(Client)Client->CancelAll();if(Packaging)Packaging->Shutdown();
    // The worker then stops within one poll and its job's drain, instead of holding editor exit until PowerShell finishes.
    if(ArchiveCancelled)ArchiveCancelled->store(true);
    if(ArchiveWorker.IsValid())ArchiveWorker.Wait();Widget.Reset();
}

void FConvaiAvatarStudioController::UpdateEngineNotice()
{
    const FString Actual=FString::Printf(TEXT("%d.%d"),FEngineVersion::Current().GetMajor(),FEngineVersion::Current().GetMinor());
    State.EngineSummary=TEXT("Host: Unreal ")+Actual+TEXT("   |   Uploader: Unreal ")+Actual+TEXT(" (this installation)");
    if(!RemoteConfigurationData.CurrentEngineVersion.IsEmpty())State.EngineSummary+=TEXT("   |   Currently supported: Unreal ")+RemoteConfigurationData.CurrentEngineVersion;
    else State.EngineSummary+=State.bRemoteConfigurationLoading?TEXT("   |   Checking current support..."):TEXT("   |   Current support unavailable");
    State.bEngineMismatch=FConvaiAvatarRemoteConfiguration::IsEngineMismatch(Actual,RemoteConfigurationData.CurrentEngineVersion);
    State.bEngineMismatchAcknowledged=AcknowledgedEnginePair==Actual+TEXT("/")+RemoteConfigurationData.CurrentEngineVersion;
    State.EngineWarning.Reset();
    if(State.bEngineMismatch)
        State.EngineWarning=State.bEngineMismatchAcknowledged?TEXT("Continuing with this engine. Required plugin, toolchain, and source compatibility checks still apply."):
            TEXT("This editor differs from the currently supported engine. You can continue with this installation after reviewing the compatibility requirements.");
    else if(!State.bRemoteConfigurationLoading && RemoteConfigurationData.CurrentEngineVersion.IsEmpty())State.EngineWarning=TEXT("Current engine support could not be checked. Refresh before starting an upload.");
}

void FConvaiAvatarStudioController::ContinueEngineMismatch()
{
    if(State.bBusy || State.bRemoteConfigurationLoading || !State.bEngineMismatch)return;
    const FString Actual=FString::Printf(TEXT("%d.%d"),FEngineVersion::Current().GetMajor(),FEngineVersion::Current().GetMinor());
    AcknowledgedEnginePair=Actual+TEXT("/")+RemoteConfigurationData.CurrentEngineVersion;
    UpdateEngineNotice();Publish();
}

void FConvaiAvatarStudioController::RefreshRemoteConfiguration(bool bApplyAfterFetch,TFunction<void()> AfterReview)
{
    if(State.bRemoteConfigurationLoading)return;
    if(!RemoteConfiguration)RemoteConfiguration=MakeShared<FConvaiAvatarRemoteConfiguration>();
    const bool bForUpload=static_cast<bool>(AfterReview);
    bRemoteActionPending=bForUpload;
    State.bRemoteConfigurationLoading=true;State.RemoteConfigurationStatus=TEXT("Reading the current project profile and engine-version information...");
    UpdateEngineNotice();
    if(bForUpload){SetJob(TEXT("Prepare upload"),TEXT("Checking the current engine support and project configuration"));if(!bRemoteActionPending)return;}
    else Publish();
    RemoteConfiguration->Fetch([Weak=AsWeak(),bApplyAfterFetch,bForUpload,AfterReview=MoveTemp(AfterReview)](FConvaiAvatarRemoteConfigurationData Data,FString Error)
    {
        auto Self=Weak.Pin();if(!Self || Self->bShuttingDown)return;
        Self->State.bRemoteConfigurationLoading=false;Self->RemoteConfigurationData=MoveTemp(Data);
        Self->State.DependencySummary=Self->RemoteConfigurationData.DependencySummary;
        Self->State.RemoteConfigurationStatus=Error.IsEmpty()?Self->RemoteConfigurationData.SourceNotice:
            Error+TEXT(" The embedded rendering profile remains available for an explicitly reviewed host-project apply.");
        if(!Self->RemoteConfigurationData.MigrationTargetVersion.IsEmpty())Self->State.RemoteConfigurationStatus+=
            TEXT("\nPlanned engine upgrade: Unreal ")+Self->RemoteConfigurationData.MigrationTargetVersion+TEXT(". This does not change the engine used for your upload.");
        Self->UpdateEngineNotice();
        // A failure can end the review first, and another job can start after it.
        if(bForUpload && Self->bRemoteActionPending)Self->EndJob(FString(),FString());
        else Self->Publish();
        if(bApplyAfterFetch && !Self->State.bBusy)Self->ApplyReviewedConfiguration();
        else if(AfterReview && !Self->State.bBusy && !Self->RemoteConfigurationData.CurrentEngineVersion.IsEmpty() &&
            (!Self->State.bEngineMismatch || Self->State.bEngineMismatchAcknowledged))AfterReview();
    });
}

void FConvaiAvatarStudioController::ApplyConfiguration()
{
    if(State.bBusy || State.bRemoteConfigurationLoading)return;
    RefreshRemoteConfiguration(true);
}
void FConvaiAvatarStudioController::ApplyReviewedConfiguration()
{
    if(State.bBusy)return;
    FConvaiAvatarConfigurationPlan Plan;FString Error;
    const FString Notice=RemoteConfigurationData.ProfileJson.IsEmpty()?TEXT("Remote profile unavailable. Review the embedded fallback settings below before applying."):RemoteConfigurationData.SourceNotice;
    const FString Source=RemoteConfigurationData.ProfileJson.IsEmpty()?FString():RemoteConfigurationData.ProfileSourceUrl;
    if(!FConvaiAvatarProjectConfiguration::Analyze(FPaths::ProjectDir(),Plan,Error,RemoteConfigurationData.ProfileJson,Source,Notice)){Fail(Error);return;}
    if(Plan.bAlreadyApplied){State.ConfigurationStatus=TEXT("The Avatar Studio rendering and packaging profile is already applied.");Publish();return;}
    if(FMessageDialog::Open(EAppMsgType::YesNo,FText::FromString(Plan.Describe()+TEXT("\n\nApply these changes? The previous values will be backed up.")),NSLOCTEXT("ConvaiCloudAvatars","ApplyConfiguration","Apply Avatar Studio configuration"))!=EAppReturnType::Yes)return;
    if(!FConvaiAvatarProjectConfiguration::Apply(Plan,Error)){Fail(Error);return;}
    State.ConfigurationStatus=TEXT("Configuration applied. Restart Unreal for rendering changes to take effect. Restore previous configuration is available.");
    Publish();
}
void FConvaiAvatarStudioController::RestoreConfiguration()
{
    if(State.bBusy)return;
    FConvaiAvatarConfigurationPlan Plan;FString Error;
    if(!FConvaiAvatarProjectConfiguration::AnalyzeRestore(FPaths::ProjectDir(),Plan,Error)){Fail(Error);return;}
    if(FMessageDialog::Open(EAppMsgType::YesNo,FText::FromString(Plan.Describe()+TEXT("\n\nRestore these previous values?")),NSLOCTEXT("ConvaiCloudAvatars","RestoreConfiguration","Restore previous configuration"))!=EAppReturnType::Yes)return;
    if(!FConvaiAvatarProjectConfiguration::Restore(Plan,Error)){Fail(Error);return;}
    State.ConfigurationStatus=TEXT("Previous configuration restored. Restart Unreal for rendering changes to take effect.");Publish();
}
