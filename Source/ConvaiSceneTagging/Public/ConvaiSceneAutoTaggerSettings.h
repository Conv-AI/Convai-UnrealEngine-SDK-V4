// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ConvaiSceneAutoTaggerSettings.generated.h"

/** How strictly verified duplicate captures are grouped when applied to the level. */
UENUM(BlueprintType)
enum class EConvaiSceneAutoTaggerDuplicateGroupingMode : uint8
{
	/** Compatibility sentinel for configurations saved before grouping modes existed. */
	Legacy UMETA(Hidden),
	/** Every verified copy remains independently addressable. */
	KeepSeparate UMETA(DisplayName = "Keep Separate"),
	/** Nearby verified copies form one logical object; distant clusters stay separate. */
	NearbyClusters UMETA(DisplayName = "Auto (Nearby Clusters)"),
	/** Every copy in one verified proof group becomes one logical object. */
	MergeAllVerified UMETA(DisplayName = "Group All Verified"),
};

/**
 * User-facing analysis quality. The serialized names are retained for
 * compatibility with settings saved before the Fast/Accurate presets existed.
 */
UENUM()
enum class EConvaiSceneAutoTaggerAnalysisDetail : uint8
{
    /** Faster model policy and compact model input. */
    Standard UMETA(DisplayName = "Fast"),
    /** Higher-detail model input and stronger recognition policy. */
    Detailed UMETA(DisplayName = "Accurate"),
    /** Internal run-option sentinel; never persisted as a user selection. */
    Unspecified UMETA(Hidden),
};

/** Project defaults for the editor-only Convai scene exploration workflow. */
UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig, meta = (DisplayName = "Convai Scene Auto Tagger"))
class CONVAISCENETAGGING_API UConvaiSceneAutoTaggerSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UConvaiSceneAutoTaggerSettings();
	virtual void PostInitProperties() override;

	/** Shipped capture defaults, kept separate from per-user config for deterministic policy tests. */
	static constexpr int32 DefaultCellResolution = 384;
	static constexpr int32 DefaultEvidencePreviewResolution = 768;
	static constexpr int32 MinimumFastEvidencePreviewResolution = 768;
	static constexpr int32 MinimumAccurateEvidencePreviewResolution = 1024;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Convai Scene Auto Tagger"); }

	/** Maximum analysis requests that may be in flight concurrently. */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Analysis", meta = (ClampMin = "1", ClampMax = "6", UIMin = "1", UIMax = "6", DisplayName = "Maximum Concurrent Analyses"))
	int32 MaxConcurrentAnalyses = 4;

    /** One quality preset that controls both model-input detail and agent reasoning policy. */
    UPROPERTY(Config, EditAnywhere, Category = "Capture", meta = (DisplayName = "Analysis Quality"))
    EConvaiSceneAutoTaggerAnalysisDetail AnalysisDetail =
        EConvaiSceneAutoTaggerAnalysisDetail::Standard;

    /** Deprecated serialized override retained only for compatibility with older user settings. */
    UPROPERTY(Config, meta = (DeprecatedProperty,
        DeprecationMessage = "Analysis Quality now owns contact-sheet packing."))
    int32 GridDimension = 4;

    /** Deprecated serialized override retained only for compatibility with older user settings. */
    UPROPERTY(Config, meta = (DeprecatedProperty,
        DeprecationMessage = "Analysis Quality now owns model-input resolution."))
    int32 CellResolution = DefaultCellResolution;

	/** Resolution retained for the exact-evidence preview after the best camera angle is selected. */
	UPROPERTY(Config, EditAnywhere, Category = "Capture", meta = (ClampMin = "256", ClampMax = "1024"))
	int32 EvidencePreviewResolution = DefaultEvidencePreviewResolution;

	/** Effective model-sheet grid after applying the user-facing analysis profile. */
	int32 GetEffectiveGridDimension() const;

	/** Effective per-object model resolution after applying the user-facing analysis profile. */
	int32 GetEffectiveCellResolution() const;

	/** Effective retained-evidence resolution for the selected profile. */
	int32 GetEffectiveEvidencePreviewResolution() const;

	/** True when the stronger, higher-detail Accurate preset is selected. */
	bool IsAccurateAnalysisEnabled() const;

	/** Saves per-angle capture probes, retained analysis grids, and per-request snapshots (sent images, request.json, response.json) under Saved/ConvaiSceneAutoTagger. Credentials are never written. */
	UPROPERTY(Config, EditAnywhere, Category = "Capture")
	bool bSaveDebugCaptures = false;

	/** Local pre-capture relevance minimum for Current Level discovery. Explicitly selected actors bypass this soft workload filter. */
	UPROPERTY(Config, EditAnywhere, Category = "Discovery", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "Minimum Local Relevance"))
	float SignificanceThreshold = 0.52f;

	UPROPERTY(Config, EditAnywhere, Category = "Discovery", meta = (ClampMin = "1", ClampMax = "2000"))
	int32 MaxCandidates = 300;

	/** Actors smaller than this on every axis are normally treated as incidental clutter. */
	UPROPERTY(Config, EditAnywhere, Category = "Discovery", meta = (ClampMin = "0.0", Units = "cm"))
	float MinimumLongestSideCm = 12.0f;

	/** Exclude ordinary stock primitives; thin authored panels used for paintings and signs remain eligible. */
	UPROPERTY(Config, EditAnywhere, Category = "Discovery")
	bool bExcludeEngineBasicShapes = true;

	UPROPERTY(Config, EditAnywhere, Category = "Discovery")
	bool bExcludeCharactersAndPawns = true;

	UPROPERTY(Config, EditAnywhere, Category = "Discovery")
	bool bExcludeAlreadyTaggedActors = true;

	UPROPERTY(Config, EditAnywhere, Category = "Discovery")
	TArray<FString> ExcludedNameFragments;

	UPROPERTY(Config, EditAnywhere, Category = "Discovery")
	TArray<FString> ImportantNameFragments;

	/** Reuse descriptions for unchanged geometry. Off by default so every review row retains the exact image analyzed in this run. */
	UPROPERTY(Config, EditAnywhere, Category = "Cache")
	bool bUseGeometryCache = false;

	/** Grouping policy for strict geometry or retained-image duplicates. */
	UPROPERTY(Config, EditAnywhere, Category = "Convai Application",
		meta = (DisplayName = "Verified Duplicate Grouping"))
	EConvaiSceneAutoTaggerDuplicateGroupingMode DuplicateGroupingMode =
		EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters;

	/** Maximum pairwise horizontal span of a nearby duplicate cluster. */
	UPROPERTY(Config, EditAnywhere, Category = "Convai Application",
		meta = (DisplayName = "Nearby Cluster Horizontal Span",
			EditCondition = "DuplicateGroupingMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters",
			ClampMin = "1.0", UIMin = "100.0", ForceUnits = "cm"))
	float DuplicateClusterMaxHorizontalSpanCm = 1000.0f;

	/** Maximum vertical separation inside a nearby duplicate cluster. */
	UPROPERTY(Config, EditAnywhere, Category = "Convai Application",
		meta = (DisplayName = "Nearby Cluster Vertical Separation",
			EditCondition = "DuplicateGroupingMode == EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters",
			ClampMin = "1.0", UIMin = "100.0", ForceUnits = "cm"))
	float DuplicateClusterMaxVerticalSeparationCm = 300.0f;

	/**
	 * Deprecated compatibility bit. It is consulted only when its key was
	 * explicitly persisted by a pre-grouping-mode installation and the modern
	 * mode remains Legacy.
	 */
	UPROPERTY(Config, meta = (DeprecatedProperty,
		DeprecationMessage = "Use DuplicateGroupingMode."))
	bool bMergeDuplicateGeometry = true;

	/**
	 * Resolve the shipped grouping policy without manufacturing a legacy value.
	 * Explicit modern modes win; an explicitly persisted deprecated key keeps
	 * its old meaning; a new installation defaults to automatic nearby clusters.
	 */
	EConvaiSceneAutoTaggerDuplicateGroupingMode GetEffectiveDuplicateGroupingMode() const;

	UPROPERTY(Config, EditAnywhere, Category = "Convai Application")
	bool bIncludeInSpatialAwareness = true;

	UPROPERTY(Config, EditAnywhere, Category = "Convai Application")
	bool bGazeable = true;

};

