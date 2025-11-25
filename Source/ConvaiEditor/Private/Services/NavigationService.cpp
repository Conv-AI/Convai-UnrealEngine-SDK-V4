/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NavigationService.cpp
 *
 * Implementation of navigation service for page routing.
 */

#include "Services/NavigationService.h"
#include "Logging/ConvaiEditorNavigationLog.h"
#include "Services/ConvaiDIContainer.h"
#include "UI/Factories/PageFactoryManager.h"
#include "UI/Pages/SBasePage.h"
#include "MVVM/ViewModel.h"
#include "Dom/JsonObject.h"
#include "Utility/ConvaiValidationUtils.h"
#include "Services/Navigation/NavigationMiddlewareManager.h"
#include "Services/Navigation/INavigationMiddleware.h"

FNavigationService::FNavigationService()
    : CurrentRoute(ConvaiEditor::Route::E::None), HistoryIndex(-1), MaxHistorySize(50)
{
}

void FNavigationService::Startup()
{
    TOptional<TSharedPtr<IPageFactoryManager>> PageFactoryManagerOpt = FConvaiValidationUtils::ResolveService<IPageFactoryManager>(TEXT("FNavigationService::Startup"));
    if (PageFactoryManagerOpt.IsSet())
    {
        PageFactoryManager = PageFactoryManagerOpt.GetValue();
    }
}

void FNavigationService::Navigate(ConvaiEditor::Route::E Route, TSharedPtr<FJsonObject> State)
{
    // Prevent navigation during shutdown
    if (bIsShuttingDown)
    {
        UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("Navigation blocked: service shutting down"));
        return;
    }

    if (Route == ConvaiEditor::Route::E::None)
    {
        UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("Navigation blocked: invalid route 'None'"));
        return;
    }
    if (Route == CurrentRoute)
    {
        return;
    }

    ConvaiEditor::Route::E PreviousRoute = CurrentRoute;

    ConvaiEditor::FNavigationContext Context(PreviousRoute, Route, State);

    TOptional<ConvaiEditor::Route::E> RedirectRoute;
    if (!ConvaiEditor::FNavigationMiddlewareManager::Get().ExecuteBeforeHooks(Context, RedirectRoute))
    {
        UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("Navigation blocked by middleware: %s -> %s"), *ConvaiEditor::Route::ToString(PreviousRoute), *ConvaiEditor::Route::ToString(Route));

        if (RedirectRoute.IsSet())
        {
            Navigate(RedirectRoute.GetValue(), State);
        }

        return;
    }

    if (ShowPageForRoute(Route))
    {
        CurrentRoute = Route;
        AddToHistory(Route, State);
        RouteChangedEvent.Broadcast(PreviousRoute, Route);

        ConvaiEditor::FNavigationMiddlewareManager::Get().ExecuteAfterHooks(Context);
    }
}

bool FNavigationService::NavigateBack()
{
    if (!CanNavigateBack())
    {
        return false;
    }

    HistoryIndex--;
    const FNavigationHistoryEntry &Entry = History[HistoryIndex];
    ConvaiEditor::Route::E PreviousRoute = CurrentRoute;

    if (ShowPageForRoute(Entry.Route))
    {
        CurrentRoute = Entry.Route;
        RouteChangedEvent.Broadcast(PreviousRoute, Entry.Route);
        return true;
    }

    // If showing the page failed, revert history index
    HistoryIndex++;
    return false;
}

bool FNavigationService::NavigateForward()
{
    if (!CanNavigateForward())
    {
        return false;
    }

    HistoryIndex++;
    const FNavigationHistoryEntry &Entry = History[HistoryIndex];
    ConvaiEditor::Route::E PreviousRoute = CurrentRoute;

    if (ShowPageForRoute(Entry.Route))
    {
        CurrentRoute = Entry.Route;
        RouteChangedEvent.Broadcast(PreviousRoute, Entry.Route);
        return true;
    }

    // If showing the page failed, revert history index
    HistoryIndex--;
    return false;
}

bool FNavigationService::CanNavigateBack() const
{
    return HistoryIndex > 0;
}

bool FNavigationService::CanNavigateForward() const
{
    return HistoryIndex >= 0 && HistoryIndex < History.Num() - 1;
}

TSharedPtr<FJsonObject> FNavigationService::GetCurrentState() const
{
    if (HistoryIndex >= 0 && HistoryIndex < History.Num())
    {
        return History[HistoryIndex].State;
    }

    return nullptr;
}

void FNavigationService::ClearHistory()
{
    History.Empty();
    HistoryIndex = -1;

    if (CurrentRoute != ConvaiEditor::Route::E::None)
    {
        AddToHistory(CurrentRoute, nullptr);
    }
}

void FNavigationService::ResetWindowState()
{
    RouteToIndex.Empty();
    InitializedPages.Empty();
    UIContainer.Reset();
    History.Empty();
    HistoryIndex = -1;
    CurrentRoute = ConvaiEditor::Route::E::None;
}

