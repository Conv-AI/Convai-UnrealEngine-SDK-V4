/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SamplesViewModel.cpp
 *
 * Implementation of the samples view model.
 */

#include "MVVM/SamplesViewModel.h"
#include "Utility/ConvaiConstants.h"

#define LOCTEXT_NAMESPACE "SamplesViewModel"

FSamplesViewModel::FSamplesViewModel()
{
}

void FSamplesViewModel::Initialize()
{
    FViewModelBase::Initialize();

    if (Items.IsEmpty())
    {
        PopulateDummyData();
    }
}

void FSamplesViewModel::Shutdown()
{
    ClearData();
    FViewModelBase::Shutdown();
}

TArray<TSharedPtr<FSampleItem>> FSamplesViewModel::GetFilteredItems(const FString &SearchText) const
{
    if (SearchText.IsEmpty())
    {
        return Items;
    }

    TArray<TSharedPtr<FSampleItem>> FilteredItems;
    const FString LowerSearchText = SearchText.ToLower();

    for (const TSharedPtr<FSampleItem> &Item : Items)
    {
        const FString LowerName = Item->Name.ToString().ToLower();
        const FString LowerDesc = Item->Description.ToString().ToLower();

        if (LowerName.Contains(LowerSearchText) || LowerDesc.Contains(LowerSearchText))
        {
            FilteredItems.Add(Item);
            continue;
        }

        for (const FString &Tag : Item->Tags)
        {
            if (Tag.ToLower().Contains(LowerSearchText))
            {
                FilteredItems.Add(Item);
                break;
            }
        }
    }

    return FilteredItems;
}

TArray<TSharedPtr<FSampleItem>> FSamplesViewModel::GetFeaturedItems() const
{
    TArray<TSharedPtr<FSampleItem>> FeaturedItems;

    for (const TSharedPtr<FSampleItem> &Item : Items)
    {
        if (Item->bIsFeatured)
        {
            FeaturedItems.Add(Item);
        }
    }

    return FeaturedItems;
}

void FSamplesViewModel::ClearData()
{
    Items.Empty();
}

void FSamplesViewModel::PopulateDummyData()
{
    Items.Reset();

    TArray<FString> GameTags = {"Game"};
    TArray<FString> ClassroomTags = {"Education", "Interior"};
    TArray<FString> TrainTags = {"Transport", "Public"};
    TArray<FString> FireTags = {"Emergency", "Services"};
    TArray<FString> SpaceTags = {"Sci-Fi", "Exploration"};
    TArray<FString> ForestTags = {"Nature", "Outdoors"};

    {
        TSharedPtr<FSampleItem> Item = MakeShared<FSampleItem>();
        Item->Name = LOCTEXT("NeuralNexusGame", "Neural Nexus Game");
        Item->Description = LOCTEXT("NeuralNexusDesc", "A cyberpunk themed environment with neon lights.");
        Item->ImagePath = ConvaiEditor::Constants::Images::Samples::Sample1;
        Item->Tags = GameTags;
        Item->bIsFeatured = true;
        Items.Add(Item);
    }

    {
        TSharedPtr<FSampleItem> Item = MakeShared<FSampleItem>();
        Item->Name = LOCTEXT("ClassroomDemo", "Classroom Demo");
        Item->Description = LOCTEXT("ClassroomDesc", "An interactive classroom environment for educational simulations.");
        Item->ImagePath = ConvaiEditor::Constants::Images::Samples::Sample2;
        Item->Tags = ClassroomTags;
        Items.Add(Item);
    }

    {
        TSharedPtr<FSampleItem> Item = MakeShared<FSampleItem>();
        Item->Name = LOCTEXT("TrainStationDemo", "Train Station Demo");
        Item->Description = LOCTEXT("TrainStationDesc", "A detailed train station for transport simulations.");
        Item->ImagePath = ConvaiEditor::Constants::Images::Samples::Sample3;
        Item->Tags = TrainTags;
        Item->bIsFeatured = true;
        Items.Add(Item);
    }

    {
        TSharedPtr<FSampleItem> Item = MakeShared<FSampleItem>();
        Item->Name = LOCTEXT("FireStationDemo", "Fire Station Demo");
        Item->Description = LOCTEXT("FireStationDesc", "Emergency response environment with fire station and vehicles.");
        Item->ImagePath = ConvaiEditor::Constants::Images::Samples::Sample4;
        Item->Tags = FireTags;
        Items.Add(Item);
    }

    {
        TSharedPtr<FSampleItem> Item = MakeShared<FSampleItem>();
        Item->Name = LOCTEXT("SpaceDemo", "Space Demo");
        Item->Description = LOCTEXT("SpaceDesc", "Lunar surface environment with spacecraft and astronauts.");
        Item->ImagePath = ConvaiEditor::Constants::Images::Samples::Sample5;
        Item->Tags = SpaceTags;
        Item->bIsFeatured = true;
        Items.Add(Item);
    }

    {
        TSharedPtr<FSampleItem> Item = MakeShared<FSampleItem>();
        Item->Name = LOCTEXT("ForestDemo", "Forest Demo");
        Item->Description = LOCTEXT("ForestDesc", "Natural forest environment with wildlife and observation tower.");
        Item->ImagePath = ConvaiEditor::Constants::Images::Samples::Sample6;
        Item->Tags = ForestTags;
        Items.Add(Item);
    }

    BroadcastInvalidated();
}

#undef LOCTEXT_NAMESPACE
