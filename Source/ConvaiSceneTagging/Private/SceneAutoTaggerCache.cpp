
#include "SceneAutoTaggerCache.h"

#include "SceneAutoTaggerViewSelection.h"

#include "Algo/Sort.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Hash/xxhash.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RawIndexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace ConvaiSceneAutoTagger
{
namespace
{
constexpr int32 CacheSchemaVersion = 4;
constexpr int64 MaximumCacheFileBytes = 8ll * 1024ll * 1024ll;
constexpr int32 MaximumCacheEntries = 5000;
constexpr double VertexPositionQuantumCm = 1.0e-4;
constexpr double ComponentOffsetQuantumCm = 0.1;
constexpr double ScaleQuantum = 1.0e-4;
constexpr double RotationQuantum = 1.0e-5;
constexpr double ExtentQuantumCm = 1.0e-3;
constexpr const TCHAR* CacheFileName = TEXT("SceneTagCache.json");

struct FMeshFingerprint
{
	FString Token;
	bool bUsedFallback = false;
};

int32 QuantizeVertexCoordinate(const double Value)
{
	const double Quantized = FMath::RoundToDouble(Value / VertexPositionQuantumCm);
	return static_cast<int32>(FMath::Clamp(
		Quantized,
		static_cast<double>(MIN_int32),
		static_cast<double>(MAX_int32)));
}

int64 QuantizeForRecord(const double Value, const double Quantum)
{
	if (!FMath::IsFinite(Value) || Quantum <= 0.0)
	{
		return 0;
	}
	const double Quantized = FMath::RoundToDouble(Value / Quantum);
	constexpr double SafeMin = -9007199254740991.0; // Largest exactly represented integral range in a double.
	constexpr double SafeMax = 9007199254740991.0;
	return static_cast<int64>(FMath::Clamp(Quantized, SafeMin, SafeMax));
}

FString QuantizedVectorToken(const FVector& Value, const double Quantum)
{
	return FString::Printf(
		TEXT("%lld,%lld,%lld"),
		static_cast<long long>(QuantizeForRecord(Value.X, Quantum)),
		static_cast<long long>(QuantizeForRecord(Value.Y, Quantum)),
		static_cast<long long>(QuantizeForRecord(Value.Z, Quantum)));
}

void AppendLengthPrefixed(FString& Destination, const FString& Value)
{
	Destination += FString::FromInt(Value.Len());
	Destination += TEXT(":");
	Destination += Value;
	Destination += TEXT(";");
}

FString Sha1Hex(const FString& CanonicalText)
{
	const FTCHARToUTF8 Utf8(*CanonicalText);
	return FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length())).ToString();
}

FString StableAssetToken(const UObject* Asset)
{
	if (!Asset)
	{
		return TEXT("none");
	}

	FString Token;
	AppendLengthPrefixed(Token, Asset->GetPathName());
	#if WITH_EDITORONLY_DATA
	if (const UPackage* Package = Asset->GetPackage())
	{
		AppendLengthPrefixed(Token, Package->GetPersistentGuid().ToString(EGuidFormats::Digits));
	}
	#endif
	return Token;
}