void FNavigationService::AddToHistory(ConvaiEditor::Route::E Route, TSharedPtr<FJsonObject> State)
{
    if (HistoryIndex >= 0 && HistoryIndex < History.Num() - 1)
    {
        History.RemoveAt(HistoryIndex + 1, History.Num() - HistoryIndex - 1);
    }

    History.Add(FNavigationHistoryEntry(Route, State));
    HistoryIndex = History.Num() - 1;

    PruneHistoryIfNeeded();
}

void FNavigationService::PruneHistoryIfNeeded()
{
    if (History.Num() <= MaxHistorySize)
    {
        return;
    }

    const int32 EntriesToRemove = History.Num() - MaxHistorySize;

    History.RemoveAt(0, EntriesToRemove);

    HistoryIndex = FMath::Max(0, HistoryIndex - EntriesToRemove);
}

void FNavigationService::SetMaxHistorySize(int32 NewMaxSize)
{
    if (NewMaxSize <= 0)
    {
        UE_LOG(LogConvaiEditorNavigation, Warning, TEXT("Navigation history size validation failed: %d (must be positive)"), NewMaxSize);
        return;
    }

    MaxHistorySize = NewMaxSize;

    PruneHistoryIfNeeded();
}

int32 FNavigationService::GetMaxHistorySize() const
{
    return MaxHistorySize;
}

int32 FNavigationService::GetCurrentHistorySize() const
{
    return History.Num();
}

bool FNavigationService::ShowPageForRoute(ConvaiEditor::Route::E Route)
{
    TSharedPtr<IUIContainer> PinnedContainer = UIContainer.Pin();
    if (!FConvaiValidationUtils::IsValidPtr(PinnedContainer, TEXT("UIContainer in FNavigationService::ShowPageForRoute")) || !PinnedContainer->IsValid())
    {
        return false;
    }

    if (!InitializedPages.Contains(Route))
    {
        InitializePage(Route, PinnedContainer);
    }

    const int32 *IndexPtr = RouteToIndex.Find(Route);
    if (ensureMsgf(IndexPtr, TEXT("Index for route %s should exist"), *ConvaiEditor::Route::ToString(Route)))
    {
        PinnedContainer->ShowPage(*IndexPtr);

        TSharedPtr<SWidget> PageWidget = PinnedContainer->GetPage(*IndexPtr);
        if (PageWidget.IsValid())
        {
            TSharedRef<SWidget> WidgetRef = PageWidget.ToSharedRef();
            if (SBasePage *BasePage = (SBasePage *)&WidgetRef.Get())
            {
                if (BasePage->IsA(SBasePage::StaticClass()))
                {
                    BasePage->OnPageActivated();
                }
            }
        }

        return true;
    }

    return false;
}

void FNavigationService::InitializePage(ConvaiEditor::Route::E Route, TSharedPtr<IUIContainer> InContainer)
{
    if (!FConvaiValidationUtils::IsValidPtr(PageFactoryManager, TEXT("PageFactoryManager in FNavigationService::InitializePage")))
    {
        return;
    }

    PageFactoryManager->CreatePage(Route)
        .LogOnFailure(LogConvaiEditorNavigation, *FString::Printf(TEXT("Page creation failed for route: %s"), *ConvaiEditor::Route::ToString(Route)))
        .Tap([this, Route, &InContainer](TSharedPtr<SWidget> Content)
             {
            if (!FConvaiValidationUtils::IsValidPtr(Content, FString::Printf(TEXT("Created page content for route %s"), *ConvaiEditor::Route::ToString(Route))))
            {
                return;
            }

            const int32 PageIndex = InContainer->AddPage(Content.ToSharedRef());

            RouteToIndex.Add(Route, PageIndex);
            InitializedPages.Add(Route);

            TSharedRef<SWidget> WidgetRef = Content.ToSharedRef();
            if (SBasePage *BasePage = (SBasePage *)&WidgetRef.Get())
            {
                if (BasePage->IsA(SBasePage::StaticClass()))
                {
                    InitializeViewModel(BasePage, Route);
                }
            } });
}

void FNavigationService::InitializeViewModel(SBasePage *BasePage, ConvaiEditor::Route::E Route)
{
    if (!FConvaiValidationUtils::IsNotNull(BasePage, TEXT("BasePage in FNavigationService::InitializeViewModel")))
    {
        return;
    }
    TSharedPtr<FViewModelBase> ViewModel = BasePage->GetViewModel();
    if (ViewModel.IsValid() && !ViewModel->IsInitialized())
    {
        ViewModel->Initialize();
    }
}

void FNavigationService::Shutdown()
{
    UE_LOG(LogConvaiEditorNavigation, Log, TEXT("NavigationService: Shutting down..."));

    // Set shutdown flag to prevent new navigation
    bIsShuttingDown = true;

    // Clear all navigation state
    PageFactoryManager.Reset();
    RouteToIndex.Empty();
    InitializedPages.Empty();
    UIContainer.Reset();
    History.Empty();
    HistoryIndex = -1;
    CurrentRoute = ConvaiEditor::Route::E::None;

    UE_LOG(LogConvaiEditorNavigation, Log, TEXT("NavigationService: Shutdown complete"));
}
