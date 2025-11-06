/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ChangelogViewModel.h
 *
 * ViewModel for changelog display and management.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ViewModel.h"
#include "MVVM/ObservableProperty.h"
#include "Models/ConvaiAnnouncementData.h"
#include "Templates/SharedPointer.h"
#include "Events/EventAggregator.h"

// Forward declarations
class IContentFeedService;

/** ViewModel for changelog display and management. */
class CONVAIEDITOR_API FChangelogViewModel : public FViewModelBase
{
public:
    /** Constructor with service injection */
    explicit FChangelogViewModel(TSharedPtr<IContentFeedService> InService);

    /** Destructor - cleans up observable bindings */
    virtual ~FChangelogViewModel() override;

    // FViewModelBase interface
    virtual void Initialize() override;
    virtual void Shutdown() override;

    /** Return the type name for lookup in the registry */
    static FName StaticType() { return TEXT("FChangelogViewModel"); }

    /** Observable: true while loading changelogs */
    FObservableBool IsLoading;

    /** Observable: true if last operation had an error */
    FObservableBool HasError;

    /** Observable: Error message from last failed operation */
    FObservableString ErrorMessage;

    /** Observable: Number of loaded changelogs */
    FObservableInt ChangelogCount;

    /** Refresh changelogs from remote source */
    void RefreshChangelogs();

    /** Load changelogs (cache or remote) */
    void LoadChangelogs(bool bForceRefresh = false);

    /** Get loaded changelogs */
    const TArray<FConvaiChangelogItem> &GetChangelogs() const { return Changelogs; }

    /** Get cache age in seconds */
    double GetCacheAge() const;

    /** Check if changelogs are currently loaded */
    bool HasChangelogs() const { return Changelogs.Num() > 0; }

private:
    /** Handle successful changelog fetch */
    void OnChangelogsLoaded(const TArray<FConvaiChangelogItem> &Items, bool bWasFromCache);

    /** Handle failed changelog fetch */
    void OnChangelogsLoadFailed(const FString &Error);

    /** Clear current changelogs and reset state */
    void ClearChangelogs();

    /** Injected generic content feed service (configured for changelogs URL) */
    TSharedPtr<IContentFeedService> Service;

    /** Loaded changelogs (sorted by date) */
    TArray<FConvaiChangelogItem> Changelogs;

    /** Event subscription for network restoration (RAII-based automatic cleanup) */
    ConvaiEditor::FEventSubscription NetworkRestoredSubscription;
};