FMeshFingerprint FingerprintStaticMesh(const UStaticMesh* StaticMesh)
{
	FMeshFingerprint Result;
	if (!StaticMesh)
	{
		Result.Token = TEXT("static:none");
		Result.bUsedFallback = true;
		return Result;
	}

	const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
	if (RenderData && !RenderData->LODResources.IsEmpty())
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		const FRawStaticIndexBuffer& IndexBuffer = LOD.IndexBuffer;
		if (Positions.GetAllowCPUAccess() && IndexBuffer.GetAllowCPUAccess())
		{
			const int32 VertexCount = Positions.GetNumVertices();
			const FIndexArrayView Indices = IndexBuffer.GetArrayView();
			const int32 TriangleCount = Indices.Num() / 3;
			if (VertexCount > 0 && TriangleCount > 0)
			{
				TArray<int32> HashWords;
				HashWords.Reserve(VertexCount * 3 + Indices.Num() + 2);
				HashWords.Add(VertexCount);
				HashWords.Add(TriangleCount);

				FBox LocalBounds(ForceInit);
				for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
				{
					const FVector Position(Positions.VertexPosition(VertexIndex));
					LocalBounds += Position;
					HashWords.Add(QuantizeVertexCoordinate(Position.X));
					HashWords.Add(QuantizeVertexCoordinate(Position.Y));
					HashWords.Add(QuantizeVertexCoordinate(Position.Z));
				}
				for (int32 IndexPosition = 0; IndexPosition < Indices.Num(); ++IndexPosition)
				{
					HashWords.Add(static_cast<int32>(Indices[IndexPosition]));
				}

				const uint64 GeometryHash = FXxHash64::HashBuffer(
					HashWords.GetData(),
					static_cast<uint64>(HashWords.Num()) * sizeof(int32)).Hash;
				Result.Token = FString::Printf(
					TEXT("static:cpu:%016llx:v%d:t%d:e%s"),
					static_cast<unsigned long long>(GeometryHash),
					VertexCount,
					TriangleCount,
					*QuantizedVectorToken(
						LocalBounds.IsValid ? LocalBounds.GetExtent() : FVector::ZeroVector,
						VertexPositionQuantumCm));
				return Result;
			}
		}
	}

	// CPU buffers are often unavailable in cooked/editor preview data. This fallback is
	// intentionally asset-version-aware rather than returning no hash, which keeps cache
	// useful without claiming cross-asset geometry identity.
	Result.bUsedFallback = true;
	const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
	Result.Token = FString::Printf(
		TEXT("static:asset:%s:lighting:%s:v%d:t%d:e%s"),
		*StableAssetToken(StaticMesh),
		*StaticMesh->GetLightingGuid().ToString(EGuidFormats::Digits),
		StaticMesh->GetNumVertices(0),
		StaticMesh->GetNumTriangles(0),
		*QuantizedVectorToken(Bounds.BoxExtent, ExtentQuantumCm));
	return Result;
}

FString MaterialToken(const UMaterialInterface* Material)
{
	if (!Material)
	{
		return TEXT("none");
	}

	// Dynamic instances have actor-local transient names. Their parent is the stable visual
	// identity available without serializing every runtime parameter override.
	if (Material->HasAnyFlags(RF_Transient))
	{
		if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			if (Instance->Parent)
			{
				return StableAssetToken(Instance->Parent);
			}
		}
	}
	return StableAssetToken(Material);
}

FVector PoseInvariantComponentCenter(const UPrimitiveComponent* Component)
{
	if (const AActor* Owner = Component ? Component->GetOwner() : nullptr)
	{
		return Owner->GetActorTransform().InverseTransformPosition(Component->Bounds.Origin);
	}
	return Component ? Component->GetRelativeLocation() : FVector::ZeroVector;
}

FQuat PoseInvariantComponentRotation(const UPrimitiveComponent* Component)
{
	if (!Component)
	{
		return FQuat::Identity;
	}
	FQuat Rotation = Component->GetRelativeRotation().Quaternion();
	if (const AActor* Owner = Component->GetOwner())
	{
		Rotation = Owner->GetActorQuat().Inverse() * Component->GetComponentQuat();
	}
	Rotation.Normalize();
	// q and -q encode the same rotation; canonicalize the sign before hashing.
	if (Rotation.W < 0.0)
	{
		Rotation.X *= -1.0;
		Rotation.Y *= -1.0;
		Rotation.Z *= -1.0;
		Rotation.W *= -1.0;
	}
	return Rotation;
}

