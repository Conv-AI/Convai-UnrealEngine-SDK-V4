// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerViewSelection.h"

#include "SceneAutoTaggerNativeCoreAdapter.h"

#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/ThreadSafeCounter.h"
#include "Materials/MaterialInterface.h"
#if WITH_EDITOR
#include "ThumbnailRendering/SceneThumbnailInfo.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogConvaiSceneAutoTaggerViewFacade, Log, All);

namespace ConvaiSceneAutoTagger
{
namespace
{
constexpr int32 MaximumNativeFailureLogs = 8;

void LogNativeFailure(const TCHAR* Operation, const convai_sat_error_v1* Error = nullptr)
{
	static FThreadSafeCounter FailureCount;
	if (FailureCount.Increment() > MaximumNativeFailureLogs)
	{
		return;
	}

	FString Detail;
	if (Error && Error->message[0] != '\0')
	{
		Detail = UTF8_TO_TCHAR(Error->message);
	}
	else
	{
		Detail = FSceneAutoTaggerNativeCoreAdapter::Get().GetDiagnostic();
	}
	if (Detail.IsEmpty())
	{
		Detail = TEXT("native core is unavailable");
	}
	UE_LOG(
		LogConvaiSceneAutoTaggerViewFacade,
		Warning,
		TEXT("Scene Auto Tagger native %s failed: %s"),
		Operation,
		*Detail);
}

const convai_sat_api_v4* GetApi(const TCHAR* Operation)
{
	const FSceneAutoTaggerNativeCoreAdapter& Adapter =
		FSceneAutoTaggerNativeCoreAdapter::Get();
	const convai_sat_api_v4* Api = Adapter.IsAvailable() ? Adapter.GetApi() : nullptr;
	if (!Api)
	{
		LogNativeFailure(Operation);
	}
	return Api;
}

convai_sat_error_v1 MakeError()
{
	convai_sat_error_v1 Error = {};
	Error.struct_size = sizeof(Error);
	return Error;
}

convai_sat_vec3d ToNativeCoordinates(const FVector& UnrealVector)
{
	// Unreal is left-handed (+X forward, +Y right, +Z up). The native ABI is
	// right-handed with +Z up, so reflect Y at the boundary.
	return { UnrealVector.X, -UnrealVector.Y, UnrealVector.Z };
}

FVector FromNativeCoordinates(const convai_sat_vec3d& NativeVector)
{
	return FVector(NativeVector.x, -NativeVector.y, NativeVector.z);
}

convai_sat_capture_lighting_policy ToNativeLightingPolicy(
	const ESceneAutoTaggerCaptureLightingPolicy Policy)
{
	switch (Policy)
	{
	case ESceneAutoTaggerCaptureLightingPolicy::SuppressDirectSpecular:
		return CONVAI_SAT_CAPTURE_LIGHTING_SUPPRESS_DIRECT_SPECULAR;
	case ESceneAutoTaggerCaptureLightingPolicy::SuppressDirectSpecularWithMetalRecovery:
		return CONVAI_SAT_CAPTURE_LIGHTING_SUPPRESS_DIRECT_SPECULAR_WITH_METAL_RECOVERY;
	default:
		return CONVAI_SAT_CAPTURE_LIGHTING_PRESERVE_MATERIAL_SPECULAR;
	}
}

bool TryGetStaticMeshPresentationDirection(
	const AActor& Actor,
	const TArray<UPrimitiveComponent*>& Components,
	FVector& OutDirection,
	float& OutPreference)
{
#if !WITH_EDITOR
	// The presentation hint reads authored thumbnail orbits, which exist only as
	// editor data; packaged runs fall back to the planner's geometric candidates.
	return false;
#else
	const UStaticMesh* StaticMesh = nullptr;
	TArray<const UStaticMeshComponent*> MeshComponents;
	for (const UPrimitiveComponent* Component : Components)
	{
		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			if (const UStaticMesh* ComponentMesh = StaticMeshComponent->GetStaticMesh())
			{
				if (StaticMesh && StaticMesh != ComponentMesh)
				{
					return false;
				}
				StaticMesh = ComponentMesh;
				MeshComponents.Add(StaticMeshComponent);
			}
		}
	}
	if (!StaticMesh || MeshComponents.IsEmpty())
	{
		return false;
	}

