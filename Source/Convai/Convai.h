// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Public/ConvaiDefinitions.h"
#include "InputCoreTypes.h"
#include "Convai.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogConvai, Log, All);

UCLASS(config = Engine, defaultconfig)
class CONVAI_API UConvaiSettings : public UObject
{
	GENERATED_BODY()

public:
	UConvaiSettings(const FObjectInitializer &ObjectInitializer)
		: Super(ObjectInitializer)
	{
		API_Key = "";
	}
	/* API Key Issued from the website (Managed automatically by Convai Editor UI - Read Only) */
	UPROPERTY(Config, VisibleAnywhere, Category = "Convai", meta = (DisplayName = "API Key"))
	FString API_Key;

	/* Authentication token used for Convai Connect (Managed automatically by Convai Editor UI - Read Only) */
	UPROPERTY(Config, VisibleAnywhere, AdvancedDisplay, Category = "Convai", meta = (DisplayName = "Auth Token"))
	FString AuthToken;

	// No visibility specifier — persisted but hidden from Project Settings.
	UPROPERTY(Config)
	FString CachedUsername;

	UPROPERTY(Config)
	FString CachedEmail;

	UPROPERTY(Config)
	FDateTime ApiKeyLastValidatedUtc;

	/* Custom Server URL (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString CustomURL;

	/* Custom Beta API URL (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString CustomBetaURL;

	/* Custom Production API URL (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString CustomProdURL;

	/* Test Character ID (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString TestCharacterID;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	bool AllowInsecureConnection;

	/*
	 * Forces the AI to include vision parameters in its initial connection setup,
	 * allowing vision components set after Begin Play to function properly.
	 */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	bool AlwaysAllowVision;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	EC_LipSyncMode LipSyncMode = EC_LipSyncMode::Auto;

	/* Extra Parameters (Used for debugging) */
	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	FString ExtraParams;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Convai")
	TMap<FString, FString> CustomPrams;

	// ── Connection ─────────────────────────────────────────

	/** Longest stretch without player speech or text input that a session may
	 *  survive. The character talking does not count as activity.
	 *
	 *  The server drops idle sessions on its own schedule and offers no way to
	 *  change it, so this controls how long the plugin keeps deferring that
	 *  deadline rather than setting the deadline itself: the real disconnect
	 *  lands on the first server deadline at or after this value. It can
	 *  therefore overshoot by up to one server warning interval, and can never
	 *  land earlier than the server's own minimum — which is why it clamps at
	 *  600 rather than pretending smaller values would be honoured. At the
	 *  default the plugin sends nothing at all and the server behaves exactly as
	 *  it did before this setting existed.
	 *
	 *  See docs/adr/0006-reactive-afk-enforcement.md. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection",
	          meta = (DisplayName = "AFK Time", ClampMin = "600.0", UIMin = "600.0", ForceUnits = "s"))
	float AfkTimeSeconds = 600.0f;

	/** Voice Activity Detection parameters sent at /connect time. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio Settings|VAD",
	          meta = (ShowOnlyInnerProperties, DisplayName = "Voice Activity Detection"))
	FConvaiVADSettings VADSettings;

	UPROPERTY(Config, VisibleAnywhere, Category = "Long Term Memory")
	TArray<FConvaiSpeakerInfo> SpeakerIDs;

	// ── Debug Overlay ──────────────────────────────────────────────────

	/** Chord that toggles the Convai debug overlay (markers, state panel,
	 *  action queue) in PIE / Development builds. Ctrl+Alt+<this key>. The
	 *  chord is ignored while a text field has keyboard focus (Ctrl+Alt is
	 *  AltGr on many layouts); `Convai.DebugOverlay` toggles from console. */
	UPROPERTY(Config, EditAnywhere, Category = "Debug Overlay",
		meta = (DisplayName = "Toggle Key (with Ctrl+Alt)"))
	FKey DebugOverlayKey = EKeys::K;

	/** The overlay never shows in Shipping/Test builds unless this is ON.
	 *  It exposes prompt state, tracked values, and action traffic — leave
	 *  OFF for releases. */
	UPROPERTY(Config, EditAnywhere, Category = "Debug Overlay",
		meta = (DisplayName = "Allow In Shipping Builds"))
	bool bAllowDebugOverlayInShipping = false;

	// ── Spatial Awareness ──────────────────────────────────────────────
	// The feature that quietly tells each chatbot where the objects, other
	// characters, and the player are around it ("the crate is close by, in front
	// of you and on top of the pressure plate"). Per-chatbot preferences (which
	// categories a chatbot receives, and whether it should react) live on the
	// Convai Chatbot Component itself.

	/** Master switch for the whole spatial-awareness system. When OFF, no
	 *  proximity, line-of-sight, or relation facts are generated for any chatbot. */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness")
	bool bEnableSpatialAwareness = true;

