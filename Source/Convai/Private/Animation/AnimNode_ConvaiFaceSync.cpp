// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Animation/AnimNode_ConvaiFaceSync.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiUtils.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
#include "Animation/AnimCurveUtils.h"
#endif
#if ConvaiDebugMode
#include "Utility/Log/ConvaiLogger.h"
#endif

FAnimNode_ConvaiFaceSync::FAnimNode_ConvaiFaceSync()
{
	SmoothedValues.Reserve(256);
}

void FAnimNode_ConvaiFaceSync::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(Initialize_AnyThread)
	SourcePose.Initialize(Context);
	bUpperFaceSetDirty = true;
	SmoothedValues.Reset();
}

void FAnimNode_ConvaiFaceSync::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(CacheBones_AnyThread)
	SourcePose.CacheBones(Context);
}

void FAnimNode_ConvaiFaceSync::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(Update_AnyThread)
	SourcePose.Update(Context);

	CachedDeltaTime = Context.GetDeltaTime();

	// Rebuild upper face set if the array changed
	if (CachedUpperFaceNamesCount != UpperFaceBlendshapeNames.Num())
	{
		bUpperFaceSetDirty = true;
	}

	if (bUpperFaceSetDirty)
	{
		UpperFaceNameSet.Empty(UpperFaceBlendshapeNames.Num());
		for (const FName& Name : UpperFaceBlendshapeNames)
		{
			UpperFaceNameSet.Add(Name);
		}
		CachedUpperFaceNamesCount = UpperFaceBlendshapeNames.Num();
		bUpperFaceSetDirty = false;
	}
}

#if ConvaiDebugMode
DEFINE_LOG_CATEGORY_STATIC(LogConvaiFaceSyncNode, Log, All);
#endif

void FAnimNode_ConvaiFaceSync::Evaluate_AnyThread(FPoseContext& Output)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(Evaluate_AnyThread)

#if ConvaiDebugMode
	const double TotalStart = FPlatformTime::Seconds();
	const double SourcePoseStart = FPlatformTime::Seconds();
#endif

	SourcePose.Evaluate(Output);

#if ConvaiDebugMode
	const double SourcePoseMs = (FPlatformTime::Seconds() - SourcePoseStart) * 1000.0;
	const double ResolveStart = FPlatformTime::Seconds();
#endif

	UConvaiChatbotComponent* Component = ResolveComponent(Output);

#if ConvaiDebugMode
	const double ResolveMs = (FPlatformTime::Seconds() - ResolveStart) * 1000.0;
#endif

	if (!Component)
	{
		return;
	}

#if ConvaiDebugMode
	const double ReadStart = FPlatformTime::Seconds();
#endif

	const TMap<FName, float> CurrentBlendshapes = Component->ConvaiGetFaceBlendshapes();

#if ConvaiDebugMode
	const double ReadMs = (FPlatformTime::Seconds() - ReadStart) * 1000.0;
#endif

	if (CurrentBlendshapes.Num() == 0)
	{
		return;
	}

#if ConvaiDebugMode
	const double MapStart = FPlatformTime::Seconds();
#endif

	const TMap<FName, float> MappedBlendshapes = (BlendshapeMapping.Num() > 0)
		? UConvaiUtils::MapBlendshapes(CurrentBlendshapes, BlendshapeMapping, GlobalMultiplier, GlobalOffset)
		: CurrentBlendshapes;

#if ConvaiDebugMode
	const double MapMs = (FPlatformTime::Seconds() - MapStart) * 1000.0;
#endif

	const bool bAnySmoothing = bEnableLowerFaceSmoothing || bEnableUpperFaceSmoothing;

#if ConvaiDebugMode
	const double SmoothStart = FPlatformTime::Seconds();