	const USceneThumbnailInfo* DefaultInfo = USceneThumbnailInfo::StaticClass()
		->GetDefaultObject<USceneThumbnailInfo>();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7
	const USceneThumbnailInfo* ThumbnailInfo = Cast<USceneThumbnailInfo>(StaticMesh->ThumbnailInfo);
#else
	const USceneThumbnailInfo* ThumbnailInfo = Cast<USceneThumbnailInfo>(StaticMesh->GetThumbnailInfo());
#endif
	const bool bExplicitOrientation = ThumbnailInfo && ThumbnailInfo->DiffersFromDefault();
	ThumbnailInfo = ThumbnailInfo ? ThumbnailInfo : DefaultInfo;
	if (!ThumbnailInfo)
	{
		return false;
	}

	const FRotator RotationOffsetToViewCenter(0.0f, 90.0f, 0.0f);
	const FMatrix ThumbnailView = FRotationMatrix(FRotator(0.0f, ThumbnailInfo->OrbitYaw, 0.0f))
		* FRotationMatrix(FRotator(0.0f, 0.0f, ThumbnailInfo->OrbitPitch))
		* FTranslationMatrix(FVector(0.0f, 1.0f, 0.0f))
		* FInverseRotationMatrix(RotationOffsetToViewCenter);
	const FVector RawDirection = ThumbnailView.InverseTransformPosition(FVector::ZeroVector);
	const FVector Horizontal(RawDirection.X, RawDirection.Y, 0.0f);
	if (Horizontal.IsNearlyZero())
	{
		return false;
	}
	const float Elevation = FMath::Clamp(FMath::Abs(RawDirection.GetSafeNormal().Z), 0.10f, 0.35f);
	const FVector AssetDirection =
		(Horizontal.GetSafeNormal() + FVector::UpVector * Elevation).GetSafeNormal();

	FVector FirstDirection = FVector::ZeroVector;
	FVector CombinedDirection = FVector::ZeroVector;
	for (const UStaticMeshComponent* MeshComponent : MeshComponents)
	{
		const FVector WorldDirection = MeshComponent->GetComponentTransform()
			.TransformVectorNoScale(AssetDirection)
			.GetSafeNormal();
		const FVector ActorDirection = Actor.GetActorTransform()
			.InverseTransformVectorNoScale(WorldDirection)
			.GetSafeNormal();
		if (ActorDirection.IsNearlyZero()
			|| (!FirstDirection.IsNearlyZero()
				&& FVector::DotProduct(FirstDirection, ActorDirection) < 0.985f))
		{
			return false;
		}
		FirstDirection = ActorDirection;
		CombinedDirection += ActorDirection;
	}

	OutDirection = CombinedDirection.GetSafeNormal();
	if (OutDirection.IsNearlyZero())
	{
		return false;
	}
	OutPreference = bExplicitOrientation ? 1.0f : 0.50f;
	return true;
#endif
}

bool IsMetallicMaterialParameter(const FMaterialParameterInfo& ParameterInfo)
{
	return ParameterInfo.Name.ToString().Contains(TEXT("metal"), ESearchCase::IgnoreCase);
}

bool HasStrongMetallicMaterialSignal(const UMaterialInterface& Material)
{
	TArray<FMaterialParameterInfo> Parameters;
	TArray<FGuid> ParameterIds;
	Material.GetAllScalarParameterInfo(Parameters, ParameterIds);
	for (const FMaterialParameterInfo& Parameter : Parameters)
	{
		float Value = 0.0f;
		if (IsMetallicMaterialParameter(Parameter)
			&& Material.GetScalarParameterValue(FHashedMaterialParameterInfo(Parameter), Value)
			&& Value >= 0.50f)
		{
			return true;
		}
	}

	Parameters.Reset();
	ParameterIds.Reset();
	Material.GetAllVectorParameterInfo(Parameters, ParameterIds);
	for (const FMaterialParameterInfo& Parameter : Parameters)
	{
		FLinearColor Value = FLinearColor::Black;
		if (IsMetallicMaterialParameter(Parameter)
			&& Material.GetVectorParameterValue(FHashedMaterialParameterInfo(Parameter), Value)
			&& FMath::Max3(Value.R, Value.G, Value.B) >= 0.50f)
		{
			return true;
		}
	}
	return false;
}

bool HasSingleExplicitMetallicSurface(const TArray<UPrimitiveComponent*>& Components)
{
	TSet<const UMaterialInterface*> Materials;
	for (const UPrimitiveComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}
		for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
		{
			if (const UMaterialInterface* Material = Component->GetMaterial(Index))
			{
				Materials.Add(Material);
			}
		}
	}
	return Materials.Num() == 1
		&& HasStrongMetallicMaterialSignal(**Materials.CreateConstIterator());
}

