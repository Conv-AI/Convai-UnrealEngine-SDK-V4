/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AnnouncementViewModel.h
 *
 * ViewModel for announcement display and management.
 */

#pragma once

#include "CoreMinimal.h"
#include "MVVM/ViewModel.h"
#include "MVVM/ObservableProperty.h"
#include "Models/ConvaiAnnouncementData.h"
#include "Templates/SharedPointer.h"
#include "Events/EventAggregator.h"

class IContentFeedService;

/** ViewModel for announcement display and management. */
class CONVAIEDITOR_API FAnnouncementViewModel : public FViewModelBase
{
public:
    explicit FAnnouncementViewModel(TSharedPtr<IContentFeedService> InService);

    virtual ~FAnnouncementViewModel() override;

    virtual void Initialize() override;
    virtual void Shutdown() override;

    static FName StaticType() { return TEXT("FAnnouncementViewModel"); }

    FObservableBool IsLoading;
    FObservableBool HasError;
    FObservableString ErrorMessage;
    FObservableInt AnnouncementCount;

    void RefreshAnnouncements();

    void LoadAnnouncements(bool bForceRefresh = false);

    const TArray<FConvaiAnnouncementItem> &GetAnnouncements() const { return Announcements; }

    TArray<FConvaiAnnouncementItem> GetAnnouncementsByType(EAnnouncementType Type) const;

    double GetCacheAge() const;

    bool HasAnnouncements() const { return Announcements.Num() > 0; }

private:
    void OnAnnouncementsLoaded(const TArray<FConvaiAnnouncementItem> &Items, bool bWasFromCache);
    void OnAnnouncementsLoadFailed(const FString &Error);
    void ClearAnnouncements();

    TSharedPtr<IContentFeedService> Service;
    TArray<FConvaiAnnouncementItem> Announcements;
    ConvaiEditor::FEventSubscription NetworkRestoredSubscription;
};
