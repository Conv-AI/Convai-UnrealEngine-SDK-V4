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
 * - Transcription: The transcription text
 * - IsTranscriptionReady: Whether the transcription is ready to be displayed
 * - IsFinal: Whether this is the final transcription for the current utterance
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnTranscriptionReceivedSignature, UConvaiConversationComponent *, Speaker, UConvaiConversationComponent *, Listener, FString, Transcription, bool, IsTranscriptionReady, bool, IsFinal);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_ThreeParams(FOnParticipantConnectionStateChangedSignature, UConvaiConversationComponent, OnParticipantConnectionStateChangedEvent, UConvaiConversationComponent *, ConvaiConversationComponent, FString, ParticipantID, EC_ConnectionState, ConnectionState);

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
    FOnParticipantConnectionStateChangedSignature OnParticipantConnectionStateChangedEvent;
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