FVector BoxCorner(const FBox& Box, const int32 X, const int32 Y, const int32 Z)
{
	return FVector(
		X == 0 ? Box.Min.X : Box.Max.X,
		Y == 0 ? Box.Min.Y : Box.Max.Y,
		Z == 0 ? Box.Min.Z : Box.Max.Z);
}

bool BuildComponentProxy(
	const AActor& Actor,
	const UPrimitiveComponent& Component,
	convai_sat_component_proxy_v1& OutProxy)
{
	FMemory::Memzero(&OutProxy, sizeof(OutProxy));
	OutProxy.struct_size = sizeof(OutProxy);

	FBox SourceBounds(ForceInit);
	bool bComponentLocalBounds = false;
	if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(&Component))
	{
		if (const UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh())
		{
			SourceBounds = StaticMesh->GetBoundingBox();
			bComponentLocalBounds = SourceBounds.IsValid != 0;
		}
	}
	if (!SourceBounds.IsValid)
	{
		const FTransform ComponentToActor = Component.GetComponentTransform()
			.GetRelativeTransform(Actor.GetActorTransform());
		SourceBounds = Component.CalcBounds(ComponentToActor).GetBox();
		bComponentLocalBounds = false;
	}
	if (!SourceBounds.IsValid)
	{
		return false;
	}

	const FTransform ActorTransform = Actor.GetActorTransform();
	const FTransform ComponentTransform = Component.GetComponentTransform();
	for (int32 X = 0; X < 2; ++X)
	{
		for (int32 Y = 0; Y < 2; ++Y)
		{
			for (int32 Z = 0; Z < 2; ++Z)
			{
				const FVector SourcePoint = BoxCorner(SourceBounds, X, Y, Z);
				const FVector ActorPoint = bComponentLocalBounds
					? ActorTransform.InverseTransformPosition(
						ComponentTransform.TransformPosition(SourcePoint))
					: SourcePoint;
				if (ActorPoint.ContainsNaN())
				{
					continue;
				}
				OutProxy.points[OutProxy.point_count++] = ToNativeCoordinates(ActorPoint);
			}
		}
	}
	return OutProxy.point_count > 0;
}

bool BuildImageView(
	const TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	const FColor& Background,
	const int32 BackgroundEpsilon,
	const TArray<uint8>* GeometryMask,
	convai_sat_image_view_v1& OutImage)
{
	static_assert(sizeof(FColor) == 4, "The native BGRA8 ABI requires four-byte FColor storage.");
	const int64 ExpectedPixels = static_cast<int64>(Width) * static_cast<int64>(Height);
	if (Width <= 0 || Height <= 0 || ExpectedPixels != Pixels.Num()
		|| Width > static_cast<int32>(CONVAI_SAT_MAX_IMAGE_DIMENSION)
		|| Height > static_cast<int32>(CONVAI_SAT_MAX_IMAGE_DIMENSION)
		|| ExpectedPixels > static_cast<int64>(CONVAI_SAT_MAX_IMAGE_PIXELS)
		|| (GeometryMask && GeometryMask->Num() != Pixels.Num()))
	{
		return false;
	}

	FMemory::Memzero(&OutImage, sizeof(OutImage));
	OutImage.struct_size = sizeof(OutImage);
	OutImage.width = static_cast<uint32>(Width);
	OutImage.height = static_cast<uint32>(Height);
	OutImage.row_stride_bytes = static_cast<uint32>(Width * sizeof(FColor));
	OutImage.pixel_format = CONVAI_SAT_PIXEL_FORMAT_BGRA8_UNORM;
	OutImage.pixels = reinterpret_cast<const uint8*>(Pixels.GetData());
	OutImage.pixel_bytes = static_cast<uint64>(Pixels.Num()) * sizeof(FColor);
	if (GeometryMask)
	{
		OutImage.geometry_mask = GeometryMask->GetData();
		OutImage.geometry_mask_row_stride_bytes = static_cast<uint32>(Width);
		OutImage.geometry_mask_bytes = static_cast<uint64>(GeometryMask->Num());
	}
	OutImage.background_epsilon = static_cast<uint32>(FMath::Clamp(BackgroundEpsilon, 0, 255));
	OutImage.background_rgba[0] = Background.R;
	OutImage.background_rgba[1] = Background.G;
	OutImage.background_rgba[2] = Background.B;
	OutImage.background_rgba[3] = Background.A;
	return true;
}

