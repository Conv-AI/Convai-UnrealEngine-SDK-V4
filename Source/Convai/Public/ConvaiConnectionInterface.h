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

    virtual bool IsVisionSupported() { return false; }

    virtual void OnConnectedToServer() {}

    virtual void OnDisconnectedFromServer() {}
    
    virtual void OnParticipantConnected(FString ParticipantId){}

    virtual void OnParticipantDisconnected(FString ParticipantId){}

    /** Called when transcription data is received */
    virtual void OnTranscriptionReceived(FString Transcription, bool IsTranscriptionReady, bool IsFinal) {}
    
    /** Called when the bot starts talking */
    virtual void OnStartedTalking() {}
    
    /** Called when the bot finishes talking */
    virtual void OnFinishedTalking() {}
    
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