FString QuantizedRotationToken(FQuat Rotation)
{
	Rotation.Normalize();
	if (Rotation.W < 0.0)
	{
		Rotation.X *= -1.0;
		Rotation.Y *= -1.0;
		Rotation.Z *= -1.0;
		Rotation.W *= -1.0;
	}
	return FString::Printf(
		TEXT("%lld,%lld,%lld,%lld"),
		static_cast<long long>(QuantizeForRecord(Rotation.X, RotationQuantum)),
		static_cast<long long>(QuantizeForRecord(Rotation.Y, RotationQuantum)),
		static_cast<long long>(QuantizeForRecord(Rotation.Z, RotationQuantum)),
		static_cast<long long>(QuantizeForRecord(Rotation.W, RotationQuantum)));
}

FString BuildInstanceLayoutToken(const UInstancedStaticMeshComponent* InstancedComponent)
{
	if (!InstancedComponent)
	{
		return FString();
	}

	TArray<FString> Instances;
	const int32 InstanceCount = InstancedComponent->GetInstanceCount();
	Instances.Reserve(InstanceCount);
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
	{
		FTransform Transform;
		if (!InstancedComponent->GetInstanceTransform(InstanceIndex, Transform, false))
		{
			continue;
		}
		const FVector Scale = Transform.GetScale3D();
		const int32 MirrorSign = Scale.X * Scale.Y * Scale.Z < 0.0 ? -1 : 1;
		Instances.Add(FString::Printf(
			TEXT("p%s:r%s:s%s:m%d"),
			*QuantizedVectorToken(Transform.GetLocation(), ComponentOffsetQuantumCm),
			*QuantizedRotationToken(Transform.GetRotation()),
			*QuantizedVectorToken(Scale.GetAbs(), ScaleQuantum),
			MirrorSign));
	}
	Instances.Sort();

	FString Result = FString::Printf(TEXT("instances:%d:"), InstanceCount);
	for (const FString& Instance : Instances)
	{
		AppendLengthPrefixed(Result, Instance);
	}
	return Result;
}

FString BuildComponentRecord(const UPrimitiveComponent* Component, bool& bOutUsedFallback)
{
	bOutUsedFallback = false;
	FString Record;
	AppendLengthPrefixed(Record, Component->GetClass()->GetPathName());

	if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
	{
		const FMeshFingerprint MeshFingerprint = FingerprintStaticMesh(StaticMeshComponent->GetStaticMesh());
		AppendLengthPrefixed(Record, MeshFingerprint.Token);
		bOutUsedFallback = MeshFingerprint.bUsedFallback;
	}
	else if (const USkinnedMeshComponent* SkinnedComponent = Cast<USkinnedMeshComponent>(Component))
	{
		AppendLengthPrefixed(Record, TEXT("skinned-asset-fallback"));
		AppendLengthPrefixed(Record, StableAssetToken(SkinnedComponent->GetSkinnedAsset()));
		bOutUsedFallback = true;
	}
	else
	{
		const FBoxSphereBounds LocalBounds = Component->CalcBounds(FTransform::Identity);
		FVector Extent = LocalBounds.BoxExtent;
		TArray<double, TInlineAllocator<3>> SortedExtents { Extent.X, Extent.Y, Extent.Z };
		SortedExtents.Sort();
		Extent = FVector(SortedExtents[0], SortedExtents[1], SortedExtents[2]);
		AppendLengthPrefixed(Record, TEXT("primitive-bounds-fallback"));
		AppendLengthPrefixed(Record, QuantizedVectorToken(Extent, ExtentQuantumCm));
		bOutUsedFallback = true;
	}

	const FVector Scale = Component->GetComponentTransform().GetScale3D();
	const FQuat Rotation = PoseInvariantComponentRotation(Component);
	const int32 MirrorSign = Scale.X * Scale.Y * Scale.Z < 0.0 ? -1 : 1;
	AppendLengthPrefixed(Record, QuantizedVectorToken(PoseInvariantComponentCenter(Component), ComponentOffsetQuantumCm));
	AppendLengthPrefixed(Record, QuantizedRotationToken(Rotation));
	AppendLengthPrefixed(Record, QuantizedVectorToken(Scale.GetAbs(), ScaleQuantum));
	AppendLengthPrefixed(Record, FString::FromInt(MirrorSign));

	FString Materials;
	const int32 MaterialCount = Component->GetNumMaterials();
	Materials += FString::Printf(TEXT("count:%d:"), MaterialCount);
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		AppendLengthPrefixed(Materials, MaterialToken(Component->GetMaterial(MaterialIndex)));
	}
	AppendLengthPrefixed(Record, Materials);

	if (const UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(Component))
	{
		AppendLengthPrefixed(Record, BuildInstanceLayoutToken(InstancedComponent));
	}
	return Record;
}

