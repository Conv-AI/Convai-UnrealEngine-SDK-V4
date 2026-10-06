// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneAutoTaggerCharacterCatalog.h"

class FSceneAutoTaggerSetupModel
{
public:
	explicit FSceneAutoTaggerSetupModel(int32 InPageSize = 12);

	static bool HasUsableCredentials(const FString& HeaderName, const FString& Credential);

	void SetCharacters(TArray<FSceneAutoTaggerCharacterOption> InCharacters);
	void SetSearchText(const FString& InSearchText);
	void LoadMore();
	bool SelectCharacter(const FString& CharacterID);
	void ClearSelection();

	const TArray<FSceneAutoTaggerCharacterOption>& GetAllCharacters() const { return Characters; }
	const TArray<FSceneAutoTaggerCharacterOption>& GetVisibleCharacters() const { return VisibleCharacters; }
	const FSceneAutoTaggerCharacterOption* GetSelectedCharacter() const;
	const FString& GetSelectedCharacterID() const { return SelectedCharacterID; }
	const FString& GetSearchText() const { return SearchText; }
	int32 GetFilteredCount() const { return FilteredCount; }
	bool CanLoadMore() const { return VisibleCharacters.Num() < FilteredCount; }

private:
	void RebuildVisibleCharacters(bool bResetPage);
	bool MatchesSearch(const FSceneAutoTaggerCharacterOption& Character) const;

	TArray<FSceneAutoTaggerCharacterOption> Characters;
	TArray<FSceneAutoTaggerCharacterOption> VisibleCharacters;
	FString SelectedCharacterID;
	FString SearchText;
	int32 PageSize = 12;
	int32 VisibleLimit = 12;
	int32 FilteredCount = 0;
};