	/** When ON, the system also checks whether each chatbot can actually SEE a
	 *  thing before telling it where that thing is — a thing hidden behind a wall
	 *  is reported as out of view and its position withheld. Costs one line trace
	 *  per chatbot/target. OFF by default. */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness",
	          meta = (EditCondition = "bEnableSpatialAwareness"))
	bool bEnableLineOfSight = false;

	/** Distance (cm) under which a target reads as "close by"; below Moderate
	 *  Distance it reads "some distance away"; beyond, "far away". */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness",
	          meta = (EditCondition = "bEnableSpatialAwareness", ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm"))
	float NearbyDistance = 1000.0f;

	/** Distance (cm) under which a target reads as "some distance away"; beyond
	 *  it, "far away". Should be larger than Nearby Distance. */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness",
	          meta = (EditCondition = "bEnableSpatialAwareness", ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm"))
	float ModerateDistance = 4000.0f;

	/** When ON, the system also describes how nearby things relate to EACH OTHER
	 *  ("the gun is on top of the crate"), not just to the chatbot. Relations are
	 *  only generated between entities within Relation Cluster Distance. */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness",
	          meta = (EditCondition = "bEnableSpatialAwareness"))
	bool bEnableRelations = true;

	/** How close (cm) two entities must be before a relation between them is
	 *  described. Keeps "the crate is next to the barrel" from firing for things
	 *  that merely share a room. */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness",
	          meta = (EditCondition = "bEnableSpatialAwareness && bEnableRelations", ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm"))
	float RelationClusterDistance = 600.0f;

	/** When ON, every described thing also carries a clause locating it from the
	 *  nearest player's camera frame, with the frame and subject named explicitly
	 *  ("From Eshmawy's position, the crate is close by, ahead and to the left") so the
	 *  character can give the player directions from the player's point of view
	 *  without doing mental rotation. The clause is never dropped by distance —
	 *  far things degrade to a bare "far away" so an earlier "close by" cannot
	 *  linger as stale truth — and player-clause-only changes republish silently.
	 *  Roughly doubles the spatial text. */
	UPROPERTY(Config, EditAnywhere, Category = "Spatial Awareness",
	          meta = (EditCondition = "bEnableSpatialAwareness", DisplayName = "Describe From Player Perspective"))
	bool bDescribePlayerPerspective = true;

	// ── Objects ────────────────────────────────────────────────────────
	// How Convai Object Components that share the same name are disambiguated.
	// Same-named objects are normally kept distinct by appending a suffix to the
	// duplicates (the first keeps the bare name) so the AI can address each one.
	// Whether a set of same-named objects is instead MERGED into a single logical
	// object is opted into PER-OBJECT on the Convai Object Component (Advanced ▸
	// "Merge With Same-Named Objects"); this setting only controls what the
	// disambiguating suffix looks like.

	/** Style of the suffix appended to duplicate object names so each stays
	 *  addressable by the AI: Numeric ("Crate", "Crate 2", "Crate 3") or
	 *  Alphabetical ("Crate", "Crate A", "Crate B"). The first object to register
	 *  keeps its bare name; later duplicates get the next suffix in registration
	 *  order. Objects that are merged together (via the per-object "Merge With
	 *  Same-Named Objects" option) intentionally share one name and are NOT suffixed. */
	UPROPERTY(Config, EditAnywhere, Category = "Objects",
	          meta = (DisplayName = "Duplicate Name Suffix Style"))
	EConvaiObjectNameSuffixStyle ObjectNameSuffixStyle = EConvaiObjectNameSuffixStyle::Numeric;

	/** Programmatically set API key and save to config (Used by Editor UI) */
	void SetAPIKey(const FString &NewApiKey);

	/** Programmatically set Auth Token and save to config (Used by Editor UI) */
	void SetAuthToken(const FString &NewAuthToken);

	/** Save settings to config file */
	void SaveSettings();
};

class CONVAI_API Convai : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	void StartupModule();
	void ShutdownModule();

	/**
	 * Singleton-like access to this module's interface.  This is just for convenience!
	 * Beware of calling this during the shutdown phase, though.  Your module might have been unloaded already.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed
	 */
	static inline Convai &Get()
	{
		return FModuleManager::LoadModuleChecked<Convai>("Convai");
	}

	/**
	 * Checks to see if this module is loaded and ready.  It is only valid to call Get() if IsAvailable() returns true.
	 *
	 * @return True if the module is loaded and ready to use
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("Convai");
	}

	virtual bool IsGameModule() const override
	{
		return true;
	}

	/** Getter for internal settings object to support runtime configuration changes */
	UConvaiSettings *GetConvaiSettings() const;

private:
	void EnsureThirdPartyLibrariesCopied(const FString& PluginBaseDir);

protected:
	/** Module settings */
	UConvaiSettings *ConvaiSettings;

	// Map to store handles to dynamically loaded DLLs
	TMap<FString, void *> ConvaiDllHandles;
};
