/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiRetargetPoseRefresher.cpp
 */

#include "ConvaiRetargetPoseRefresher.h"
#include "ConvaiEditor.h"

// The MetaHuman content that depends on the project skeleton ships only in UE 5.6+
// packages (E:/Dependencies_MH_fix), and the APIs used here are exported from 5.6.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)

#include "Animation/AnimSequence.h"
#include "Containers/Ticker.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FDelegateHandle AssetLoadedHandle;
	FDelegateHandle PreSaveHandle;
	FTSTicker::FDelegateHandle TickHandle;
	TArray<TWeakObjectPtr<UAnimSequence>> Pending;

	bool IsConvaiRetargetedSequence(const UObject* Object)
	{
		const UAnimSequence* Sequence = Cast<UAnimSequence>(Object);
		if (!Sequence || !Sequence->GetSkeleton())
		{
			return false;
		}
		// The stored pose is only used when no named retarget source is selected.
		return Sequence->RetargetSource.IsNone()
			&& !Sequence->GetRetargetSourceAsset().IsNull()
			&& Sequence->GetPackage()->GetName().StartsWith(TEXT("/ConvAI/"));
	}

	/** Rebuilds the stored pose; returns true when it differed from the stale one. */
	bool RefreshPose(UAnimSequence* Sequence)
	{
		const TArray<FTransform> Before = Sequence->GetRetargetTransforms();
		Sequence->UpdateRetargetSourceAssetData();
		const TArray<FTransform>& After = Sequence->GetRetargetTransforms();
		if (Before.Num() != After.Num())
		{
			return true;
		}
		for (int32 Index = 0; Index < After.Num(); ++Index)
		{
			if (!Before[Index].Equals(After[Index], KINDA_SMALL_NUMBER))
			{
				return true;
			}
		}
		return false;
	}

	bool ProcessPending(float)
	{
		int32 Refreshed = 0;
		const TArray<TWeakObjectPtr<UAnimSequence>> Batch = MoveTemp(Pending);
		for (const TWeakObjectPtr<UAnimSequence>& Weak : Batch)
		{
			UAnimSequence* Sequence = Weak.Get();
			if (Sequence && RefreshPose(Sequence))
			{
				// Compression bakes the retarget pose in, so the cached data is stale too.
				Sequence->BeginCacheDerivedDataForCurrentPlatform();
				++Refreshed;
			}
		}
		if (Refreshed > 0)
		{
			UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: refreshed the retarget pose of %d Convai animation(s) for this project's MetaHuman skeleton"), Refreshed);
		}
		TickHandle.Reset();
		return false;
	}

	void Enqueue(UAnimSequence* Sequence)
	{
		Pending.AddUnique(Sequence);
		if (!TickHandle.IsValid())
		{
			TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&ProcessPending));
		}
	}

	void OnAssetLoaded(UObject* Object)
	{
		// The cook path is handled in PreSave, where the target platform is known.
		if (!IsRunningCommandlet() && IsConvaiRetargetedSequence(Object))
		{
			Enqueue(CastChecked<UAnimSequence>(Object));
		}
	}

	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
	{
		if (!SaveContext.IsCooking() || !IsConvaiRetargetedSequence(Object))
		{
			return;
		}
		UAnimSequence* Sequence = CastChecked<UAnimSequence>(Object);
		if (RefreshPose(Sequence))
		{
			// The platform data was compressed with the stale pose before this point;
			// rebuild it before the package is serialized.
			Sequence->CacheDerivedDataForPlatform(SaveContext.GetTargetPlatform());
			UE_LOG(LogConvaiEditor, Log, TEXT("ConvaiEditor: refreshed the retarget pose of %s for cooking"), *Sequence->GetPathName());
		}
	}
}

void FConvaiRetargetPoseRefresher::Register()
{
	AssetLoadedHandle = FCoreUObjectDelegates::OnAssetLoaded.AddStatic(&OnAssetLoaded);
	PreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&OnObjectPreSave);

	// Sequences loaded before this module started never reach OnAssetLoaded.
	if (!IsRunningCommandlet())
	{
		for (TObjectIterator<UAnimSequence> It; It; ++It)
		{
			if (IsConvaiRetargetedSequence(*It))
			{
				Enqueue(*It);
			}
		}
	}
}

void FConvaiRetargetPoseRefresher::Unregister()
{
	FCoreUObjectDelegates::OnAssetLoaded.Remove(AssetLoadedHandle);
	FCoreUObjectDelegates::OnObjectPreSave.Remove(PreSaveHandle);
	AssetLoadedHandle.Reset();
	PreSaveHandle.Reset();
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Pending.Reset();
}

#else

void FConvaiRetargetPoseRefresher::Register() {}
void FConvaiRetargetPoseRefresher::Unregister() {}

#endif
