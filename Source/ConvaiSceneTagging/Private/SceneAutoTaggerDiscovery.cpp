
// Copyright Convai. All Rights Reserved.

#include "SceneAutoTaggerDiscovery.h"

#include "ConvaiObjectComponent.h"
#include "ConvaiSceneAutoTaggerSettings.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Brush.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Info.h"
#include "GameFramework/Volume.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiSceneAutoTaggerDiscovery, Log, All);

namespace
{
bool IsGenericActorLabel(const FString& LowerLabel)
{
	return LowerLabel.StartsWith(TEXT("staticmeshactor"))
		|| LowerLabel.StartsWith(TEXT("actor_"))
		|| LowerLabel.StartsWith(TEXT("bp_actor"));
}

bool IsInfrastructureActor(const AActor* Actor)
{
	return !Actor
		|| Actor->IsA<AWorldSettings>()
		|| Actor->IsA<ALevelScriptActor>()
		|| Actor->IsA<ABrush>()
		|| Actor->IsA<AVolume>()
		|| Actor->IsA<AInfo>();
}

bool IsRenderablePrimitive(const UPrimitiveComponent* Component)
{
	return Component
		&& !Component->HasAnyFlags(RF_Transient)
		&& !Component->IsA<UBillboardComponent>()
		&& !Component->IsA<UArrowComponent>()
		&& !Component->IsA<UShapeComponent>()
		&& Component->Bounds.SphereRadius > KINDA_SMALL_NUMBER;
}

bool IsEngineBasicShape(const UPrimitiveComponent* Component)
{
	const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
	const UStaticMesh* Mesh = StaticMeshComponent ? StaticMeshComponent->GetStaticMesh() : nullptr;
	return Mesh && Mesh->GetPathName().StartsWith(TEXT("/Engine/BasicShapes/"), ESearchCase::IgnoreCase);
}

bool HasAuthoredMaterialOverride(const UPrimitiveComponent* Component)
{
	const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
	const UStaticMesh* Mesh = StaticMeshComponent ? StaticMeshComponent->GetStaticMesh() : nullptr;
	if (!Mesh)
	{
		return false;
	}

	const int32 MaterialSlotCount = FMath::Max(
		StaticMeshComponent->GetNumMaterials(),
		Mesh->GetStaticMaterials().Num());
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialSlotCount; ++MaterialIndex)
	{
		const UMaterialInterface* ComponentMaterial = StaticMeshComponent->GetMaterial(MaterialIndex);
		const UMaterialInterface* MeshMaterial = Mesh->GetMaterial(MaterialIndex);
		if (ComponentMaterial && ComponentMaterial != MeshMaterial)
		{
			return true;
		}
	}
	return false;
}

bool HasPanelLikeDimensions(const FVector& Size)
{
	TArray<double, TInlineAllocator<3>> Dimensions {
		FMath::Abs(Size.X),
		FMath::Abs(Size.Y),
		FMath::Abs(Size.Z)
	};
	Dimensions.Sort();
	// Keep paintings, signs, and shallow framed artwork made from stock Plane/Cube
	// meshes, but do not treat an arbitrary colored cube as meaningful content.
	return Dimensions[1] >= 20.0
		&& Dimensions[2] >= 20.0
		&& Dimensions[0] <= FMath::Max(6.0, Dimensions[1] * 0.15);
}

bool IsPanelLike(const FBox& Bounds)
{
	return Bounds.IsValid && HasPanelLikeDimensions(Bounds.GetSize());
}

bool IsPanelLike(const UPrimitiveComponent* Component)
{
	const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
	const UStaticMesh* Mesh = StaticMeshComponent ? StaticMeshComponent->GetStaticMesh() : nullptr;
	if (!Mesh)
	{
		return false;
	}
	// Asset-local dimensions with component scale avoid turning a rotated wall
	// painting into a seemingly dimensional world-axis AABB.
	return HasPanelLikeDimensions(
		Mesh->GetBoundingBox().GetSize() * StaticMeshComponent->GetComponentScale().GetAbs());
}

/**
 * A bounded surface profile used to identify a single custom mesh that carries
 * little geometric information. Triangle area, rather than vertex count, is
 * accumulated into normal and supporting-plane clusters. Dense coplanar
 * tessellation therefore does not make a box or flat slab look more detailed.
 */
