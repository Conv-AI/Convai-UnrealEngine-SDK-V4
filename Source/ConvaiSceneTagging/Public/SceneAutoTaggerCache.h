#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;

namespace ConvaiSceneAutoTagger
{
/** Bump this whenever the object-fingerprint inputs or quantization change. */
CONVAISCENETAGGING_API const TCHAR* GeometryFingerprintVersion();

/**
 * Builds a deterministic object fingerprint without world position or rotation.
 *
 * LOD0 vertex/index data is used when CPU buffers are available. Cooked or GPU-only
 * meshes fall back to an editor-safe asset/package signature instead of disabling cache.
 */
CONVAISCENETAGGING_API FString ComputeObjectFingerprint(
	const TArray<UPrimitiveComponent*>& Components,
	FString* OutWarning = nullptr);

struct FSceneAutoTaggerCachedTag
{
	FString Name;
	FString Description;
	float Confidence = 0.0f;
};

struct CONVAISCENETAGGING_API FSceneAutoTaggerCacheIdentity
{
	FString ModelId;
	FString PromptVersion;
	FString GeometryVersion;
	FString NativeCorePolicyId;
	FString UnrealCapturePolicyId;
	FString VisionCharacterId;
	FString SceneDescription;
	FString DescriptionFocus;

	FSceneAutoTaggerCacheIdentity();
	FSceneAutoTaggerCacheIdentity(
		FString InModelId,
		FString InPromptVersion,
		FString InNativeCorePolicyId = FString(),
		FString InUnrealCapturePolicyId = FString(),
		FString InVisionCharacterId = FString(),
		FString InSceneDescription = FString(),
		FString InDescriptionFocus = FString());

	bool operator==(const FSceneAutoTaggerCacheIdentity& Other) const;
	bool operator!=(const FSceneAutoTaggerCacheIdentity& Other) const { return !(*this == Other); }
};

/**
 * Versioned, deterministic JSON cache stored under Saved/ConvaiSceneAutoTagger/Cache.
 * A model, prompt, schema, geometry, native-policy, Unreal-capture-policy,
 * selected-character, or guidance mismatch loads as an empty invalidated cache.
 */
class CONVAISCENETAGGING_API FSceneAutoTaggerCache
{
public:
	explicit FSceneAutoTaggerCache(FSceneAutoTaggerCacheIdentity InIdentity);

	bool Load(FString& OutError, bool* bOutInvalidated = nullptr);
	bool Save(FString& OutError) const;
	bool Clear(FString& OutError);

	const FSceneAutoTaggerCachedTag* Find(const FString& Fingerprint) const;
	void Store(const FString& Fingerprint, const FSceneAutoTaggerCachedTag& Tag);
	bool Remove(const FString& Fingerprint);
	void Reset();

	const TMap<FString, FSceneAutoTaggerCachedTag>& GetEntries() const { return Entries; }
	const FSceneAutoTaggerCacheIdentity& GetIdentity() const { return Identity; }

	static FString GetDefaultCacheDirectory();
	static FString GetDefaultCacheFilePath();

private:
	FSceneAutoTaggerCacheIdentity Identity;
	TMap<FString, FSceneAutoTaggerCachedTag> Entries;
};
}