bool TryGetRequiredString(const FJsonObject& JsonObject, const TCHAR* FieldName, FString& OutValue)
{
	return JsonObject.TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty();
}

bool IsValidFingerprint(const FString& Fingerprint)
{
	if (Fingerprint.Len() != 40)
	{
		return false;
	}
	for (const TCHAR Character : Fingerprint)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}
	return true;
}

void SanitizeCachedTag(FSceneAutoTaggerCachedTag& Tag)
{
	auto SanitizeText = [](FString& Value, int32 MaximumLength)
	{
		FString Sanitized;
		Sanitized.Reserve(FMath::Min(Value.Len(), MaximumLength));
		for (const TCHAR Character : Value)
		{
			if ((Character >= 0x20 || Character == TEXT('\n') || Character == TEXT('\t'))
				&& !(Character >= 0x202A && Character <= 0x202E)
				&& !(Character >= 0x2066 && Character <= 0x2069))
			{
				Sanitized.AppendChar(Character);
				if (Sanitized.Len() >= MaximumLength)
				{
					break;
				}
			}
		}
		Sanitized.TrimStartAndEndInline();
		Value = MoveTemp(Sanitized);
	};

	SanitizeText(Tag.Name, 96);
	SanitizeText(Tag.Description, 320);
	Tag.Confidence = FMath::Clamp(Tag.Confidence, 0.0f, 1.0f);

}
}

const TCHAR* GeometryFingerprintVersion()
{
	return TEXT("scene-auto-tagger-geom-v2-q1e-4-offset-q0.1-rotation-q1e-5-scale-q1e-4-materials-asset-fallback");
}

FString ComputeObjectFingerprint(
	const TArray<UPrimitiveComponent*>& Components,
	FString* OutWarning)
{
	// CPU geometry path and order-independent member aggregation are adapted from
	// ConvaiAssemblyStudio's ConvaiGeometryHash.cpp::HashStaticMeshGeometry/ObjectHashHex.
	if (OutWarning)
	{
		OutWarning->Reset();
	}

	TArray<FString> ComponentRecords;
	ComponentRecords.Reserve(Components.Num());
	bool bUsedFallback = false;
	for (const UPrimitiveComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		bool bComponentUsedFallback = false;
		ComponentRecords.Add(BuildComponentRecord(Component, bComponentUsedFallback));
		bUsedFallback |= bComponentUsedFallback;
	}
	if (ComponentRecords.IsEmpty())
	{
		if (OutWarning)
		{
			*OutWarning = TEXT("fingerprint unavailable: object has no valid primitive components");
		}
		return FString();
	}

	ComponentRecords.Sort();
	FString Canonical;
	AppendLengthPrefixed(Canonical, GeometryFingerprintVersion());
	AppendLengthPrefixed(Canonical, FString::FromInt(ComponentRecords.Num()));
	for (const FString& ComponentRecord : ComponentRecords)
	{
		AppendLengthPrefixed(Canonical, ComponentRecord);
	}

	if (bUsedFallback && OutWarning)
	{
		*OutWarning = TEXT("Fingerprint used stable asset/bounds fallback because exact CPU geometry was unavailable for one or more primitives.");
	}
	return Sha1Hex(Canonical);
}

FSceneAutoTaggerCacheIdentity::FSceneAutoTaggerCacheIdentity()
	: GeometryVersion(GeometryFingerprintVersion())
	, NativeCorePolicyId(ConvaiSceneAutoTagger::NativeCorePolicyId())
	, UnrealCapturePolicyId(ConvaiSceneAutoTagger::CapturePolicyVersion())
{
}

