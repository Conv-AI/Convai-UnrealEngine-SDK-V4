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

// ─────────────────────────────────────────────────────────────────────────────
// Command-line overrides
//
// Every tunable on this node can be overridden at launch via a command-line
// flag prefixed with "LipSyncAnim". A flag that is not set on the command line
// falls through to the UPROPERTY value authored on the anim graph node.
//
// Command line is immutable per-process, so all flags are parsed exactly once
// (first time any anim node calls GetLipSyncAnimCmdOverrides) and cached in a
// process-wide struct — subsequent Evaluate calls do nothing but read it.
//
// Example: -LipSyncAnimUpperFaceAlpha=0.5 -LipSyncAnimBypassSmoothing=1
//
// See Convai/Docs/LipSyncAnimCommandLineFlags.md for the full list.
// ─────────────────────────────────────────────────────────────────────────────
// Resolved via UConvaiUtils::GetLipSyncAnimParamOverride which goes through the full
// ResolveCustomParam chain: hardcoded default → CustomPrams settings → command line.
// Empty string means "not set" — the caller falls back to the UPROPERTY value.
namespace
{
	void ParseFloatFlag(const TCHAR* Flag, TOptional<float>& Out)
	{
		const FString Raw = UConvaiUtils::GetLipSyncAnimParamOverride(Flag);
		if (!Raw.IsEmpty())
		{
			Out = FCString::Atof(*Raw);
		}
	}

	void ParseBoolFlag(const TCHAR* Flag, TOptional<bool>& Out)
	{
		const FString Raw = UConvaiUtils::GetLipSyncAnimParamOverride(Flag);
		if (!Raw.IsEmpty())
		{
			Out = (Raw == TEXT("1")) || Raw.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		}
	}

	void ParseApplyModeFlag(const TCHAR* Flag, TOptional<EConvaiFaceSyncApplyMode>& Out)
	{
		const FString Raw = UConvaiUtils::GetLipSyncAnimParamOverride(Flag);
		if (Raw.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
		{
			Out = EConvaiFaceSyncApplyMode::Add;
		}
		else if (Raw.Equals(TEXT("Override"), ESearchCase::IgnoreCase))
		{
			Out = EConvaiFaceSyncApplyMode::Override;
		}
	}

	void ResolveLipSyncAnimOverrides(FLipSyncAnimOverrides& O)
	{
		O = FLipSyncAnimOverrides();
		ParseFloatFlag    (TEXT("LipSyncAnimUpperFaceAlpha"),           O.UpperFaceAlpha);
		ParseFloatFlag    (TEXT("LipSyncAnimLowerFaceAlpha"),           O.LowerFaceAlpha);
		ParseFloatFlag    (TEXT("LipSyncAnimStarvationBlendInDuration"),  O.StarvationBlendInDuration);
		ParseFloatFlag    (TEXT("LipSyncAnimStarvationBlendOutDuration"), O.StarvationBlendOutDuration);
		ParseFloatFlag    (TEXT("LipSyncAnimGlobalMultiplier"),         O.GlobalMultiplier);
		ParseFloatFlag    (TEXT("LipSyncAnimGlobalOffset"),             O.GlobalOffset);
		ParseFloatFlag    (TEXT("LipSyncAnimLowerFaceSmoothingSpeed"),  O.LowerSmoothingSpeed);
		ParseFloatFlag    (TEXT("LipSyncAnimUpperFaceSmoothingSpeed"),  O.UpperSmoothingSpeed);
		ParseBoolFlag     (TEXT("LipSyncAnimEnableLowerFaceSmoothing"), O.bEnableLowerSmoothing);
		ParseBoolFlag     (TEXT("LipSyncAnimEnableUpperFaceSmoothing"), O.bEnableUpperSmoothing);
		ParseBoolFlag     (TEXT("LipSyncAnimBypassSmoothing"),          O.bBypassSmoothing);
		ParseBoolFlag     (TEXT("LipSyncAnimBypassStarvationBlend"),    O.bBypassStarvationBlend);
		ParseApplyModeFlag(TEXT("LipSyncAnimApplyMode"),                O.ApplyMode);
	}
}

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
	CurrentStarvationAlpha = 0.0f;
	ResolveLipSyncAnimOverrides(CachedOverrides);
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

