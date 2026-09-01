// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "ConvaiTestEventSink.h"
#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class FConvaiTestEventRecorder;
class UConvaiChatbotComponent;
class UConvaiPlayerComponent;
class UConvaiVirtualMicComponent;
class UWorld;

/**
 * The actor and components every live scenario needs, spawned once.
 *
 * Extracted after the fourth scenario opened with the same forty lines. It is
 * deliberately not a base class: scenarios differ in what they do, not in what
 * they are, and a fixture that owns the scenario would have to grow a hook for
 * every difference.
 *
 * The component set is the one a game ships — a player component and a chatbot
 * component on one actor — with the Virtual Mic standing in for the microphone
 * through IConvaiAudioCaptureInterface, which is the seam the plugin already
 * offers third parties (ADR-0005). Nothing here reaches past a public API.
 */
struct CONVAITESTS_API FConvaiTestFixture
{
    struct FOptions
    {
        /** Leave empty for UConvaiUtils::GetTestCharacterID(). A scenario that
         *  wants a connection to fail sets a deliberately invalid one. */
        FString CharacterID;

        /** Bind the sink to the chatbot's transcript delegate as well as the
         *  player's. Both broadcast with themselves as Speaker, so one sink
         *  records both sides and the speaker name separates them. */
        bool bListenToBotTranscript = true;

        /** Soft class path of a character Blueprint, /Game/....BP_X_C. Empty
         *  spawns the bare actor. With a class the chatbot is the Blueprint's
         *  own; the player and Virtual Mic still go on the bare actor, so
         *  nothing is doubled and mic adoption order is unchanged. */
        FString ActorClassPath;
    };

    /** Credentials only. Separate from Spawn so a scenario can fail with a
     *  precise reason before it has built anything to tear down. */
    static bool HasCredentials(FString& OutError, bool bRequireCharacter = true);

    /** The -ConvaiTestActorClass= input. False with OutError when absent, so
     *  every configured-world scenario refuses with the same message. */
    static bool HasConfiguredActorClass(FString& OutPath, FString& OutError);

    /** False with OutError set; the caller is expected to fail the scenario.
     *  Records setup milestones on Recorder. */
    bool Spawn(UWorld* World, FConvaiTestEventRecorder& Recorder, const FOptions& Options,
               FString& OutError);

    /** Destroys the actor and drops the sink. Safe to call twice, and safe on a
     *  fixture that never spawned. */
    void Destroy();

    /** Connected, from the chatbot's own public state. */
    bool IsChatbotConnected() const;

    TWeakObjectPtr<AActor> Owner;

    /** The spawned character Blueprint; unset for the bare fixture. */
    TWeakObjectPtr<AActor> Character;
    TWeakObjectPtr<UConvaiVirtualMicComponent> Mic;
    TWeakObjectPtr<UConvaiPlayerComponent> Player;
    TWeakObjectPtr<UConvaiChatbotComponent> Chatbot;

    // Strong: nothing else references the sink, and a GC between the delegate
    // firing and the scenario reading it would lose the run's evidence.
    TStrongObjectPtr<UConvaiTestEventSink> Sink;

    FString CharacterID;
};
