// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"
#include "SceneAutoTaggerCharacterCatalog.generated.h"

class UConvaiChatBotGetCharsProxy;

struct FSceneAutoTaggerCharacterOption
{
	FString ID;
	FString Name;
	FString Description;
	FString AvatarUrl;

	bool IsValid() const { return !ID.IsEmpty(); }
};

UCLASS()
class USceneAutoTaggerCharacterCatalog : public UObject
{
	GENERATED_BODY()

public:
	using FRefreshCallback = TFunction<void(bool bSuccess, TArray<FSceneAutoTaggerCharacterOption> Characters, FString Error)>;

	void Refresh(FRefreshCallback Callback);
	void Cancel();
	virtual void BeginDestroy() override;

	/** Converts the SDK proxy result into the compact picker model. */
	static TArray<FSceneAutoTaggerCharacterOption> ConvertCharacters(const TArray<FConvaiAvatarInfo>& Characters);

private:
	UFUNCTION()
	void HandleSuccess(const TArray<FString>& CharacterIDs, const TArray<FConvaiAvatarInfo>& Characters);

	UFUNCTION()
	void HandleFailure(const TArray<FString>& CharacterIDs, const TArray<FConvaiAvatarInfo>& Characters);

	UPROPERTY()
	TObjectPtr<UConvaiChatBotGetCharsProxy> ActiveProxy;
	FRefreshCallback ActiveCallback;
};