bool BuildImageView(
	const TArray<uint8>& BGRA,
	const FIntPoint& Size,
	convai_sat_image_view_v1& OutImage)
{
	if (Size.X <= 0 || Size.Y <= 0
		|| Size.X > static_cast<int32>(CONVAI_SAT_MAX_IMAGE_DIMENSION)
		|| Size.Y > static_cast<int32>(CONVAI_SAT_MAX_IMAGE_DIMENSION)
		|| static_cast<int64>(Size.X) * static_cast<int64>(Size.Y)
			> static_cast<int64>(CONVAI_SAT_MAX_IMAGE_PIXELS))
	{
		return false;
	}
	const int64 PixelCount = static_cast<int64>(Size.X) * static_cast<int64>(Size.Y);
	const int64 ExpectedBytes = PixelCount * 4;
	if (ExpectedBytes != BGRA.Num())
	{
		return false;
	}

	FMemory::Memzero(&OutImage, sizeof(OutImage));
	OutImage.struct_size = sizeof(OutImage);
	OutImage.width = static_cast<uint32>(Size.X);
	OutImage.height = static_cast<uint32>(Size.Y);
	OutImage.row_stride_bytes = static_cast<uint32>(Size.X * 4);
	OutImage.pixel_format = CONVAI_SAT_PIXEL_FORMAT_BGRA8_UNORM;
	OutImage.pixels = BGRA.GetData();
	OutImage.pixel_bytes = static_cast<uint64>(BGRA.Num());
	OutImage.background_epsilon = 8;
	OutImage.background_rgba[0] = BGRA[2];
	OutImage.background_rgba[1] = BGRA[1];
	OutImage.background_rgba[2] = BGRA[0];
	OutImage.background_rgba[3] = BGRA[3];
	return true;
}

FSceneAutoTaggerViewMetrics FromNativeMetrics(const convai_sat_view_metrics_v1& Native)
{
	FSceneAutoTaggerViewMetrics Result;
	Result.bValid = Native.valid != 0;
	Result.ForegroundPixels = Native.foreground_pixels;
	Result.InteriorPixels = Native.interior_pixels;
	Result.ForegroundFraction = Native.foreground_fraction;
	Result.BorderTouchFraction = Native.border_touch_fraction;
	Result.ColorEntropy = Native.color_entropy;
	Result.EdgeDetail = Native.edge_detail;
	Result.SilhouetteInformation = Native.silhouette_information;
	Result.bShapeProfileCandidate = Native.shape_profile_candidate != 0;
	Result.CentralForegroundPixels = Native.central_foreground_pixels;
	Result.CentralToneEntropy = Native.central_tone_entropy;
	Result.CentralColorEntropy = Native.central_color_entropy;
	Result.CentralEdgeDetail = Native.central_edge_detail;
	Result.CentralForegroundFill = Native.central_foreground_fill;
	Result.CentralAuthoredContent = Native.central_authored_content;
	Result.CaptureQuality = Native.capture_quality;
	Result.FrontPreference = Native.front_preference;
	Result.SelectionScore = Native.selection_score;
	return Result;
}

convai_sat_view_metrics_v1 ToNativeMetrics(const FSceneAutoTaggerViewMetrics& Source)
{
	convai_sat_view_metrics_v1 Result = {};
	Result.struct_size = sizeof(Result);
	Result.valid = Source.bValid ? 1u : 0u;
	Result.foreground_pixels = Source.ForegroundPixels;
	Result.interior_pixels = Source.InteriorPixels;
	Result.foreground_fraction = Source.ForegroundFraction;
	Result.border_touch_fraction = Source.BorderTouchFraction;
	Result.color_entropy = Source.ColorEntropy;
	Result.edge_detail = Source.EdgeDetail;
	Result.silhouette_information = Source.SilhouetteInformation;
	Result.shape_profile_candidate = Source.bShapeProfileCandidate ? 1u : 0u;
	Result.central_foreground_pixels = Source.CentralForegroundPixels;
	Result.central_tone_entropy = Source.CentralToneEntropy;
	Result.central_color_entropy = Source.CentralColorEntropy;
	Result.central_edge_detail = Source.CentralEdgeDetail;
	Result.central_foreground_fill = Source.CentralForegroundFill;
	Result.central_authored_content = Source.CentralAuthoredContent;
	Result.capture_quality = Source.CaptureQuality;
	Result.front_preference = Source.FrontPreference;
	Result.selection_score = Source.SelectionScore;
	return Result;
}
}

#if WITH_DEV_AUTOMATION_TESTS
FVector ConvertUnrealToNativeCoordinatesForTesting(const FVector& UnrealVector)
{
	const convai_sat_vec3d NativeVector = ToNativeCoordinates(UnrealVector);
	return FVector(NativeVector.x, NativeVector.y, NativeVector.z);
}

