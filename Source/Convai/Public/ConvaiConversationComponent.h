// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiAudioStreamer.h"
#include "ConvaiConversationComponent.generated.h"

// Forward declarations
class UConvaiConversationComponent;

/**
 * Delegate for transcription events
 * Parameters:
 * - Speaker: The component that is speaking (source of the transcription)
 * - Listener: The component that is receiving the transcription
 * - Transcription: What this holds depends on the speaker. Read
 *   IsTranscriptionReady; a listener that assumes one shape renders the
 *   other's utterance either repeated or truncated.
 * - IsTranscriptionReady: True means Transcription is a standalone fragment to
 *   append to what came before. False means it is the whole utterance so far
 *   and replaces what came before.
 *
 *     Player     always false. The packet is forwarded exactly as the server
 *                sent it; on every session measured in PIE that is the whole
 *                utterance so far, and the final carries the server's
 *                corrected rendering of it. The plugin does not normalise
 *                this, so a session that sends word-sized deltas instead
 *                would reach a listener as deltas.
 *     Character  always true. Each broadcast is one sentence of the answer,
 *                appended to the ones before it, and the turn is closed by an
 *                EMPTY final.
 *
 * - IsFinal: The last broadcast for this utterance. For the player it carries
 *   the corrected whole utterance and is never empty. For the character it is
 *   ALWAYS EMPTY -- the sentences already carried every word, so repeating the
 *   assembled turn here would append the answer twice. A game that wants the
 *   character's finished text accumulates the sentences itself.
 *
 *   Known gap: when the player types rather than speaks, the server sends no
 *   bot-transcription, so the character's answer reaches this delegate not at
 *   all. Its only carrier there is bot-llm-text, whose sub-word tokens cannot
 *   be appended without mangling the spacing.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnTranscriptionReceivedSignature, UConvaiConversationComponent *, Speaker, UConvaiConversationComponent *, Listener, FString, Transcription, bool, IsTranscriptionReady, bool, IsFinal);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_ThreeParams(FOnAttendeeConnectionStateChangedSignature, UConvaiConversationComponent, OnAttendeeConnectionStateChangedEvent, UConvaiConversationComponent *, ConvaiConversationComponent, FString, AttendeeID, EC_ConnectionState, ConnectionState);

/**
 * Base class for Convai coversational components
 * This class is blueprintable and serves as a base for both player and chatbot components
 */
UCLASS(Blueprintable, Abstract)
class CONVAI_API UConvaiConversationComponent : public UConvaiAudioStreamer
{
    GENERATED_BODY()

public:
    /** Called when a transcription is received */
    UPROPERTY(BlueprintAssignable, Category = "Convai|Transcription")
    FOnTranscriptionReceivedSignature OnTranscriptionReceivedDelegate;

    UPROPERTY(BlueprintAssignable, Category = "Convai|Connection")
    FOnAttendeeConnectionStateChangedSignature OnAttendeeConnectionStateChangedEvent;
    /**
     * Determines if this component represents a player
     * @return True if this is a player component, false if it's a chatbot or other component
     */
    UFUNCTION(BlueprintPure, Category = "Convai")
    virtual bool IsPlayer() const;

    /**
     * Gets the name of this component (player name or character name)
     * @return The name of this component
     */
    UFUNCTION(BlueprintPure, Category = "Convai")
    virtual FString GetConversationalName() const;
};