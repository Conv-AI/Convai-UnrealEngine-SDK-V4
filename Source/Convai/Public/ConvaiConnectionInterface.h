// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ConvaiDefinitions.h"
#include "ConvaiConnectionInterface.generated.h"

UINTERFACE(MinimalAPI)
class UConvaiConnectionInterface : public UInterface
{
    GENERATED_BODY()
};

/**
 * Interface for handling connection callbacks
 * Implement this interface to receive callbacks from various connection types
 */
class CONVAI_API IConvaiConnectionInterface
{
    GENERATED_BODY()

public:
    /** Returns the action_config JSON to send at /connect, or empty string to omit it.
     *  Empty string means this connection participant does not opt into action grounding. */
    virtual FString GetActionConfigJson() { return FString(); }

    /** The action templates (plus injected built-in action names) this owner just
     *  advertised via GetActionConfigJson. Called right after it when a /connect
     *  payload is built, so the subsystem can publish the contract onto the session
     *  proxy — the contract belongs to the CONNECTION and must survive orphan
     *  rebinds to a different owner. Default: an explicit EMPTY contract (matches
     *  the default GetActionConfigJson). */
    virtual void GetAdvertisedActionContract(TArray<FConvaiAction>& OutActions, TArray<FString>& OutBuiltInNames)
    {
        OutActions.Reset();
        OutBuiltInNames.Reset();
    }

    /** Returns the End User ID for long term memory (LTM) */
    virtual FString GetEndUserID() { return FString(); }

    /** Returns the End User Metadata as a JSON string */
    virtual FString GetEndUserMetadata() { return FString(); }

    virtual bool IsVisionSupported() { return false; }

    virtual EC_LipSyncMode GetLipSyncMode() { return EC_LipSyncMode::Off; }

    /** Returns true if the lipsync component requires precomputed face data from server */
    virtual bool RequiresPrecomputedFaceData() { return true; }

    virtual void OnConnectedToServer() {}

    virtual void OnDisconnectedFromServer() {}
    
    virtual void OnAttendeeConnecting(){}

    virtual void OnAttendeeConnected(FString AttendeeId){}

    virtual void OnAttendeeDisconnected(FString AttendeeId){}

    /** Called when transcription data is received */
    virtual void OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal) {}
    
    /** Called when the bot starts talking */
    virtual void OnStartedTalking() {}

    /** Response-aware bridge for bot-started-speaking. Existing connection
     *  implementations remain compatible through the zero-argument callback. */
    virtual void OnStartedTalkingForResponse(const FString& ResponseId) { OnStartedTalking(); }
    
    /** Called when the bot finishes talking */
    virtual void OnFinishedTalking() {}

    /** Response-aware bridge for bot-stopped-speaking. Existing connection
     *  implementations remain compatible through the zero-argument callback. */
    virtual void OnFinishedTalkingForResponse(const FString& ResponseId) { OnFinishedTalking(); }

    /** Authoritative response lifecycle terminal. This does not imply that
     *  locally buffered audio or facial animation has finished playing. */
    virtual void OnBotTurnCompleted(const FString& ResponseId, bool bWasInterrupted,
        bool bWasAborted, const FString& ErrorReason) {}

    /** Called when the LLM starts generating a response */
    virtual void OnLLMStarted() {}

    /** Response-aware bridge for bot-llm-started. */
    virtual void OnLLMStartedForResponse(const FString& ResponseId) { OnLLMStarted(); }

    /** Called when the LLM finishes generating a response */
    virtual void OnLLMStopped() {}

    /** Called when the user interrupts (starts speaking while bot is talking) */
    virtual void OnInterrupt() {}

    /** Called when the interrupt ends (user stops speaking) */
    virtual void OnInterruptEnd() {}
    
    /** Called when audio data is received */
    virtual void OnAudioDataReceived(const int16_t* AudioData, size_t NumFrames, uint32_t SampleRate, uint32_t BitsPerSample, uint32_t NumChannels) {}
    
    /** Called when face animation data is received */
    virtual void OnFaceDataReceived(FAnimationSequence FaceDataAnimation) {}
    
    /** Called when a session ID is received */
    virtual void OnSessionIDReceived(FString ReceivedSessionID) {}
    
    /** Called when an interaction ID is received */
    virtual void OnInteractionIDReceived(FString ReceivedInteractionID) {}
    
    /** Called when action sequence data is received */
    virtual void OnActionSequenceReceived(const TArray<FConvaiResultAction>& ReceivedSequenceOfActions) {}
    
    /** Called when emotion data is received */
    virtual void OnEmotionReceived(FString ReceivedEmotionResponse, FAnimationFrame EmotionBlendshapesFrame, bool MultipleEmotions) {}
    
    /** Called when narrative section data is received */
    virtual void OnNarrativeSectionReceived(FString BT_Code, FString BT_Constants, FString ReceivedNarrativeSectionID) {}
    
    /** Called when a failure occurs */
    virtual void OnFailure(FString Message) {}

    /**
     * Called when the server reports an error over the data channel.
     *
     * Separate from OnFailure because severity is separate from the packet type. An
     * `error-response` is always advisory; an `error` carries its own `fatal` flag and is
     * usually advisory too -- one measured session took 90 of them, none fatal. A healthy
     * connected session receives both kinds, so treating either as a failure would report a
     * failure on every successful connection. Fatal errors still reach OnFailure, by default
     * here.
     */
    virtual void OnServerError(const FString& Message, bool bFatal)
    {
        if (bFatal)
        {
            OnFailure(Message);
        }
    }
};