FVector ConvertNativeToUnrealCoordinatesForTesting(const FVector& NativeVector)
{
	return FromNativeCoordinates({ NativeVector.X, NativeVector.Y, NativeVector.Z });
}
#endif

FSceneAutoTaggerViewPlan BuildActorLocalViewPlan(
	const AActor& Actor,
	const TArray<UPrimitiveComponent*>& Components)
{
	FSceneAutoTaggerViewPlan Result;
	const convai_sat_api_v4* Api = GetApi(TEXT("view planning"));
	if (!Api || !Api->build_view_plan)
	{
		return Result;
	}

	TArray<convai_sat_component_proxy_v1> Proxies;
	Proxies.Reserve(FMath::Min(Components.Num(), static_cast<int32>(CONVAI_SAT_MAX_COMPONENT_PROXIES)));
	for (const UPrimitiveComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}
		if (Proxies.Num() >= static_cast<int32>(CONVAI_SAT_MAX_COMPONENT_PROXIES))
		{
			LogNativeFailure(TEXT("view planning (too many component proxies)"));
			return {};
		}
		convai_sat_component_proxy_v1 Proxy;
		if (BuildComponentProxy(Actor, *Component, Proxy))
		{
			Proxies.Add(Proxy);
		}
	}

	convai_sat_view_plan_input_v1 Input = {};
	Input.struct_size = sizeof(Input);
	Input.flags = CONVAI_SAT_VIEW_PLAN_PRESERVE_REFLECTED_Y_ORDER;
	Input.component_count = static_cast<uint32>(Proxies.Num());
	Input.components = Proxies.GetData();
	FVector PresentationDirection;
	float PresentationPreference = 0.0f;
	if (TryGetStaticMeshPresentationDirection(
		Actor,
		Components,
		PresentationDirection,
		PresentationPreference))
	{
		Input.flags |= CONVAI_SAT_VIEW_PLAN_HAS_PRESENTATION_HINT;
		Input.presentation_direction_actor_local = ToNativeCoordinates(PresentationDirection);
		Input.presentation_preference = PresentationPreference;
	}
	if (HasSingleExplicitMetallicSurface(Components))
	{
		Input.flags |= CONVAI_SAT_VIEW_PLAN_SINGLE_EXPLICIT_METALLIC_SURFACE;
	}

	convai_sat_view_plan_v1 NativePlan = {};
	NativePlan.struct_size = sizeof(NativePlan);
	convai_sat_error_v1 Error = MakeError();
	if (Api->build_view_plan(&Input, &NativePlan, &Error) != CONVAI_SAT_STATUS_OK
		|| NativePlan.candidate_count > CONVAI_SAT_MAX_VIEW_CANDIDATES)
	{
		LogNativeFailure(TEXT("view planning"), &Error);
		return {};
	}

	switch (NativePlan.lighting_policy)
	{
	case CONVAI_SAT_CAPTURE_LIGHTING_PRESERVE_MATERIAL_SPECULAR:
		Result.CaptureLightingPolicy = ESceneAutoTaggerCaptureLightingPolicy::PreserveMaterialSpecular;
		break;
	case CONVAI_SAT_CAPTURE_LIGHTING_SUPPRESS_DIRECT_SPECULAR:
		Result.CaptureLightingPolicy = ESceneAutoTaggerCaptureLightingPolicy::SuppressDirectSpecular;
		break;
	case CONVAI_SAT_CAPTURE_LIGHTING_SUPPRESS_DIRECT_SPECULAR_WITH_METAL_RECOVERY:
		Result.CaptureLightingPolicy =
			ESceneAutoTaggerCaptureLightingPolicy::SuppressDirectSpecularWithMetalRecovery;
		break;
	default:
		LogNativeFailure(TEXT("view planning (unknown lighting policy)"));
		return {};
	}

	Result.bPlanar = (NativePlan.flags & CONVAI_SAT_VIEW_PLAN_PLANAR) != 0;
	Result.bOpposedBroadFacePresentation =
		(NativePlan.flags & CONVAI_SAT_VIEW_PLAN_OPPOSED_BROAD_FACES) != 0;
	Result.CameraOffsetsActorLocal.Reserve(NativePlan.candidate_count);
	Result.FrontPreferences.Reserve(NativePlan.candidate_count);
	Result.ShapeProfileCandidates.Reserve(NativePlan.candidate_count);
	for (uint32 Index = 0; Index < NativePlan.candidate_count; ++Index)
	{
		const convai_sat_view_candidate_v1& Candidate = NativePlan.candidates[Index];
		Result.CameraOffsetsActorLocal.Add(FromNativeCoordinates(Candidate.direction_actor_local));
		Result.FrontPreferences.Add(Candidate.front_preference);
		Result.ShapeProfileCandidates.Add(
			(Candidate.flags & CONVAI_SAT_VIEW_CANDIDATE_SHAPE_PROFILE) != 0);
	}
	return Result;
}