bool HasLowGeometryInformation(const UPrimitiveComponent* Component)
{
	const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
	const UStaticMesh* Mesh = StaticMeshComponent ? StaticMeshComponent->GetStaticMesh() : nullptr;
	const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
	const FStaticMeshLODResources* Lod = RenderData ? RenderData->GetCurrentFirstLOD(0) : nullptr;
	if (!Lod)
	{
		return false;
	}

	const FPositionVertexBuffer& Positions = Lod->VertexBuffers.PositionVertexBuffer;
	const FIndexArrayView Indices = Lod->IndexBuffer.GetArrayView();
	if (!Positions.GetVertexData() || Positions.GetNumVertices() == 0 || Indices.Num() < 6)
	{
		// Unknown geometry is never filtered merely because its CPU render data is
		// unavailable (for example while a streamed LOD is transitioning).
		return false;
	}

	struct FSurfaceSample
	{
		FVector3d Normal = FVector3d::ZeroVector;
		double PlaneOffset = 0.0;
		double Area = 0.0;
	};
	struct FNormalCluster
	{
		FVector3d WeightedNormal = FVector3d::ZeroVector;
		double Area = 0.0;
	};
	struct FPlaneCluster
	{
		int32 NormalClusterIndex = INDEX_NONE;
		double WeightedOffset = 0.0;
		double Area = 0.0;
	};

	constexpr int32 MaxSampledTriangles = 4096;
	constexpr double NormalClusterCosine = 0.984807753; // 10 degrees.
	constexpr double SignificantNormalAreaFraction = 0.015;
	constexpr double MinimumRectilinearCoverageFraction = 0.90;
	constexpr double SignificantPlaneAreaFraction = 0.015;
	const int32 TriangleCount = Indices.Num() / 3;
	const int32 TriangleStride = FMath::Max(1, FMath::DivideAndRoundUp(TriangleCount, MaxSampledTriangles));

	TArray<FSurfaceSample, TInlineAllocator<128>> Samples;
	Samples.Reserve(FMath::Min(TriangleCount, MaxSampledTriangles));
	double TotalSampledArea = 0.0;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; TriangleIndex += TriangleStride)
	{
		const int32 FirstIndex = TriangleIndex * 3;
		const uint32 Index0 = Indices[FirstIndex];
		const uint32 Index1 = Indices[FirstIndex + 1];
		const uint32 Index2 = Indices[FirstIndex + 2];
		if (Index0 >= Positions.GetNumVertices()
			|| Index1 >= Positions.GetNumVertices()
			|| Index2 >= Positions.GetNumVertices())
		{
			continue;
		}

		const FVector3d Position0(Positions.VertexPosition(Index0));
		const FVector3d Position1(Positions.VertexPosition(Index1));
		const FVector3d Position2(Positions.VertexPosition(Index2));
		const FVector3d Cross = FVector3d::CrossProduct(Position1 - Position0, Position2 - Position0);
		const double DoubleArea = Cross.Size();
		if (!FMath::IsFinite(DoubleArea) || DoubleArea <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}

		FVector3d Normal = Cross / DoubleArea;
		// Canonicalize opposing faces into one information direction while
		// retaining signed supporting-plane offsets.
		if (Normal.X < -KINDA_SMALL_NUMBER
			|| (FMath::IsNearlyZero(Normal.X) && Normal.Y < -KINDA_SMALL_NUMBER)
			|| (FMath::IsNearlyZero(Normal.X) && FMath::IsNearlyZero(Normal.Y) && Normal.Z < 0.0))
		{
			Normal *= -1.0;
		}

		const double Area = DoubleArea * 0.5;
		const FVector3d Centroid = (Position0 + Position1 + Position2) / 3.0;
		Samples.Add({ Normal, FVector3d::DotProduct(Normal, Centroid), Area });
		TotalSampledArea += Area;
	}
	if (Samples.Num() < 2 || TotalSampledArea <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}

	TArray<FNormalCluster, TInlineAllocator<16>> NormalClusters;
	for (const FSurfaceSample& Sample : Samples)
	{
		int32 BestClusterIndex = INDEX_NONE;
		double BestDot = NormalClusterCosine;
		for (int32 ClusterIndex = 0; ClusterIndex < NormalClusters.Num(); ++ClusterIndex)
		{
			const FVector3d ClusterNormal = NormalClusters[ClusterIndex].WeightedNormal.GetSafeNormal();
			const double Dot = FVector3d::DotProduct(Sample.Normal, ClusterNormal);
			if (Dot >= BestDot)
			{
				BestDot = Dot;
				BestClusterIndex = ClusterIndex;
			}
		}
		if (BestClusterIndex == INDEX_NONE)
		{
			NormalClusters.Add({ Sample.Normal * Sample.Area, Sample.Area });
		}
		else
		{
			NormalClusters[BestClusterIndex].WeightedNormal += Sample.Normal * Sample.Area;
			NormalClusters[BestClusterIndex].Area += Sample.Area;
		}
	}

	TArray<int32, TInlineAllocator<8>> SignificantNormalClusters;
	double SignificantNormalArea = 0.0;
	for (int32 ClusterIndex = 0; ClusterIndex < NormalClusters.Num(); ++ClusterIndex)
	{
		if (NormalClusters[ClusterIndex].Area >= TotalSampledArea * SignificantNormalAreaFraction)
		{
			SignificantNormalClusters.Add(ClusterIndex);
			SignificantNormalArea += NormalClusters[ClusterIndex].Area;
		}
	}
	if (SignificantNormalClusters.IsEmpty()
		|| SignificantNormalClusters.Num() > 4
		|| SignificantNormalArea < TotalSampledArea * MinimumRectilinearCoverageFraction)
	{
		// A sculpture can have a few broad planes, but its facial, drapery, and
		// silhouette curvature is spread across many smaller normal clusters. Do
		// not discard that distributed surface information as tessellation noise.
		return false;
	}
	constexpr double RectilinearDotTolerance = 0.258819045; // 15 degrees from perpendicular.
	for (int32 LeftIndex = 0; LeftIndex < SignificantNormalClusters.Num(); ++LeftIndex)
	{
		const FVector3d LeftNormal = NormalClusters[SignificantNormalClusters[LeftIndex]].WeightedNormal.GetSafeNormal();
		for (int32 RightIndex = LeftIndex + 1; RightIndex < SignificantNormalClusters.Num(); ++RightIndex)
		{
			const FVector3d RightNormal = NormalClusters[SignificantNormalClusters[RightIndex]].WeightedNormal.GetSafeNormal();
			if (FMath::Abs(FVector3d::DotProduct(LeftNormal, RightNormal)) > RectilinearDotTolerance)
			{
				// A tetrahedron, pyramid, or other deliberately faceted prop is not
				// equivalent to an axis-aligned block merely because it has few faces.
				return false;
			}
		}
	}

	const double MeshLongestSide = Mesh->GetBoundingBox().GetSize().GetMax();
	const double PlaneOffsetTolerance = FMath::Max(0.25, MeshLongestSide * 0.005);
	TArray<FPlaneCluster, TInlineAllocator<24>> PlaneClusters;
	for (const FSurfaceSample& Sample : Samples)
	{
		int32 BestNormalClusterIndex = INDEX_NONE;
		double BestDot = NormalClusterCosine;
		for (const int32 ClusterIndex : SignificantNormalClusters)
		{
			const FVector3d ClusterNormal = NormalClusters[ClusterIndex].WeightedNormal.GetSafeNormal();
			const double Dot = FVector3d::DotProduct(Sample.Normal, ClusterNormal);
			if (Dot >= BestDot)
			{
				BestDot = Dot;
				BestNormalClusterIndex = ClusterIndex;
			}
		}
		if (BestNormalClusterIndex == INDEX_NONE)
		{
			continue;
		}

		int32 BestPlaneClusterIndex = INDEX_NONE;
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneClusters.Num(); ++PlaneIndex)
		{
			const FPlaneCluster& Plane = PlaneClusters[PlaneIndex];
			const double ExistingOffset = Plane.Area > UE_DOUBLE_SMALL_NUMBER
				? Plane.WeightedOffset / Plane.Area
				: Plane.WeightedOffset;
			if (Plane.NormalClusterIndex == BestNormalClusterIndex
				&& FMath::Abs(Sample.PlaneOffset - ExistingOffset) <= PlaneOffsetTolerance)
			{
				BestPlaneClusterIndex = PlaneIndex;
				break;
			}
		}
		if (BestPlaneClusterIndex == INDEX_NONE)
		{
			PlaneClusters.Add({ BestNormalClusterIndex, Sample.PlaneOffset * Sample.Area, Sample.Area });
		}
		else
		{
			PlaneClusters[BestPlaneClusterIndex].WeightedOffset += Sample.PlaneOffset * Sample.Area;
			PlaneClusters[BestPlaneClusterIndex].Area += Sample.Area;
		}
	}

	int32 SignificantPlaneCount = 0;
	for (const FPlaneCluster& Plane : PlaneClusters)
	{
		SignificantPlaneCount += Plane.Area >= TotalSampledArea * SignificantPlaneAreaFraction ? 1 : 0;
	}
	return SignificantPlaneCount > 0 && SignificantPlaneCount <= 12;
}

