// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerSetupModel.h"

FSceneAutoTaggerSetupModel::FSceneAutoTaggerSetupModel(const int32 InPageSize)
	: PageSize(FMath::Max(1, InPageSize))
	, VisibleLimit(PageSize)
{
}

bool FSceneAutoTaggerSetupModel::HasUsableCredentials(
	const FString& HeaderName,
	const FString& Credential)
{
	return !HeaderName.TrimStartAndEnd().IsEmpty()
		&& !Credential.TrimStartAndEnd().IsEmpty();
}

void FSceneAutoTaggerSetupModel::SetCharacters(TArray<FSceneAutoTaggerCharacterOption> InCharacters)
{
	TSet<FString> SeenIDs;
	Characters.Reset(InCharacters.Num());
	for (FSceneAutoTaggerCharacterOption& Character : InCharacters)
	{
		Character.ID = Character.ID.TrimStartAndEnd();
		if (!Character.IsValid() || SeenIDs.Contains(Character.ID))
		{
			continue;
		}
		SeenIDs.Add(Character.ID);
		Characters.Add(MoveTemp(Character));
	}

	if (!SelectedCharacterID.IsEmpty() && !Characters.ContainsByPredicate(
		[this](const FSceneAutoTaggerCharacterOption& Character) { return Character.ID == SelectedCharacterID; }))
	{
		SelectedCharacterID.Reset();
	}
	RebuildVisibleCharacters(true);
}

void FSceneAutoTaggerSetupModel::SetSearchText(const FString& InSearchText)
{
	SearchText = InSearchText.TrimStartAndEnd();
	RebuildVisibleCharacters(true);
}

void FSceneAutoTaggerSetupModel::LoadMore()
{
	VisibleLimit += PageSize;
	RebuildVisibleCharacters(false);
}

bool FSceneAutoTaggerSetupModel::SelectCharacter(const FString& CharacterID)
{
	const FString NormalizedID = CharacterID.TrimStartAndEnd();
	if (!Characters.ContainsByPredicate(
		[&NormalizedID](const FSceneAutoTaggerCharacterOption& Character) { return Character.ID == NormalizedID; }))
	{
		return false;
	}
	SelectedCharacterID = NormalizedID;
	return true;
}

void FSceneAutoTaggerSetupModel::ClearSelection()
{
	SelectedCharacterID.Reset();
}

const FSceneAutoTaggerCharacterOption* FSceneAutoTaggerSetupModel::GetSelectedCharacter() const
{
	return Characters.FindByPredicate(
		[this](const FSceneAutoTaggerCharacterOption& Character) { return Character.ID == SelectedCharacterID; });
}

void FSceneAutoTaggerSetupModel::RebuildVisibleCharacters(const bool bResetPage)
{
	if (bResetPage)
	{
		VisibleLimit = PageSize;
	}
	VisibleCharacters.Reset();
	FilteredCount = 0;
	for (const FSceneAutoTaggerCharacterOption& Character : Characters)
	{
		if (!MatchesSearch(Character))
		{
			continue;
		}
		++FilteredCount;
		if (VisibleCharacters.Num() < VisibleLimit)
		{
			VisibleCharacters.Add(Character);
		}
	}
}

bool FSceneAutoTaggerSetupModel::MatchesSearch(const FSceneAutoTaggerCharacterOption& Character) const
{
	return SearchText.IsEmpty()
		|| Character.Name.Contains(SearchText, ESearchCase::IgnoreCase)
		|| Character.Description.Contains(SearchText, ESearchCase::IgnoreCase)
		|| Character.ID.Contains(SearchText, ESearchCase::IgnoreCase);
}