bool SelectCapturedView(
	const FSceneAutoTaggerViewPlan& Plan,
	const TArray<FSceneAutoTaggerProbeImage>& Probes,
	FSceneAutoTaggerSelectViewResult& OutResult)
{
	OutResult = FSceneAutoTaggerSelectViewResult();
	const int32 CandidateCount = FMath::Min(
		Plan.CameraOffsetsActorLocal.Num(),
		static_cast<int32>(CONVAI_SAT_MAX_VIEW_CANDIDATES));
	if (CandidateCount <= 0 || Probes.IsEmpty() || Probes.Num() > CandidateCount)
	{
		return false;
	}

	const convai_sat_api_v4* Api = GetApi(TEXT("view selection"));
	if (!Api || !Api->select_view)
	{
		return false;
	}

	convai_sat_view_plan_v1 NativePlan = {};
	NativePlan.struct_size = sizeof(NativePlan);
	NativePlan.candidate_count = static_cast<uint32>(CandidateCount);
	if (Plan.bPlanar)
	{
		NativePlan.flags |= CONVAI_SAT_VIEW_PLAN_PLANAR;
	}
	if (Plan.bOpposedBroadFacePresentation)
	{
		NativePlan.flags |= CONVAI_SAT_VIEW_PLAN_OPPOSED_BROAD_FACES;
	}
	NativePlan.lighting_policy = ToNativeLightingPolicy(Plan.CaptureLightingPolicy);
	for (int32 Index = 0; Index < CandidateCount; ++Index)
	{
		convai_sat_view_candidate_v1& Candidate = NativePlan.candidates[Index];
		Candidate.direction_actor_local =
			ToNativeCoordinates(Plan.CameraOffsetsActorLocal[Index]);
		Candidate.front_preference = Plan.FrontPreferences.IsValidIndex(Index)
			? Plan.FrontPreferences[Index]
			: 0.0f;
		if (Plan.ShapeProfileCandidates.IsValidIndex(Index)
			&& Plan.ShapeProfileCandidates[Index])
		{
			Candidate.flags |= CONVAI_SAT_VIEW_CANDIDATE_SHAPE_PROFILE;
		}
	}

	TArray<convai_sat_image_view_v1> Images;
	Images.SetNumZeroed(Probes.Num());
	bool bAnyPresent = false;
	for (int32 Index = 0; Index < Probes.Num(); ++Index)
	{
		const FSceneAutoTaggerProbeImage& Probe = Probes[Index];
		if (!Probe.bPresent)
		{
			// An absent probe keeps its slot: struct_size set, pixels NULL, pixel_bytes 0.
			Images[Index].struct_size = sizeof(convai_sat_image_view_v1);
			continue;
		}
		if (!BuildImageView(
			Probe.Pixels,
			Probe.Width,
			Probe.Height,
			Probe.Background,
			Probe.BackgroundEpsilon,
			Probe.GeometryMask.IsEmpty() ? nullptr : &Probe.GeometryMask,
			Images[Index]))
		{
			return false;
		}
		bAnyPresent = true;
	}
	if (!bAnyPresent)
	{
		return false;
	}

	convai_sat_select_view_input_v4 Input = {};
	Input.struct_size = sizeof(Input);
	Input.image_count = static_cast<uint32>(Images.Num());
	Input.plan = &NativePlan;
	Input.images = Images.GetData();
	convai_sat_select_view_result_v4 NativeResult = {};
	NativeResult.struct_size = sizeof(NativeResult);
	convai_sat_error_v1 Error = MakeError();
	if (Api->select_view(&Input, &NativeResult, &Error) != CONVAI_SAT_STATUS_OK)
	{
		LogNativeFailure(TEXT("view selection"), &Error);
		return false;
	}
	if (NativeResult.selected_index != INDEX_NONE
		&& !Probes.IsValidIndex(NativeResult.selected_index))
	{
		LogNativeFailure(TEXT("view selection (invalid result index)"));
		return false;
	}

	OutResult.SelectedIndex = NativeResult.selected_index;
	OutResult.SelectionMargin = NativeResult.selection_margin;
	OutResult.SelectedScore = NativeResult.selected_score;
	OutResult.Metrics.SetNum(Probes.Num());
	const int32 MetricCount = FMath::Min(
		static_cast<int32>(NativeResult.metric_count),
		OutResult.Metrics.Num());
	for (int32 Index = 0; Index < MetricCount; ++Index)
	{
		OutResult.Metrics[Index] = FromNativeMetrics(NativeResult.metrics[Index]);
	}
	return true;
}

