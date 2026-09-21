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
	 * Every Blueprint that owns a Convai player component, native or BP_ConvaiPlayerComponent.
	 * That component is the one that adopts the microphone, and it only looks at its own actor's
	 * components -- so the capture has to be a sibling of it, wherever the project put it.
	 * A project that uses no Convai player is a normal result: the list is simply empty.
	 */
	static TArray<class UBlueprint*> FindBlueprintsWithConvaiPlayer();
};
