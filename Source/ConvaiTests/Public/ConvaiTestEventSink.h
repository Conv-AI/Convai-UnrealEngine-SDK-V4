// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"
#include "UObject/Object.h"
#include "ConvaiTestEventSink.generated.h"

class UConvaiChatbotComponent;
class UConvaiConversationComponent;
class UConvaiPlayerComponent;

/**
 * Binds the plugin's Blueprint-facing events so a scenario can assert on them.
 *
 * A UObject rather than plain state because the plugin's events are
 * DECLARE_DYNAMIC_MULTICAST delegates, which can only bind a UFUNCTION on a
 * UObject. Scenarios are plain C++ classes, so one of these stands between them
 * and the delegate.
 *
 * Everything recorded here is what a game would receive. Nothing is read
 * through a back door, which is the whole of ADR-0005's argument: if an
 * assertion needs something this cannot see, the plugin is not exposing it to
 * customers either.
 */
UCLASS()
class CONVAITESTS_API UConvaiTestEventSink : public UObject
{
    GENERATED_BODY()

public:
    struct FTranscript
    {
        FString Speaker;
        FString Listener;
        FString Text;
        bool bReady = false;
        bool bFinal = false;
        double AtSeconds = 0.0;
    };

    UFUNCTION()
    void HandleTranscription(UConvaiConversationComponent* Speaker,
                             UConvaiConversationComponent* Listener, FString Transcription,
                             bool IsTranscriptionReady, bool IsFinal);

    /** One completed bot turn. The server reports how the turn ended, and the
     *  difference matters: a turn that was interrupted is a success for
     *  barge-in and a failure for a scenario that expected a full answer. */
    struct FBotTurn
    {
        FString ResponseId;
        bool bInterrupted = false;
        bool bAborted = false;
        FString ErrorReason;
        double AtSeconds = 0.0;
    };

    UFUNCTION()
    void HandleFailure();

    /** The character-details request's outcome. OnFailureEvent carries no
     *  cause; this is the one failure that fires it on an otherwise healthy
     *  session, so a scenario can name it instead of blaming the exchange. */
    UFUNCTION()
    void HandleCharacterDataLoaded(UConvaiChatbotComponent* ChatbotComponent, bool Success);

    int32 CharacterDataLoadFailures() const;

    /** One server-reported error, as the game receives it. */
    struct FServerError
    {
        FString Message;
        bool bFatal = false;
        double AtSeconds = 0.0;
    };

    UFUNCTION()
    void HandleServerError(UConvaiChatbotComponent* ChatbotComponent, FString Message, bool bFatal);

    UFUNCTION()
    void HandleBotTurnCompleted(UConvaiChatbotComponent* ChatbotComponent, FString ResponseId,
                                bool bWasInterrupted, bool bWasAborted, FString ErrorReason);

    UFUNCTION()
    void HandleInterrupted(UConvaiChatbotComponent* ChatbotComponent,
                           UConvaiPlayerComponent* InteractingPlayerComponent);

    /** The chatbot's emotion state as a game would read it right after
     *  OnEmotionStateChanged: the strongest basic emotion and its score.
     *  None with score 0 is what a neutral -- or an undecodable -- packet
     *  leaves behind, so a scenario can tell "no emotion" from "wrong name". */
    struct FEmotionSample
    {
        EBasicEmotions Dominant = EBasicEmotions::None;
        float Score = 0.0f;
        double AtSeconds = 0.0;
    };

    UFUNCTION()
    void HandleEmotionStateChanged(UConvaiChatbotComponent* ChatbotComponent,
                                   UConvaiPlayerComponent* InteractingPlayerComponent);

    TArray<FEmotionSample> EmotionSamples() const;

    /** One OnActionReceivedEvent_V2 broadcast, as a game receives it: the whole
     *  sequence the model chose for a turn, already parsed against the actions
     *  the chatbot advertised at connect. */
    struct FActionBatch
    {
        TArray<FConvaiResultAction> Actions;
        double AtSeconds = 0.0;
    };

    UFUNCTION()
    void HandleActionsReceived(UConvaiChatbotComponent* ChatbotComponent,
                               UConvaiPlayerComponent* InteractingPlayerComponent,
                               const TArray<FConvaiResultAction>& SequenceOfActions);

    TArray<FActionBatch> ActionBatches() const;

    /** One OnFacialDataReady: the face-sync component applied a lip-sync frame
     *  to its blendshape map this tick. Not receipt -- frames arrive on the
     *  transport thread with no public event -- but the earliest evidence a
     *  game gets that lip-sync is flowing. */
    UFUNCTION()
    void HandleFacialDataReady();

    int32 FacialFrames() const;

    TArray<FTranscript> Transcripts() const;

    /** Transcripts whose speaker component had this name, in arrival order.
     *  Both the player and the chatbot broadcast on the same delegate shape
     *  with themselves as Speaker, so the name is what separates the two
     *  halves of a roundtrip. */
    TArray<FTranscript> TranscriptsFrom(const FString& SpeakerName) const;

    /** The last non-empty final transcript from one speaker. Same reason as
     *  LatestFinalText for ignoring the trailing empty final. */
    FString LatestFinalTextFrom(const FString& SpeakerName) const;

    TArray<FBotTurn> BotTurns() const;

    int32 InterruptCount() const;

    /** The last non-empty transcript flagged final, which is the one to score.
     *  Partials arrive continuously and scoring one would measure timing.
     *
     *  Non-empty matters: the server sends a second, empty final transcript
     *  after the real one -- observed on every live run -- so "the last final"
     *  is always blank. Anything binding this delegate to display a transcript
     *  will clear it a moment after showing it. */
    FString LatestFinalText() const;

    /** Distinct speakers seen. More than one player speaker across Talk Targets
     *  is the duplicate-transcript invariant tripping. */
    TSet<FString> Speakers() const;

    int32 FailureCount() const;

    TArray<FServerError> ServerErrors() const;

private:
    mutable FCriticalSection Lock;
    TArray<FTranscript> Recorded;
    TArray<FBotTurn> Turns;
    TArray<FServerError> ServerErrorsSeen;
    TArray<FEmotionSample> Emotions;
    TArray<FActionBatch> Batches;
    int32 Failures = 0;
    int32 Interrupts = 0;
    int32 FacialFrameCount = 0;
    int32 CharacterDataFailures = 0;
};
