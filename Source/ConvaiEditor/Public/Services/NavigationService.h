/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * NavigationService.h
 *
 * Navigation service for page routing and history management.
 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/UnrealString.h"
#include "ConvaiEditor.h"
#include "Services/Routes.h"
#include "Services/IUIContainer.h"
#include "UI/Factories/IPageFactory.h"
#include "UI/Factories/PageFactoryManager.h"

// Forward declarations
class SWidget;
class SBasePage;
class IPageFactoryManager;
class FJsonObject;

namespace ConvaiEditor
{
    class FNavigationMiddlewareManager;
}

/**
 * Fired when navigation occurs.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnRouteChanged, ConvaiEditor::Route::E /*PreviousRoute*/, ConvaiEditor::Route::E /*NewRoute*/);

/**
 * Navigation history entry.
 */
struct FNavigationHistoryEntry
{
    /** The route that was navigated to */
    ConvaiEditor::Route::E Route;

    /** Optional state data for the route */
    TSharedPtr<FJsonObject> State;

    /** Constructor */
    FNavigationHistoryEntry(ConvaiEditor::Route::E InRoute = ConvaiEditor::Route::E::None, TSharedPtr<FJsonObject> InState = nullptr)
        : Route(InRoute), State(InState)
    {
    }
};

/**
 * Interface for page navigation.
 */
class CONVAIEDITOR_API INavigationService : public IConvaiService
{
public:
    virtual void SetUIContainer(TWeakPtr<IUIContainer> InContainer) = 0;
    virtual void Navigate(ConvaiEditor::Route::E Route, TSharedPtr<FJsonObject> State = nullptr) = 0;
    virtual bool NavigateBack() = 0;
    virtual bool NavigateForward() = 0;
    virtual bool CanNavigateBack() const = 0;
    virtual bool CanNavigateForward() const = 0;
    virtual ConvaiEditor::Route::E GetCurrentRoute() const = 0;
    virtual TSharedPtr<FJsonObject> GetCurrentState() const = 0;
    virtual const TArray<FNavigationHistoryEntry> &GetHistory() const = 0;
    virtual void ClearHistory() = 0;
    virtual void ResetWindowState() = 0;
    virtual FOnRouteChanged &OnRouteChanged() = 0;

    static FName StaticType() { return TEXT("INavigationService"); }
};

/**
 * Handles UI navigation with lazy page loading and history management.
 */
class CONVAIEDITOR_API FNavigationService : public INavigationService
{
public:
    FNavigationService();

    virtual void Startup() override;
    virtual void Shutdown() override;
    static FName StaticType() { return TEXT("INavigationService"); }

    virtual void SetUIContainer(TWeakPtr<IUIContainer> InContainer) override { UIContainer = InContainer; }

    virtual void Navigate(ConvaiEditor::Route::E Route, TSharedPtr<FJsonObject> State = nullptr) override;
    virtual bool NavigateBack() override;
    virtual bool NavigateForward() override;
    virtual bool CanNavigateBack() const override;
    virtual bool CanNavigateForward() const override;
    virtual ConvaiEditor::Route::E GetCurrentRoute() const override { return CurrentRoute; }
    virtual TSharedPtr<FJsonObject> GetCurrentState() const override;
    virtual const TArray<FNavigationHistoryEntry> &GetHistory() const override { return History; }
    virtual void ClearHistory() override;
    virtual void ResetWindowState() override;
    virtual FOnRouteChanged &OnRouteChanged() override { return RouteChangedEvent; }

    /** Sets maximum history size */
    void SetMaxHistorySize(int32 NewMaxSize);

    /** Returns maximum history size */
    int32 GetMaxHistorySize() const;

    /** Returns current history size */
    int32 GetCurrentHistorySize() const;

private:
    bool ShowPageForRoute(ConvaiEditor::Route::E Route);
    void InitializePage(ConvaiEditor::Route::E Route, TSharedPtr<IUIContainer> InContainer);
    void InitializeViewModel(SBasePage *BasePage, ConvaiEditor::Route::E Route);
    void AddToHistory(ConvaiEditor::Route::E Route, TSharedPtr<FJsonObject> State);
    void PruneHistoryIfNeeded();

    TWeakPtr<IUIContainer> UIContainer;
    TSharedPtr<IPageFactoryManager> PageFactoryManager;
    TSet<ConvaiEditor::Route::E> InitializedPages;
    TMap<ConvaiEditor::Route::E, int32> RouteToIndex;
    ConvaiEditor::Route::E CurrentRoute;
    TArray<FNavigationHistoryEntry> History;
    int32 HistoryIndex = -1;
    int32 MaxHistorySize;
    bool bIsShuttingDown = false;
    FOnRouteChanged RouteChangedEvent;
};
