
// Copyright Convai. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#define CONVAI_SAT_DYNAMIC_LOAD 1
#include "convai_scene_auto_tagger_core.h"
#undef CONVAI_SAT_DYNAMIC_LOAD

/**
 * Owns the explicitly loaded Scene Auto Tagger native core.
 *
 * The process singleton is initialized by the module. A separate instance can
 * be constructed with an explicit absolute DLL path for focused loader tests.
 */
class CONVAISCENETAGGING_API FSceneAutoTaggerNativeCoreAdapter
{
public:
	explicit FSceneAutoTaggerNativeCoreAdapter(FString InExplicitDllPath = FString());
	~FSceneAutoTaggerNativeCoreAdapter();

	FSceneAutoTaggerNativeCoreAdapter(const FSceneAutoTaggerNativeCoreAdapter&) = delete;
	FSceneAutoTaggerNativeCoreAdapter& operator=(const FSceneAutoTaggerNativeCoreAdapter&) = delete;

	static FSceneAutoTaggerNativeCoreAdapter& Get();

	bool Initialize();
	void Shutdown();

	bool IsAvailable() const { return Api != nullptr; }
	const convai_sat_api_v4* GetApi() const { return Api; }
	/** Opaque identity of the core's embedded vision prompt/encoding; empty when unavailable. */
	FString GetVisionProtocolId() const;
	const FString& GetDiagnostic() const { return Diagnostic; }
	const FString& GetLoadedDllPath() const { return LoadedDllPath; }

private:
	FString ResolveDllPath() const;
	bool ValidateApi(const convai_sat_api_v4* InApi, FString& OutDiagnostic) const;
	void SetUnavailable(FString InDiagnostic);

	FString ExplicitDllPath;
	FString LoadedDllPath;
	FString Diagnostic;
	void* DllHandle = nullptr;
	const convai_sat_api_v4* Api = nullptr;
};