bool EvaluateFinalCapture(
	const TArray<uint8>& BGRA,
	const FIntPoint& Size,
	const TArray<FSceneAutoTaggerViewMetrics>& ProbeMetrics,
	const FSceneAutoTaggerCaptureFacts& Facts,
	FSceneAutoTaggerCaptureEvaluation& OutEvaluation)
{
	OutEvaluation = FSceneAutoTaggerCaptureEvaluation();
	const convai_sat_api_v4* Api = GetApi(TEXT("capture evaluation"));
	convai_sat_evaluate_capture_input_v4 Input = {};
	Input.struct_size = sizeof(Input);
	if (!Api || !Api->evaluate_capture || !BuildImageView(BGRA, Size, Input.final_image))
	{
		return false;
	}

	if (Facts.bSingleCandidateRequest)
	{
		Input.flags |= CONVAI_SAT_CAPTURE_FACTS_SINGLE_CANDIDATE_REQUEST;
	}
	if (Facts.bReviewBatchRequest)
	{
		Input.flags |= CONVAI_SAT_CAPTURE_FACTS_REVIEW_BATCH_REQUEST;
	}
	if (Facts.bForceExplicitScope)
	{
		Input.flags |= CONVAI_SAT_CAPTURE_FACTS_FORCE_EXPLICIT_SCOPE;
	}
	if (Facts.bOpposedBroadFace)
	{
		Input.flags |= CONVAI_SAT_CAPTURE_FACTS_OPPOSED_BROAD_FACE;
	}
	if (Facts.bHasExistingObject)
	{
		Input.flags |= CONVAI_SAT_CAPTURE_FACTS_HAS_EXISTING_OBJECT;
	}
	Input.significance_score = Facts.SignificanceScore;
	Input.primitive_count = static_cast<uint32>(FMath::Max(Facts.PrimitiveCount, 0));
	Input.significance_reason_flags = Facts.NativeSignificanceReasonFlags;

	TArray<convai_sat_view_metrics_v1> NativeProbeMetrics;
	NativeProbeMetrics.Reserve(ProbeMetrics.Num());
	for (const FSceneAutoTaggerViewMetrics& Metric : ProbeMetrics)
	{
		// Absent probes travel as fully zeroed entries, which the core skips.
		convai_sat_view_metrics_v1 NativeMetric = {};
		if (Metric.bValid)
		{
			NativeMetric = ToNativeMetrics(Metric);
		}
		NativeProbeMetrics.Add(NativeMetric);
	}
	Input.probe_metric_count = static_cast<uint32>(NativeProbeMetrics.Num());
	Input.probe_metrics = NativeProbeMetrics.GetData();

	convai_sat_evaluate_capture_result_v4 NativeResult = {};
	NativeResult.struct_size = sizeof(NativeResult);
	convai_sat_error_v1 Error = MakeError();
	if (Api->evaluate_capture(&Input, &NativeResult, &Error) != CONVAI_SAT_STATUS_OK)
	{
		LogNativeFailure(TEXT("capture evaluation"), &Error);
		return false;
	}

	OutEvaluation.bRetain = NativeResult.decision != CONVAI_SAT_ELIGIBILITY_EXCLUDE;
	OutEvaluation.bExcludedForVisibility =
		NativeResult.reason == CONVAI_SAT_ELIGIBILITY_REASON_OBJECT_NOT_CLEARLY_VISIBLE;
	OutEvaluation.bExcludedForNoUsefulDetail =
		NativeResult.reason == CONVAI_SAT_ELIGIBILITY_REASON_NO_USEFUL_VISUAL_DETAIL;
	OutEvaluation.bReadableCenteredCoverage = NativeResult.readable_centered_coverage != 0;
	OutEvaluation.bInformativeEvidence = NativeResult.informative_evidence != 0;
	OutEvaluation.bMeaningfulMidScaleStructure =
		NativeResult.meaningful_mid_scale_structure != 0;
	OutEvaluation.bDescriptorValid = NativeResult.descriptor_valid != 0;
	if (OutEvaluation.bDescriptorValid)
	{
		OutEvaluation.Descriptor.NativeDescriptorBytes.SetNumUninitialized(
			static_cast<int32>(sizeof(NativeResult.descriptor)));
		FMemory::Memcpy(
			OutEvaluation.Descriptor.NativeDescriptorBytes.GetData(),
			&NativeResult.descriptor,
			sizeof(NativeResult.descriptor));
	}
	return true;
}