FSceneAutoTaggerCacheIdentity::FSceneAutoTaggerCacheIdentity(
	FString InModelId,
	FString InPromptVersion,
	FString InNativeCorePolicyId,
	FString InUnrealCapturePolicyId,
	FString InVisionCharacterId,
	FString InSceneDescription,
	FString InDescriptionFocus)
	: ModelId(MoveTemp(InModelId))
	, PromptVersion(MoveTemp(InPromptVersion))
	, GeometryVersion(GeometryFingerprintVersion())
	, NativeCorePolicyId(MoveTemp(InNativeCorePolicyId))
	, UnrealCapturePolicyId(MoveTemp(InUnrealCapturePolicyId))
	, VisionCharacterId(MoveTemp(InVisionCharacterId))
	, SceneDescription(MoveTemp(InSceneDescription))
	, DescriptionFocus(MoveTemp(InDescriptionFocus))
{
	if (NativeCorePolicyId.IsEmpty())
	{
		NativeCorePolicyId = ConvaiSceneAutoTagger::NativeCorePolicyId();
	}
	if (UnrealCapturePolicyId.IsEmpty())
	{
		UnrealCapturePolicyId = ConvaiSceneAutoTagger::CapturePolicyVersion();
	}
}

bool FSceneAutoTaggerCacheIdentity::operator==(const FSceneAutoTaggerCacheIdentity& Other) const
{
	return ModelId == Other.ModelId
		&& PromptVersion == Other.PromptVersion
		&& GeometryVersion == Other.GeometryVersion
		&& NativeCorePolicyId == Other.NativeCorePolicyId
		&& UnrealCapturePolicyId == Other.UnrealCapturePolicyId
		&& VisionCharacterId == Other.VisionCharacterId
		&& SceneDescription == Other.SceneDescription
		&& DescriptionFocus == Other.DescriptionFocus;
}

FSceneAutoTaggerCache::FSceneAutoTaggerCache(FSceneAutoTaggerCacheIdentity InIdentity)
	: Identity(MoveTemp(InIdentity))
{
	if (Identity.GeometryVersion.IsEmpty())
	{
		Identity.GeometryVersion = GeometryFingerprintVersion();
	}
	if (Identity.NativeCorePolicyId.IsEmpty())
	{
		Identity.NativeCorePolicyId = ConvaiSceneAutoTagger::NativeCorePolicyId();
	}
	if (Identity.UnrealCapturePolicyId.IsEmpty())
	{
		Identity.UnrealCapturePolicyId = ConvaiSceneAutoTagger::CapturePolicyVersion();
	}
}