#endif

	// Compute final values (alpha + smoothing) into a TMap
	TMap<FName, float> FinalValues;
	FinalValues.Reserve(MappedBlendshapes.Num());

	for (const auto& Pair : MappedBlendshapes)
	{
		const bool bIsUpperFace = UpperFaceNameSet.Contains(Pair.Key);
		const float Alpha = bIsUpperFace ? UpperFaceAlpha : LowerFaceAlpha;
		float FinalValue = Pair.Value * Alpha;

		if (bAnySmoothing)
		{
			const bool bSmoothThis = bIsUpperFace ? bEnableUpperFaceSmoothing : bEnableLowerFaceSmoothing;
			if (bSmoothThis && CachedDeltaTime > 0.0f)
			{
				const float Speed = bIsUpperFace ? UpperFaceSmoothingSpeed : LowerFaceSmoothingSpeed;
				const float SmoothFactor = 1.0f - FMath::Pow(1.0f - Speed, CachedDeltaTime * 60.0f);

				const float* PrevValue = SmoothedValues.Find(Pair.Key);
				const float Previous = PrevValue ? *PrevValue : FinalValue;
				FinalValue = Previous + SmoothFactor * (FinalValue - Previous);
			}
			SmoothedValues.FindOrAdd(Pair.Key) = FinalValue;
		}

		FinalValues.Add(Pair.Key, FinalValue);
	}

#if ConvaiDebugMode
	const double SmoothMs = (FPlatformTime::Seconds() - SmoothStart) * 1000.0;
	const double CurveStart = FPlatformTime::Seconds();
#endif

	// Bulk curve write
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
	FBlendedHeapCurve OurCurve;
	UE::Anim::FCurveUtils::BuildUnsorted(OurCurve, FinalValues);

	if (ApplyMode == EConvaiFaceSyncApplyMode::Add)
	{
		Output.Curve.Accumulate(OurCurve, 1.0f);
	}
	else
	{
		Output.Curve.Combine(OurCurve);
	}
#else
	USkeleton* Skeleton = Output.AnimInstanceProxy->GetSkeleton();
	const FSmartNameMapping* CurveMapping = Skeleton ? Skeleton->GetSmartNameContainer(USkeleton::AnimCurveMappingName) : nullptr;
	if (!CurveMapping)
	{
		return;
	}

	for (const auto& Pair : FinalValues)
	{
		SmartName::UID_Type CurveUID = CurveMapping->FindUID(Pair.Key);
		if (CurveUID != SmartName::MaxUID)
		{
			if (ApplyMode == EConvaiFaceSyncApplyMode::Add)
			{
				const float Existing = Output.Curve.Get(CurveUID);
				Output.Curve.Set(CurveUID, Existing + Pair.Value);
			}
			else
			{
				Output.Curve.Set(CurveUID, Pair.Value);
			}
		}
	}
#endif

#if ConvaiDebugMode
	const double CurveMs = (FPlatformTime::Seconds() - CurveStart) * 1000.0;
	const double TotalMs = (FPlatformTime::Seconds() - TotalStart) * 1000.0;

	static int32 FrameCounter = 0;
	if (++FrameCounter % 60 == 0)
	{
		CONVAI_LOG(LogConvaiFaceSyncNode, Log,
			TEXT("[ConvaiFaceSync] Total=%.3fms | SourcePose=%.3fms | Resolve=%.3fms | ReadBS=%.3fms | Map=%.3fms | Smoothing=%.3fms | CurveWrite=%.3fms | Entries=%d"),
			TotalMs, SourcePoseMs, ResolveMs, ReadMs, MapMs, SmoothMs, CurveMs, MappedBlendshapes.Num());
	}
#endif
}

UConvaiChatbotComponent* FAnimNode_ConvaiFaceSync::ResolveComponent(const FAnimationBaseContext& Context)
{
	// If the pin provided a valid component, use it and cache
	if (ConvaiChatbotComponent && IsValid(ConvaiChatbotComponent))
	{
		ResolvedComponent = ConvaiChatbotComponent;
		return ConvaiChatbotComponent;
	}

	// Return cached result if still valid
	if (ResolvedComponent.IsValid())
	{
		return ResolvedComponent.Get();
	}

	// Auto-discover from owning actor
	if (Context.AnimInstanceProxy)
	{
		if (AActor* Owner = Context.AnimInstanceProxy->GetSkelMeshComponent()->GetOwner())
		{
			UConvaiChatbotComponent* Found = Owner->FindComponentByClass<UConvaiChatbotComponent>();
			ResolvedComponent = Found;
			return Found;
		}
	}

	return nullptr;
}

void FAnimNode_ConvaiFaceSync::GatherDebugData(FNodeDebugData& DebugData)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(GatherDebugData)
	FString DebugLine = DebugData.GetNodeName(this);
	DebugData.AddDebugItem(DebugLine);
	SourcePose.GatherDebugData(DebugData);
}