/** Per-user, per-map editor state. This never dirties a level or project default config. */
UCLASS(Config = EditorPerProjectUserSettings)
class CONVAISCENETAGGING_API UConvaiSceneAutoTaggerUserState : public UObject
{
	GENERATED_BODY()

public:
	virtual void PostInitProperties() override;
	FString GetSceneContextForMap(const FString& MapPackageName) const;
	void SetSceneContextForMap(const FString& MapPackageName, const FString& SceneContext);
	FString GetDescriptionFocusForMap(const FString& MapPackageName) const;
	void SetDescriptionFocusForMap(const FString& MapPackageName, const FString& DescriptionFocus);
	FString GetLastSelectedVisionCharacterID() const;
	void SetLastSelectedVisionCharacterID(const FString& CharacterID);
	bool HasAcceptedPrivacyConsent(const FString& ConsentKey) const;
	void SetPrivacyConsentAccepted(const FString& ConsentKey, bool bAccepted);

private:
	UPROPERTY(Config)
	TMap<FString, FString> SceneContextsByMap;

	/** Optional emphasis preference kept separate from the scene-setting prior. */
	UPROPERTY(Config)
	TMap<FString, FString> DescriptionFocusByMap;

	/** Last explicit picker choice. Metadata is refreshed from the account catalog. */
	UPROPERTY(Config)
	FString LastSelectedVisionCharacterID;

	/** Versioned disclosure keys accepted by this user for this project. */
	UPROPERTY(Config)
	TSet<FString> AcceptedPrivacyConsentKeys;
};