	// ── Resolve effective values (UPROPERTY, with optional -LipSyncAnim* cmdline overrides) ──
	const FLipSyncAnimOverrides& Ovr = CachedOverrides;
	const float EffUpperFaceAlpha           = Ovr.UpperFaceAlpha         .Get(UpperFaceAlpha);
	const float EffLowerFaceAlpha           = Ovr.LowerFaceAlpha         .Get(LowerFaceAlpha);
	const float EffStarvationBlendInDuration  = Ovr.StarvationBlendInDuration .Get(StarvationBlendInDuration);
	const float EffStarvationBlendOutDuration = Ovr.StarvationBlendOutDuration.Get(StarvationBlendOutDuration);
	const float EffGlobalMultiplier         = Ovr.GlobalMultiplier       .Get(GlobalMultiplier);
	const float EffGlobalOffset             = Ovr.GlobalOffset           .Get(GlobalOffset);
	const float EffLowerSmoothingSpeed      = Ovr.LowerSmoothingSpeed    .Get(LowerFaceSmoothingSpeed);
	const float EffUpperSmoothingSpeed      = Ovr.UpperSmoothingSpeed    .Get(UpperFaceSmoothingSpeed);
	const bool  bEffEnableLowerSmoothing    = Ovr.bEnableLowerSmoothing  .Get(bEnableLowerFaceSmoothing);
	const bool  bEffEnableUpperSmoothing    = Ovr.bEnableUpperSmoothing  .Get(bEnableUpperFaceSmoothing);
	const bool  bBypassSmoothing            = Ovr.bBypassSmoothing       .Get(false);
	const bool  bBypassStarvationBlend      = Ovr.bBypassStarvationBlend .Get(false);
	const EConvaiFaceSyncApplyMode EffApplyMode = Ovr.ApplyMode          .Get(ApplyMode);

	// Starvation blend: fade out curves when the lipsync system has no frames to play,
	// fade back in when frames resume. Alpha moves at a constant rate determined by
	// EffStarvationBlendDuration (0 = instant). Bypassed entirely when the command-line
	// flag -LipSyncAnimBypassStarvationBlend=1 is set — curves are always applied at full strength.
	if (bBypassStarvationBlend)
	{
		CurrentStarvationAlpha = 1.0f;
		if (!Component)
		{
			return;
		}
	}
	else
	{
		const bool bStarved = !Component || !Component->HasPlayableFaceFrames();
		const float TargetStarvationAlpha = bStarved ? 0.0f : 1.0f;
		// Use the fade-out duration when losing lipsync, fade-in duration when regaining it
		const float EffStarvationBlendDuration = bStarved ? EffStarvationBlendOutDuration : EffStarvationBlendInDuration;
		if (EffStarvationBlendDuration > 0.0f && CachedDeltaTime > 0.0f)
		{
			const float Speed = 1.0f / EffStarvationBlendDuration;
			CurrentStarvationAlpha = FMath::FInterpConstantTo(CurrentStarvationAlpha, TargetStarvationAlpha, CachedDeltaTime, Speed);
		}
		else
		{
			CurrentStarvationAlpha = TargetStarvationAlpha;
		}

		// Fully faded out — skip all curve work (SourcePose is already evaluated above)
		if (CurrentStarvationAlpha <= KINDA_SMALL_NUMBER)
		{
			return;
		}

		// Alpha is still fading but we lost the component — nothing to read
		if (!Component)
		{
			return;
		}
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
		? UConvaiUtils::MapBlendshapes(CurrentBlendshapes, BlendshapeMapping, EffGlobalMultiplier, EffGlobalOffset)
		: CurrentBlendshapes;

#if ConvaiDebugMode
	const double MapMs = (FPlatformTime::Seconds() - MapStart) * 1000.0;
#endif

	const bool bAnySmoothing = !bBypassSmoothing && (bEffEnableLowerSmoothing || bEffEnableUpperSmoothing);

#if ConvaiDebugMode
	const double SmoothStart = FPlatformTime::Seconds();
#endif

	// Compute final values (alpha + smoothing) into a TMap
	TMap<FName, float> FinalValues;
	FinalValues.Reserve(MappedBlendshapes.Num());

	for (const auto& Pair : MappedBlendshapes)
	{
		const bool bIsUpperFace = UpperFaceNameSet.Contains(Pair.Key);
		const float Alpha = bIsUpperFace ? EffUpperFaceAlpha : EffLowerFaceAlpha;
		float FinalValue = Pair.Value * Alpha;

		if (bAnySmoothing)
		{
			const bool bSmoothThis = bIsUpperFace ? bEffEnableUpperSmoothing : bEffEnableLowerSmoothing;
			if (bSmoothThis && CachedDeltaTime > 0.0f)
			{
				const float Speed = bIsUpperFace ? EffUpperSmoothingSpeed : EffLowerSmoothingSpeed;
				const float SmoothFactor = 1.0f - FMath::Pow(1.0f - Speed, CachedDeltaTime * 60.0f);

				const float* PrevValue = SmoothedValues.Find(Pair.Key);
				const float Previous = PrevValue ? *PrevValue : FinalValue;
				FinalValue = Previous + SmoothFactor * (FinalValue - Previous);
			}
			SmoothedValues.FindOrAdd(Pair.Key) = FinalValue;
		}

		// Apply starvation blend alpha on top (after smoothing, so smoothing tracks
		// the natural intended value and this acts as a separate fade modulation)
		FinalValue *= CurrentStarvationAlpha;

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

	if (EffApplyMode == EConvaiFaceSyncApplyMode::Add)
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
			if (EffApplyMode == EConvaiFaceSyncApplyMode::Add)
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
			if (!Found)
			{
				if (AActor* ParentActor = Owner->GetAttachParentActor())
				{
					Found = ParentActor->FindComponentByClass<UConvaiChatbotComponent>();
				}
			}
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
