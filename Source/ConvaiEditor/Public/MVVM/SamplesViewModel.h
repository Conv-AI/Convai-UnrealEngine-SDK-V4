/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SamplesViewModel.h
 *
 * ViewModel for the samples browser.
 */

#pragma once

#include "MVVM/ViewModel.h"
#include "Templates/SharedPointer.h"

/**
 * Model for sample items displayed in the samples browser.
 */
struct FSampleItem
{
    FText Name;
    FText Description;
    FString ImagePath;
    TArray<FString> Tags;
    bool bIsFeatured = false;
};

/**
 * ViewModel for the samples browser.
 * Manages sample items and provides filtering capabilities.
 */
class FSamplesViewModel final : public FViewModelBase
{
public:
    FSamplesViewModel();

    /** Initialize the ViewModel with data */
    virtual void Initialize() override;

    /** Clean up resources */
    virtual void Shutdown() override;

    /** Return the type name for lookup in the registry */
    static FName StaticType() { return TEXT("FSamplesViewModel"); }

    /** Get all sample items */
    const TArray<TSharedPtr<FSampleItem>> &GetItems() const { return Items; }

    /** Get filtered sample items based on search text */
    TArray<TSharedPtr<FSampleItem>> GetFilteredItems(const FString &SearchText) const;

    /** Get only featured sample items */
    TArray<TSharedPtr<FSampleItem>> GetFeaturedItems() const;

private:
    /** Load sample data */
    void PopulateDummyData();

    /** Reset all data */
    void ClearData();

private:
    /** All available samples */
    TArray<TSharedPtr<FSampleItem>> Items;
};
