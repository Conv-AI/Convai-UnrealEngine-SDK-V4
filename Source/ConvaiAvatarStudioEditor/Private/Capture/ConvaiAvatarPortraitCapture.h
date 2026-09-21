// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/SoftObjectPath.h"

class UBlueprint;
class UWorld;

/** Game-thread service. Owns an isolated preview; never starts PIE or saves an Unreal asset. */
class FConvaiAvatarPortraitCapture : public TSharedFromThis<FConvaiAvatarPortraitCapture>
{
public:
	using FCompletion = TFunction<void(const FString& Path, const FString& Error)>;
	FConvaiAvatarPortraitCapture();
	~FConvaiAvatarPortraitCapture();
	static bool IsAvailable(FString& Reason);
	void Start(const FSoftObjectPath& Blueprint, bool bIsMetaHuman, FCompletion Completion);
	/** Synchronous teardown. A cancelled operation never invokes its completion callback. */
	void Cancel();
	bool IsBusy() const;
#if WITH_DEV_AUTOMATION_TESTS
	static FVector GetCameraOffsetForTests(const FVector& BoundsExtent);
	UWorld* GetPreviewWorldForTests() const;
	UBlueprint* GetPreviewBlueprintForTests() const;
#endif

private:
	struct FState;
	TUniquePtr<FState> State;
	FTSTicker::FDelegateHandle TickHandle;
	FCompletion OnComplete;
	bool Tick(float DeltaTime);
	void Finish(const FString& Path, const FString& Error);
};
