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
    virtual UConvaiEnvironment* GetConvaiEnvironment() { return nullptr; }

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
    
    virtual void OnAttendeeConnected(FString AttendeeId){}

    virtual void OnAttendeeDisconnected(FString AttendeeId){}

    /** Called when transcription data is received */
    virtual void OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal) {}
    
    /** Called when the bot starts talking */
    virtual void OnStartedTalking() {}
    
    /** Called when the bot finishes talking */
    virtual void OnFinishedTalking() {}

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
};