bool FSceneAutoTaggerCache::Load(FString& OutError, bool* bOutInvalidated)
{
	OutError.Reset();
	Entries.Reset();
	if (bOutInvalidated)
	{
		*bOutInvalidated = false;
	}

	const FString CachePath = GetDefaultCacheFilePath();
	const int64 FileSize = IFileManager::Get().FileSize(*CachePath);
	if (FileSize == INDEX_NONE)
	{
		return true;
	}
	if (FileSize < 0 || FileSize > MaximumCacheFileBytes)
	{
		OutError = FString::Printf(TEXT("Cache load failed: invalid or oversized file '%s'"), *CachePath);
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *CachePath))
	{
		OutError = FString::Printf(TEXT("Cache load failed: could not read '%s'"), *CachePath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("Cache load failed: invalid JSON in '%s'"), *CachePath);
		return false;
	}

	int32 SchemaVersion = 0;
	FString ModelId;
	FString PromptVersion;
	FString GeometryVersion;
	FString NativeCorePolicyId;
	FString UnrealCapturePolicyId;
	FString VisionCharacterId;
	FString SceneDescription;
	FString DescriptionFocus;
	const bool bHasIdentity = Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
		&& Root->TryGetStringField(TEXT("modelId"), ModelId)
		&& Root->TryGetStringField(TEXT("promptVersion"), PromptVersion)
		&& Root->TryGetStringField(TEXT("geometryVersion"), GeometryVersion)
		&& Root->TryGetStringField(TEXT("nativeCorePolicyId"), NativeCorePolicyId)
		&& Root->TryGetStringField(TEXT("unrealCapturePolicyId"), UnrealCapturePolicyId)
		&& Root->TryGetStringField(TEXT("visionCharacterId"), VisionCharacterId)
		&& Root->TryGetStringField(TEXT("sceneDescription"), SceneDescription)
		&& Root->TryGetStringField(TEXT("descriptionFocus"), DescriptionFocus);
	FSceneAutoTaggerCacheIdentity StoredIdentity(
		MoveTemp(ModelId),
		MoveTemp(PromptVersion),
		MoveTemp(NativeCorePolicyId),
		MoveTemp(UnrealCapturePolicyId),
		MoveTemp(VisionCharacterId),
		MoveTemp(SceneDescription),
		MoveTemp(DescriptionFocus));
	StoredIdentity.GeometryVersion = MoveTemp(GeometryVersion);
	if (!bHasIdentity
		|| SchemaVersion != CacheSchemaVersion
		|| StoredIdentity != Identity)
	{
		if (bOutInvalidated)
		{
			*bOutInvalidated = true;
		}
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonEntries = nullptr;
	if (!Root->TryGetArrayField(TEXT("entries"), JsonEntries) || !JsonEntries)
	{
		OutError = TEXT("Cache load failed: missing entries array");
		return false;
	}
	if (JsonEntries->Num() > MaximumCacheEntries)
	{
		OutError = TEXT("Cache load failed: entry count exceeds the safety limit");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& JsonValue : *JsonEntries)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> JsonEntry = JsonValue->AsObject();
		if (!JsonEntry.IsValid())
		{
			continue;
		}

		FString Fingerprint;
		FSceneAutoTaggerCachedTag Tag;
		if (!TryGetRequiredString(*JsonEntry, TEXT("fingerprint"), Fingerprint)
			|| !JsonEntry->TryGetStringField(TEXT("name"), Tag.Name)
			|| !JsonEntry->TryGetStringField(TEXT("description"), Tag.Description))
		{
			continue;
		}
		if (!IsValidFingerprint(Fingerprint))
		{
			continue;
		}
		JsonEntry->TryGetNumberField(TEXT("confidence"), Tag.Confidence);
		SanitizeCachedTag(Tag);
		if (Tag.Name.IsEmpty() || Tag.Description.IsEmpty())
		{
			continue;
		}
		Entries.Add(MoveTemp(Fingerprint), MoveTemp(Tag));
	}

	return true;
}

