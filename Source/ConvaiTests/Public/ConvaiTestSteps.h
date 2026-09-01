// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "ConvaiTestScenario.h"
#include "CoreMinimal.h"

class UConvaiChatbotComponent;
class UConvaiConversationComponent;
class UConvaiPlayerComponent;
class UConvaiVirtualMicComponent;
class UConvaiTestEventSink;

/**
 * Issue 04's step library. Steps are C++ functions, not a scenario language —
 * the JSON front-end (issue 11) is extracted from what recurs across written
 * scenarios rather than invented ahead of them.
 *
 * Free functions taking their subject explicitly, because there is no state to
 * hold: every step acts on a component the scenario already spawned. Three of
 * the four are one line over API the plugin already exposes, and they exist as
 * named steps so a scenario reads as a script rather than as component
 * plumbing. SpeakWav is the one with substance.
 */
namespace ConvaiTestSteps
{
    /** Where the WAV fixtures and STT.json live: Source/ConvaiTests/Data,
     *  stripped with the module at release (issue 12). */
    CONVAITESTS_API FString DataDir();

    /**
     * Loads a fixture, resamples it to the Virtual Mic's rate, and queues it.
     *
     * Resampling goes through UConvaiUtils::ResampleAudio — the same call the
     * plugin's own capture path uses — so a fixture is degraded exactly the way
     * real microphone audio is, and a resampler bug shows up in both rather
     * than only in the thing that has no test.
     *
     * @param FixtureName  bare name, no extension: "S1", "S3", "M1"
     * @return false with OutError set; the caller is expected to fail the
     *         scenario rather than measure silence.
     */
    CONVAITESTS_API bool SpeakWav(UConvaiVirtualMicComponent* Mic, const FString& FixtureName,
                                  FString& OutError);

    /** A scripted pause. The stream keeps running: stopping it is a different
     *  code path from a quiet microphone and would not be a pause. */
    CONVAITESTS_API void Silence(UConvaiVirtualMicComponent* Mic, float Seconds);

    /** Routes the microphone to the conversation. One Connection per process
     *  here, so the targets are not addressed individually — what the step
     *  does is open the player's own session, without which no audio streams
     *  at all. Scenarios still name their targets so they read the same
     *  wherever Talk Targets do exist. */
    CONVAITESTS_API void SetTalkTargets(UConvaiPlayerComponent* Player,
                                        const TArray<UConvaiChatbotComponent*>& Targets);

    CONVAITESTS_API void SayText(UConvaiPlayerComponent* Player,
                                 UConvaiConversationComponent* Chatbot, const FString& Text);

    /** The fixture's known transcript from STT.json. Empty if absent. */
    CONVAITESTS_API FString ExpectedTranscript(const FString& FixtureName);

    /**
     * Word error rate, Levenshtein over words after case and punctuation are
     * folded away. Reported, never asserted: issue 04 keeps ASR quality out of
     * the pass condition because it is model-dependent, and a threshold here
     * would fail on the model changing rather than on the plugin breaking.
     *
     * Edits are normalised by the *reference* length, per the standard
     * definition, so the result exceeds 1.0 when the hypothesis is longer than
     * the reference. Returns 1.0 when the reference is empty and the hypothesis
     * is not.
     */
    CONVAITESTS_API double WordErrorRate(const FString& Reference, const FString& Hypothesis);

    /**
     * The finding for OnFailureEvent(s) during an exchange that otherwise ran.
     * Keyed by what the sink saw fail, so a flaky character-details request
     * gets its own occurrence rate instead of reading as a broken response
     * path. Exchange names the scenario's action for the evidence text.
     */
    CONVAITESTS_API FConvaiScenarioFinding FailureEventFinding(int32 Failures,
                                                               int32 CharacterDataFailures,
                                                               const FString& Exchange,
                                                               const TMap<FString, double>& Metrics);

    /**
     * The character's answer, assembled from its increments.
     *
     * Not LatestFinalTextFrom: the character's final is always empty by
     * contract, because the increments already carried every word. A game that
     * wants the finished answer accumulates the non-final broadcasts exactly
     * like this, and so does every assertion here.
     */
    CONVAITESTS_API FString AssembledTextFrom(const UConvaiTestEventSink& Sink,
                                              const FString& SpeakerName);

    /** Which shape a speaker's broadcasts are contracted to carry. The two
     *  differ, and a listener written for one renders the other's utterance
     *  either repeated or truncated -- see FOnTranscriptionReceivedSignature. */
    enum class ETranscriptShape : uint8
    {
        /** Player: every broadcast is the whole utterance so far and replaces
         *  what came before; the final carries the corrected whole utterance
         *  and is never empty; IsTranscriptionReady is never set. */
        WholeUtterance,
        /** Character: every broadcast is one sentence of the answer, which the
         *  listener appends; IsTranscriptionReady is always set; the final is
         *  always empty, because the sentences already carried every word. */
        Increment,
    };

    /**
     * Grades one speaker's transcript stream against its contracted shape.
     *
     * Exists because nothing else can see it. Every scenario grades
     * LatestFinalTextFrom, and the final was still correct while the partials
     * had turned into whole utterances -- which is how a chat widget that
     * appends what it is handed came to render "Hello Hello. Hello." with the
     * whole suite green.
     *
     * Under WholeUtterance a non-final broadcast must extend the one before it;
     * the final is exempt, being the server's re-punctuated rendering, which
     * legitimately does not extend the deltas (" Yeah mine." -> "Yeah, mine.").
     * Under Increment a non-final broadcast must NOT contain everything
     * broadcast so far -- one that does is the whole answer being re-sent, the
     * regression this exists to catch.
     *
     * Returns an unset finding when the stream honours its contract, or when
     * the speaker broadcast nothing -- absence is the caller's assertion to
     * make, not this one's.
     */
    CONVAITESTS_API TOptional<FConvaiScenarioFinding> TranscriptShapeFinding(
        const UConvaiTestEventSink& Sink, const FString& SpeakerName, ETranscriptShape Shape,
        const TMap<FString, double>& Metrics);
}
