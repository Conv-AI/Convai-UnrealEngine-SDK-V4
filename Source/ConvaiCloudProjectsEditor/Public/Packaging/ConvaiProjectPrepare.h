// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** What a prepare pass changed, so the UI can tell the user before anything is packaged. */
struct FConvaiProjectPrepareReport
{
	/** One line per change actually made. Empty means the project was already set up. */
	TArray<FString> Changes;
	/** Non-blocking observations, e.g. no player Blueprint could be resolved. */
	TArray<FString> Warnings;
	FString Error;

	bool IsSuccess() const { return Error.IsEmpty(); }
};

/**
 * The editor-side setup a project needs before it can stream from the cloud. Every step is
 * idempotent, so running an upload twice changes nothing the second time. Game thread only.
 */
class FConvaiProjectPrepare
{
public:
	/** Enables Pixel Streaming and puts the streaming audio component on the player pawn. */
	static FConvaiProjectPrepareReport Run();

	/** The project's own name, used to seed the create form and to detect a mismatched update. */
	static FString GetProjectName();

	/** True when the Pixel Streaming plugin is enabled for this project. */
	static bool IsPixelStreamingEnabled();

private:
	static bool EnablePixelStreaming(FConvaiProjectPrepareReport& Report);
	static void AddStreamingAudioComponent(FConvaiProjectPrepareReport& Report);

	/**
	 * Every project Blueprint whose actors own a Convai player component, native or BP_ConvaiPlayerComponent,
	 * from its own construction script, an ancestor's, or a C++ class in NativePlayerClasses.
	 * That component is the one that adopts the microphone, and it only looks at its own actor's
	 * components -- so the capture has to be a sibling of it, wherever the project put it.
	 * A project that uses no Convai player is a normal result: the list is simply empty.
	 */
	static TArray<class UBlueprint*> FindBlueprintsWithConvaiPlayer(const TArray<class UClass*>& NativePlayerClasses);

	/** The registry's answer to "may Prepare load and edit this asset", asked before anything loads. */
	static bool IsProjectBlueprint(const struct FAssetData& Asset);

	/** Where a Blueprint's actors get a component from, nearest first. */
	enum class EComponentSource : uint8
	{
		None,
		/** A node of the Blueprint's own construction script. */
		OwnScript,
		/** A node of an ancestor Blueprint's construction script, inherited. */
		ParentScript,
		/** A default subobject that a C++ constructor creates. */
		Native,
	};

	/** Asks for the player component and for the capture alike, so both see the same inheritance. */
	static EComponentSource FindComponentSource(const class UBlueprint* Blueprint, class UClass* ComponentClass);

	/** A current C++ actor class whose defaults hold ComponentClass; a class replaced by a reload or deprecated is not. */
	static bool IsNativeHolder(const class UClass* Class, class UClass* ComponentClass);

	/**
	 * Where Blueprint's capture goes: the highest project Blueprint in its chain that sees the player, stopping below
	 * any that is above a target with a capture of its own. Null when Blueprint itself is above one.
	 */
	static class UBlueprint* FindCaptureOwner(class UBlueprint* Blueprint, const TArray<class UBlueprint*>& Targets,
		class UClass* PlayerClass, class UClass* CaptureClass);

#if WITH_DEV_AUTOMATION_TESTS
	friend class FConvaiProjectPrepareHarness;
#endif
};
