// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Hosting and runtime settings for a PS application, ported field for field from the dashboard's
 * ps-application-hosting-state.ts so both clients write the same metadata.
 *
 *   metadata.deployment_config  capacity and sleep
 *   metadata.runtime_config     { "ps-frontend-config": {...}, "ps-ue-config": {...} }
 *
 * Every runtime field may be left empty, which means "use the runtime default" and omits the key.
 */
struct FConvaiProjectHostingSettings
{
	/** The four ps-frontend-config keys the dashboard validates as booleans. */
	static const TArray<FString>& FrontendBooleanKeys();

	// --- deployment_config ---------------------------------------------------------------
	bool bOnDemandMode = false;
	FString TargetFreeStreams;
	FString StreamsPerInstance;
	FString MinInstances;
	FString MaxInstances;
	FString IdleTimeoutMinutes;

	// --- runtime_config / ps-frontend-config --------------------------------------------
	/** Parsed browser overrides. Arbitrary keys are allowed; values are bool, number, or string. */
	TSharedPtr<FJsonObject> FrontendConfig;
	/** Pretty-printed mirror of FrontendConfig; the advanced editor's text and the save source. */
	FString FrontendJson;

	// --- runtime_config / ps-ue-config ---------------------------------------------------
	/** Sent as "<width>x<height>"; both must be set, and both are ignored when MatchViewportRes. */
	FString ResolutionWidth;
	FString ResolutionHeight;
	FString ScreenPercentage;
	FString ExtraArgs;

	/** Reads both metadata keys, applying the same defaults the dashboard shows for a new form. */
	static FConvaiProjectHostingSettings Read(const TSharedPtr<FJsonObject>& Metadata);

	/** Field name -> message. Empty means the settings can be saved. */
	TMap<FString, FString> Validate() const;

	TSharedRef<FJsonObject> SerializeDeploymentConfig() const;
	/** Fails only when FrontendJson is not a valid object; Validate() reports the same problem. */
	bool SerializeRuntimeConfig(TSharedRef<FJsonObject>& OutRuntime, FString& OutError) const;

	/**
	 * One ps-frontend-config boolean, as a tri-state. Unset means the key is absent, which the
	 * dashboard surfaces as "Use runtime default": no URL parameter is emitted and Epic's own
	 * default applies. That is a different thing from an explicit false, which forces the flag off.
	 */
	TOptional<bool> GetFrontendBool(const FString& Key) const;
	/** Convenience for the "=== true" checks; an absent key is not true. */
	bool IsFrontendTrue(const FString& Key) const;
	int32 GetAfkTimeout() const;
	/** Writes the key, or removes it when unset, mirroring setFrontendSetting(key, undefined). */
	void SetFrontendBool(const FString& Key, TOptional<bool> Value);
	void SetAfkTimeout(const FString& Value);
	/** Replaces FrontendConfig from edited JSON text. Invalid text is kept for the user to fix. */
	void SetFrontendJson(const FString& Json);

	/**
	 * VPX compute-shader encoding, expressed as an ExtraArgs token rather than its own field.
	 * ps-ue-config is a closed schema on the server, so a dedicated key would be rejected; the
	 * container's entrypoint suppresses its hardcoded default when ExtraArgs names this flag.
	 */
	bool GetDisableVpxCompute() const;
	void SetDisableVpxCompute(bool bDisable);

	/** Parses browser settings with the dashboard's type rules. */
	static bool ParseFrontendConfig(const FString& Json, TSharedPtr<FJsonObject>& OutConfig, FString& OutError);
};