bool FSceneAutoTaggerCache::Save(FString& OutError) const
{
	OutError.Reset();
	const FString CacheDirectory = GetDefaultCacheDirectory();
	if (!IFileManager::Get().MakeDirectory(*CacheDirectory, true)
		&& !IFileManager::Get().DirectoryExists(*CacheDirectory))
	{
		OutError = FString::Printf(TEXT("Cache save failed: could not create '%s'"), *CacheDirectory);
		return false;
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), CacheSchemaVersion);
	Root->SetStringField(TEXT("modelId"), Identity.ModelId);
	Root->SetStringField(TEXT("promptVersion"), Identity.PromptVersion);
	Root->SetStringField(TEXT("geometryVersion"), Identity.GeometryVersion);
	Root->SetStringField(TEXT("nativeCorePolicyId"), Identity.NativeCorePolicyId);
	Root->SetStringField(TEXT("unrealCapturePolicyId"), Identity.UnrealCapturePolicyId);
	Root->SetStringField(TEXT("visionCharacterId"), Identity.VisionCharacterId);
	Root->SetStringField(TEXT("sceneDescription"), Identity.SceneDescription);
	Root->SetStringField(TEXT("descriptionFocus"), Identity.DescriptionFocus);
	Root->SetStringField(TEXT("updatedUtc"), FDateTime::UtcNow().ToIso8601());

	TArray<FString> Fingerprints;
	Entries.GetKeys(Fingerprints);
	Fingerprints.Sort();
	TArray<TSharedPtr<FJsonValue>> JsonEntries;
	JsonEntries.Reserve(Fingerprints.Num());
	for (const FString& Fingerprint : Fingerprints)
	{
		const FSceneAutoTaggerCachedTag* Tag = Entries.Find(Fingerprint);
		if (!Tag || Fingerprint.IsEmpty())
		{
			continue;
		}

		const TSharedRef<FJsonObject> JsonEntry = MakeShared<FJsonObject>();
		JsonEntry->SetStringField(TEXT("fingerprint"), Fingerprint);
		JsonEntry->SetStringField(TEXT("name"), Tag->Name);
		JsonEntry->SetStringField(TEXT("description"), Tag->Description);
		JsonEntry->SetNumberField(TEXT("confidence"), FMath::Clamp(Tag->Confidence, 0.0f, 1.0f));

		JsonEntries.Add(MakeShared<FJsonValueObject>(JsonEntry));
	}
	Root->SetArrayField(TEXT("entries"), MoveTemp(JsonEntries));

	FString JsonText;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer
		= TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Cache save failed: could not serialize JSON");
		return false;
	}

	const FString CachePath = GetDefaultCacheFilePath();
	const FString TempPath = FString::Printf(
		TEXT("%s.tmp-%s"),
		*CachePath,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!FFileHelper::SaveStringToFile(
		JsonText,
		*TempPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		OutError = FString::Printf(TEXT("Cache save failed: could not write temporary file '%s'"), *TempPath);
		return false;
	}

	// Same-directory replace keeps the final cache path atomic on supported platform files.
	// This mirrors ConvaiAssemblyScene.cpp::SaveGeometryTagCache.
	if (!IFileManager::Get().Move(*CachePath, *TempPath, true, true))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		OutError = FString::Printf(TEXT("Cache save failed: could not replace '%s'"), *CachePath);
		return false;
	}
	return true;
}

bool FSceneAutoTaggerCache::Clear(FString& OutError)
{
	OutError.Reset();
	Entries.Reset();
	const FString CachePath = GetDefaultCacheFilePath();
	if (IFileManager::Get().FileExists(*CachePath)
		&& !IFileManager::Get().Delete(*CachePath, false, true, true))
	{
		OutError = FString::Printf(TEXT("Cache clear failed: could not delete '%s'"), *CachePath);
		return false;
	}
	return true;
}

const FSceneAutoTaggerCachedTag* FSceneAutoTaggerCache::Find(const FString& Fingerprint) const
{
	return Entries.Find(Fingerprint);
}

void FSceneAutoTaggerCache::Store(
	const FString& Fingerprint,
	const FSceneAutoTaggerCachedTag& Tag)
{
	if (!IsValidFingerprint(Fingerprint)
		|| (!Entries.Contains(Fingerprint) && Entries.Num() >= MaximumCacheEntries))
	{
		return;
	}
	FSceneAutoTaggerCachedTag SanitizedTag = Tag;
	SanitizeCachedTag(SanitizedTag);
	if (!SanitizedTag.Name.IsEmpty() && !SanitizedTag.Description.IsEmpty())
	{
		Entries.Add(Fingerprint, MoveTemp(SanitizedTag));
	}
}

bool FSceneAutoTaggerCache::Remove(const FString& Fingerprint)
{
	return Entries.Remove(Fingerprint) > 0;
}

void FSceneAutoTaggerCache::Reset()
{
	Entries.Reset();
}

FString FSceneAutoTaggerCache::GetDefaultCacheDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("ConvaiSceneAutoTagger"),
		TEXT("Cache")));
}

FString FSceneAutoTaggerCache::GetDefaultCacheFilePath()
{
	return FPaths::Combine(GetDefaultCacheDirectory(), CacheFileName);
}
}