bool BuildCapturedViewDescriptor(
	const TArray<uint8>& BGRA,
	const FIntPoint& Size,
	FSceneAutoTaggerEvidenceDescriptor& OutDescriptor)
{
	OutDescriptor.NativeDescriptorBytes.Reset();
	FSceneAutoTaggerCaptureFacts Facts;
	Facts.bSingleCandidateRequest = true;
	Facts.SignificanceScore = 1.0f;
	Facts.PrimitiveCount = 2;
	FSceneAutoTaggerCaptureEvaluation Evaluation;
	if (!EvaluateFinalCapture(BGRA, Size, {}, Facts, Evaluation)
		|| !Evaluation.bDescriptorValid)
	{
		return false;
	}
	OutDescriptor = MoveTemp(Evaluation.Descriptor);
	return true;
}

bool AreCapturedViewDescriptorsVisuallyEquivalent(
	const FSceneAutoTaggerEvidenceDescriptor& Left,
	const FSceneAutoTaggerEvidenceDescriptor& Right)
{
	if (Left.NativeDescriptorBytes.Num()
			!= static_cast<int32>(sizeof(convai_sat_evidence_descriptor_v1))
		|| Right.NativeDescriptorBytes.Num()
			!= static_cast<int32>(sizeof(convai_sat_evidence_descriptor_v1)))
	{
		return false;
	}
	const convai_sat_api_v4* Api = GetApi(TEXT("evidence comparison"));
	if (!Api || !Api->compare_evidence)
	{
		return false;
	}

	convai_sat_evidence_descriptor_v1 NativeLeft;
	convai_sat_evidence_descriptor_v1 NativeRight;
	FMemory::Memcpy(&NativeLeft, Left.NativeDescriptorBytes.GetData(), sizeof(NativeLeft));
	FMemory::Memcpy(&NativeRight, Right.NativeDescriptorBytes.GetData(), sizeof(NativeRight));
	uint32 bEquivalent = 0;
	convai_sat_error_v1 Error = MakeError();
	if (Api->compare_evidence(
		&NativeLeft,
		&NativeRight,
		&bEquivalent,
		&Error) != CONVAI_SAT_STATUS_OK)
	{
		LogNativeFailure(TEXT("evidence comparison"), &Error);
		return false;
	}
	return bEquivalent != 0;
}

FString NativeCorePolicyId()
{
	const FSceneAutoTaggerNativeCoreAdapter& Adapter =
		FSceneAutoTaggerNativeCoreAdapter::Get();
	const convai_sat_api_v4* Api = Adapter.IsAvailable() ? Adapter.GetApi() : nullptr;
	if (!Api || !Api->view_policy_id_utf8)
	{
		return FString::Printf(
			TEXT("native-abi=%u;native-policy=unavailable"),
			CONVAI_SAT_ABI_VERSION_4);
	}
	return FString::Printf(
		TEXT("native-core=%s;view-abi=%u;view=%s"),
		UTF8_TO_TCHAR(Api->core_version_utf8),
		Api->abi_version,
		UTF8_TO_TCHAR(Api->view_policy_id_utf8));
}

const TCHAR* CapturePolicyVersion()
{
	return TEXT("ue-capture-input-v1-thumbnail-presentation-prior-v1-component-relative-presentation-v1-component-corner-proxy-v1-ldr-srgb-target-v1-rgb64-probe384-midscale-v4-final-fast-residency-v4-preprobe-render-readiness-v1-first-probe-render-prime-v1-synthetic-streaming-view-v1-profile-generation-settle-v1-srgb-adaptive-resize-v1-scene-authored-lighting-safe-pp-relative-ev-diffuse-fill-planar-diffuse-exposure-v2-opposing-diffuse-shadow-recovery-v1-dimensional-shadow-balance-v3-material-gated-metal-key-recovery-v6-live-calibrated-after-warmup-v4-dual-dark-neutral-backdrop-v1-geometry-depth-mask-v1-actor-owned-evidence-v1");
}

FString BuildCapturePolicyId(
	const int32 EvidenceCaptureResolution,
	const int32 ModelCellResolution,
	const int32 GridDimension)
{
	return FString::Printf(
		TEXT("%s;evidence=%d;model-cell=%d;grid=%d;fov=35;fill=0.80"),
		CapturePolicyVersion(),
		FMath::Clamp(EvidenceCaptureResolution, 256, 1024),
		FMath::Clamp(ModelCellResolution, 128, 512),
		FMath::Clamp(GridDimension, 2, 5));
}
}