bool HasAuthoredTexture(const TSet<const UMaterialInterface*>& Materials)
{
	for (const UMaterialInterface* Material : Materials)
	{
		if (!Material)
		{
			continue;
		}
		TArray<UTexture*> UsedTextures;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7
		Material->GetUsedTextures(UsedTextures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true);
#else
		Material->GetUsedTextures(UsedTextures);
#endif
		for (const UTexture* Texture : UsedTextures)
		{
			if (Texture && !Texture->GetPathName().StartsWith(TEXT("/Engine/"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
	}
	return false;
}

FString ComponentAssetName(const UPrimitiveComponent* Component)
{
	if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
	{
		if (const UStaticMesh* Mesh = StaticMeshComponent->GetStaticMesh())
		{
			return Mesh->GetName();
		}
	}
	if (const USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
	{
		if (const USkeletalMesh* Mesh = SkeletalMeshComponent->GetSkeletalMeshAsset())
		{
			return Mesh->GetName();
		}
	}
	return Component ? Component->GetClass()->GetName() : FString();
}

FString HumanizeIdentifier(FString Value)
{
	Value.TrimStartAndEndInline();
	for (const TCHAR* Prefix : { TEXT("SM_"), TEXT("SK_"), TEXT("BP_"), TEXT("MESH_") })
	{
		if (Value.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			Value.RightChopInline(FCString::Strlen(Prefix));
			break;
		}
	}

	while (!Value.IsEmpty() && FChar::IsDigit(Value[Value.Len() - 1]))
	{
		Value.LeftChopInline(1);
	}
	Value.RemoveFromEnd(TEXT("_"));
	Value.ReplaceInline(TEXT("_"), TEXT(" "));
	Value.ReplaceInline(TEXT("-"), TEXT(" "));

	FString Spaced;
	Spaced.Reserve(Value.Len() + 8);
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR C = Value[Index];
		if (Index > 0 && FChar::IsUpper(C) && FChar::IsLower(Value[Index - 1]))
		{
			Spaced.AppendChar(TEXT(' '));
		}
		Spaced.AppendChar(C);
	}

	TArray<FString> Words;
	Spaced.ParseIntoArrayWS(Words);
	for (FString& Word : Words)
	{
		Word.ToLowerInline();
		if (!Word.IsEmpty())
		{
			Word[0] = FChar::ToUpper(Word[0]);
		}
	}
	return FString::Join(Words, TEXT(" ")).Left(96);
}
}

void FSceneAutoTaggerDiscovery::Discover(
	UWorld* World,
	const FSceneAutoTaggerRunOptions& Options,
	const UConvaiSceneAutoTaggerSettings& Settings,
	TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& OutCandidates,
	FSceneAutoTaggerDiscoveryStats& OutStats)
{
	OutCandidates.Reset();
	OutStats = FSceneAutoTaggerDiscoveryStats();
	if (!World)
	{
		return;
	}

	TSet<TWeakObjectPtr<AActor>> ExplicitScope;
	for (const TWeakObjectPtr<AActor>& Actor : Options.ScopedActors)
	{
		if (Actor.IsValid())
		{
			ExplicitScope.Add(Actor);
		}
	}
	const bool bHasExplicitScope = ExplicitScope.Num() > 0;
	const bool bForceAnalyzeExplicitScope = Options.bForceAnalyzeScopedActors
		&& bHasExplicitScope;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (bHasExplicitScope && !ExplicitScope.Contains(Actor))
		{
			continue;
		}
		++OutStats.ScannedActors;

		if (!IsValid(Actor)
			|| Actor->IsTemplate()
			|| Actor->HasAnyFlags(RF_Transient | RF_ClassDefaultObject | RF_ArchetypeObject)
			|| Actor->IsActorBeingDestroyed()
			|| Actor->IsEditorOnly()
#if WITH_EDITOR
			|| Actor->IsHiddenEd()
#endif
			|| IsInfrastructureActor(Actor)
			|| (Settings.bExcludeCharactersAndPawns && Actor->IsA<APawn>()))
		{
			++OutStats.ExcludedInfrastructure;
			continue;
		}

		const bool bHasConvaiComponent = Actor->FindComponentByClass<UConvaiObjectComponent>() != nullptr;
		// The run-scoped option is authoritative. The project setting only seeds the
		// setup checkbox; once Explore is pressed, the visible choice must exactly
		// match discovery behavior.
		if (bHasConvaiComponent && !Options.bIncludeAlreadyTaggedActors)
		{
			++OutStats.ExcludedAlreadyTagged;
			continue;
		}

		TInlineComponentArray<UPrimitiveComponent*> ActorPrimitives;
		Actor->GetComponents(ActorPrimitives);
		TArray<UPrimitiveComponent*> RenderablePrimitives;
		for (UPrimitiveComponent* Primitive : ActorPrimitives)
		{
			if (IsRenderablePrimitive(Primitive))
			{
				RenderablePrimitives.Add(Primitive);
			}
		}
		if (RenderablePrimitives.IsEmpty())
		{
			++OutStats.ExcludedNoRenderableGeometry;
			continue;
		}

		FBox Bounds(ForceInit);
		TSet<FString> AssetNames;
		TSet<const UMaterialInterface*> Materials;
		int32 BasicShapeCount = 0;
		int32 CustomizedBasicShapeCount = 0;
		int32 AuthoredMaterialOverrideCount = 0;
		for (UPrimitiveComponent* Primitive : RenderablePrimitives)
		{
			Bounds += Primitive->Bounds.GetBox();
			const FString AssetName = ComponentAssetName(Primitive);
			if (!AssetName.IsEmpty())
			{
				AssetNames.Add(AssetName);
			}
			const bool bEngineBasicShape = IsEngineBasicShape(Primitive);
			const bool bHasAuthoredMaterialOverride = HasAuthoredMaterialOverride(Primitive);
			BasicShapeCount += bEngineBasicShape ? 1 : 0;
			CustomizedBasicShapeCount += bEngineBasicShape && bHasAuthoredMaterialOverride ? 1 : 0;
			AuthoredMaterialOverrideCount += bHasAuthoredMaterialOverride ? 1 : 0;
			for (int32 MaterialIndex = 0; MaterialIndex < Primitive->GetNumMaterials(); ++MaterialIndex)
			{
				if (const UMaterialInterface* Material = Primitive->GetMaterial(MaterialIndex))
				{
					Materials.Add(Material);
				}
			}
		}
		if (!Bounds.IsValid)
		{
			++OutStats.ExcludedNoRenderableGeometry;
			continue;
		}

		const FString ActorLabel = Actor->GetActorNameOrLabel();
		const FString AssetSummary = FString::Join(AssetNames.Array(), TEXT(", "));
		const FString SearchText = (ActorLabel + TEXT(" ") + AssetSummary + TEXT(" ") + Actor->GetClass()->GetName()).ToLower();
		FString ImportantMatch;
		FString ExcludedMatch;
		const bool bImportant = ContainsAnyFragment(SearchText, Settings.ImportantNameFragments, &ImportantMatch);
		const bool bStructural = ContainsAnyFragment(SearchText, Settings.ExcludedNameFragments, &ExcludedMatch);
		const bool bOnlyBasicShapes = BasicShapeCount == RenderablePrimitives.Num();
		const bool bOnlyPlainBasicShapes = bOnlyBasicShapes && CustomizedBasicShapeCount == 0;
		const FVector Size = Bounds.GetSize();
		const bool bPanelLike = RenderablePrimitives.Num() == 1
			? IsPanelLike(RenderablePrimitives[0])
			: IsPanelLike(Bounds);
		const bool bMaterialAuthoredPanel = bOnlyBasicShapes
			&& CustomizedBasicShapeCount > 0
			&& bPanelLike;
		const bool bLowGeometryInformation = RenderablePrimitives.Num() == 1
			&& HasLowGeometryInformation(RenderablePrimitives[0]);
		const bool bHasAuthoredSurface = bLowGeometryInformation
			&& (AuthoredMaterialOverrideCount > 0
				|| Materials.Num() > 1
				|| HasAuthoredTexture(Materials));
		const bool bAuthoredInformationPanel = bPanelLike && bHasAuthoredSurface;

		if (Settings.bExcludeEngineBasicShapes
			&& bOnlyBasicShapes
			&& !bMaterialAuthoredPanel
			&& !bImportant)
		{
			++OutStats.ExcludedBasicShapes;
			continue;
		}

		const float LongestSide = Size.GetMax();
		float Score = 0.42f;
		TArray<FString> Reasons;
		if (LongestSide >= Settings.MinimumLongestSideCm && LongestSide <= 1200.0f)
		{
			Score += 0.12f;
			Reasons.Add(TEXT("object-scale geometry"));
		}
		else if (LongestSide > 1200.0f)
		{
			Score += 0.03f;
			Reasons.Add(TEXT("large geometry"));
		}
		else
		{
			Score -= 0.28f;
			Reasons.Add(TEXT("very small"));
		}
		if (RenderablePrimitives.Num() > 1)
		{
			Score += FMath::Min(0.12f, 0.03f * static_cast<float>(RenderablePrimitives.Num() - 1));
			Reasons.Add(TEXT("multi-part"));
		}
		if (Materials.Num() > 1)
		{
			Score += FMath::Min(0.10f, 0.025f * static_cast<float>(Materials.Num() - 1));
			Reasons.Add(TEXT("material detail"));
		}
		if (bImportant)
		{
			Score += 0.36f;
			Reasons.Add(FString::Printf(TEXT("tour keyword '%s'"), *ImportantMatch));
		}
		if (bStructural && !bImportant)
		{
			Score -= 0.48f;
			Reasons.Add(FString::Printf(TEXT("structural keyword '%s'"), *ExcludedMatch));
		}
		if (!IsGenericActorLabel(ActorLabel.ToLower()))
		{
			Score += 0.04f;
			Reasons.Add(TEXT("authored label"));
		}
		if (bOnlyPlainBasicShapes)
		{
			Score -= 0.35f;
			Reasons.Add(TEXT("plain engine primitive"));
		}
		else if (bMaterialAuthoredPanel)
		{
			Reasons.Add(TEXT("authored material on panel-like engine primitive"));
		}
		if (bLowGeometryInformation && !bImportant && !bAuthoredInformationPanel)
		{
			// A non-generic label and object-scale bounds previously gave any
			// custom cuboid 58% relevance. Cap low-information geometry below the
			// normal threshold; a genuinely authored surface remains recoverable
			// through a deliberately lowered threshold, while an undecorated block
			// is treated as near-zero-information workload.
			if (bHasAuthoredSurface)
			{
				Score = FMath::Min(Score - 0.18f, 0.44f);
				Reasons.Add(TEXT("low-information geometry with authored surface"));
			}
			else
			{
				Score = FMath::Min(Score - 0.46f, 0.12f);
				Reasons.Add(TEXT("low-information geometry"));
			}
		}
		Score = FMath::Clamp(Score, 0.0f, 1.0f);

		// A forced explicit selection is deliberate user intent. Preserve the measured score
		// for review and sorting, but do not let the Current Level workload filter
		// turn Explore selected actors into a silent no-op. Explicit validity,
		// renderability, already-tagged, pawn, and stock-basic-shape rules above
		// still apply; only the soft score floor yields to the explicit request.
		const bool bExplicitRedoOfExistingObject =
			Options.bIncludeAlreadyTaggedActors && bHasConvaiComponent;
		if (!bForceAnalyzeExplicitScope && !bExplicitRedoOfExistingObject
			&& Score < Options.SignificanceThreshold)
		{
			++OutStats.BelowThreshold;
			continue;
		}

		TSharedPtr<FSceneAutoTaggerCandidate> Candidate = MakeShared<FSceneAutoTaggerCandidate>();
		Candidate->Actor = Actor;
		Candidate->WorldBounds = Bounds;
		Candidate->ActorLabel = ActorLabel;
		Candidate->ActorPath = Actor->GetPathName();
		Candidate->AssetSummary = AssetSummary;
		Candidate->SignificanceScore = Score;
		Candidate->SignificanceReason = FString::Join(Reasons, TEXT("; "));
		Candidate->bHadConvaiComponent = bHasConvaiComponent;
		for (UPrimitiveComponent* Primitive : RenderablePrimitives)
		{
			Candidate->PrimitiveComponents.Add(Primitive);
		}
		OutCandidates.Add(MoveTemp(Candidate));
	}

	OutCandidates.Sort([bPrioritizeExistingObjects = Options.bIncludeAlreadyTaggedActors](
		const TSharedPtr<FSceneAutoTaggerCandidate>& A,
		const TSharedPtr<FSceneAutoTaggerCandidate>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid();
		}
		if (bPrioritizeExistingObjects && A->bHadConvaiComponent != B->bHadConvaiComponent)
		{
			return A->bHadConvaiComponent;
		}
		if (!FMath::IsNearlyEqual(A->SignificanceScore, B->SignificanceScore))
		{
			return A->SignificanceScore > B->SignificanceScore;
		}
		return A->ActorLabel < B->ActorLabel;
	});
	if (OutCandidates.Num() > Settings.MaxCandidates)
	{
		OutCandidates.SetNum(Settings.MaxCandidates);
	}
	OutStats.IncludedActors = OutCandidates.Num();
}

bool FSceneAutoTaggerDiscovery::RefreshCandidateGeometry(
	AActor& Actor,
	FSceneAutoTaggerCandidate& Candidate)
{
	if (!IsValid(&Actor)
		|| Actor.IsTemplate()
		|| Actor.IsActorBeingDestroyed())
	{
		return false;
	}

	TInlineComponentArray<UPrimitiveComponent*> ActorPrimitives;
	Actor.GetComponents(ActorPrimitives);
	FBox Bounds(ForceInit);
	TArray<TWeakObjectPtr<UPrimitiveComponent>> RenderablePrimitives;
	for (UPrimitiveComponent* Primitive : ActorPrimitives)
	{
		if (!IsValid(Primitive) || !Primitive->IsRegistered()
			|| !IsRenderablePrimitive(Primitive))
		{
			continue;
		}
		Bounds += Primitive->Bounds.GetBox();
		RenderablePrimitives.Add(Primitive);
	}
	if (!Bounds.IsValid || RenderablePrimitives.IsEmpty())
	{
		return false;
	}

	Candidate.Actor = &Actor;
	Candidate.ActorLabel = Actor.GetActorNameOrLabel();
	Candidate.ActorPath = Actor.GetPathName();
	Candidate.WorldBounds = Bounds;
	Candidate.PrimitiveComponents = MoveTemp(RenderablePrimitives);
	return true;
}

FString FSceneAutoTaggerDiscovery::MakeReadableObjectName(const FSceneAutoTaggerCandidate& Candidate)
{
	FString Source = Candidate.ActorLabel;
	if (IsGenericActorLabel(Source.ToLower()) && !Candidate.AssetSummary.IsEmpty())
	{
		TArray<FString> Assets;
		Candidate.AssetSummary.ParseIntoArray(Assets, TEXT(","), true);
		if (!Assets.IsEmpty())
		{
			Source = Assets[0];
		}
	}
	FString Result = HumanizeIdentifier(Source);
	return Result.IsEmpty() ? TEXT("Scene Object") : Result;
}

void FSceneAutoTaggerDiscovery::ApplyHeuristicFallback(FSceneAutoTaggerCandidate& Candidate, const FString& Reason)
{
	Candidate.SuggestedName = MakeReadableObjectName(Candidate);
	const FVector Size = Candidate.WorldBounds.IsValid ? Candidate.WorldBounds.GetSize() : FVector::ZeroVector;
	Candidate.Description = FString::Printf(
		TEXT("A scene object resembling %s, measuring approximately %.0f x %.0f x %.0f cm."),
		*Candidate.SuggestedName.ToLower(), Size.X, Size.Y, Size.Z);
	Candidate.Confidence = 0.25f;
	Candidate.Source = ESceneAutoTaggerSource::HeuristicFallback;
	Candidate.bTagComplete = true;
	Candidate.Error = Reason;
}

bool FSceneAutoTaggerDiscovery::ContainsAnyFragment(const FString& LowerHaystack, const TArray<FString>& Fragments, FString* OutMatch)
{
	for (FString Fragment : Fragments)
	{
		Fragment.TrimStartAndEndInline();
		Fragment.ToLowerInline();
		if (!Fragment.IsEmpty() && LowerHaystack.Contains(Fragment))
		{
			if (OutMatch)
			{
				*OutMatch = Fragment;
			}
			return true;
		}
	}
	return false;
}
