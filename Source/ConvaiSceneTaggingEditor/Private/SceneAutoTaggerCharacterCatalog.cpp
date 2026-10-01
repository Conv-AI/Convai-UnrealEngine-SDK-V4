// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerCharacterCatalog.h"

#include "ConvaiChatBotProxy.h"
#include "Editor.h"

void USceneAutoTaggerCharacterCatalog::BeginDestroy()
{
	Cancel();
	Super::BeginDestroy();
}

void USceneAutoTaggerCharacterCatalog::Refresh(FRefreshCallback Callback)
{
	Cancel();
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		Callback(false, {}, TEXT("An editor world is required to load Convai characters."));
		return;
	}

	ActiveProxy = UConvaiChatBotGetCharsProxy::CreateCharacterGetCharsProxy(World);
	if (!ActiveProxy)
	{
		Callback(false, {}, TEXT("Could not create the Convai character request."));
		return;
	}
	ActiveCallback = MoveTemp(Callback);
	ActiveProxy->OnSuccess.AddDynamic(this, &USceneAutoTaggerCharacterCatalog::HandleSuccess);
	ActiveProxy->OnFailure.AddDynamic(this, &USceneAutoTaggerCharacterCatalog::HandleFailure);
	ActiveProxy->Activate();
}

void USceneAutoTaggerCharacterCatalog::Cancel()
{
	if (ActiveProxy)
	{
		ActiveProxy->OnSuccess.RemoveDynamic(this, &USceneAutoTaggerCharacterCatalog::HandleSuccess);
		ActiveProxy->OnFailure.RemoveDynamic(this, &USceneAutoTaggerCharacterCatalog::HandleFailure);
		ActiveProxy = nullptr;
	}
	ActiveCallback = nullptr;
}

void USceneAutoTaggerCharacterCatalog::HandleSuccess(
	const TArray<FString>&,
	const TArray<FConvaiAvatarInfo>& Characters)
{
	if (!ActiveProxy)
	{
		return;
	}
	TArray<FSceneAutoTaggerCharacterOption> Options = ConvertCharacters(Characters);
	FRefreshCallback Callback = MoveTemp(ActiveCallback);
	ActiveProxy = nullptr;
	Callback(true, MoveTemp(Options), FString());
}

void USceneAutoTaggerCharacterCatalog::HandleFailure(
	const TArray<FString>&,
	const TArray<FConvaiAvatarInfo>&)
{
	if (!ActiveProxy)
	{
		return;
	}
	FRefreshCallback Callback = MoveTemp(ActiveCallback);
	ActiveProxy = nullptr;
	Callback(false, {}, TEXT("Could not load characters through the Convai character service."));
}

TArray<FSceneAutoTaggerCharacterOption> USceneAutoTaggerCharacterCatalog::ConvertCharacters(
	const TArray<FConvaiAvatarInfo>& Characters)
{
	TArray<FSceneAutoTaggerCharacterOption> Options;
	TSet<FString> SeenIDs;
	for (const FConvaiAvatarInfo& Character : Characters)
	{
		FSceneAutoTaggerCharacterOption Option;
		Option.ID = Character.CharacterId.TrimStartAndEnd();
		if (Option.ID.IsEmpty() || SeenIDs.Contains(Option.ID))
		{
			continue;
		}
		SeenIDs.Add(Option.ID);
		Option.Name = Character.CharacterName.TrimStartAndEnd();
		Option.Description = Character.Description.TrimStartAndEnd();
		// The wide avatar and model placeholder are not thumbnails. Rendering them
		// inside a square character card produces distorted full-body portraits.
		// Keep the picker honest: use the service's dedicated square image only.
		Option.AvatarUrl = Character.ModelDetails.MetaHuman.AvatarImageSquare.TrimStartAndEnd();
		if (Option.Name.IsEmpty())
		{
			Option.Name = TEXT("Unnamed character");
		}
		Options.Add(MoveTemp(Option));
	}
	return Options;
}
