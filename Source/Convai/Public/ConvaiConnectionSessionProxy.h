// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiConnectionSessionProxy.generated.h"

// Forward declarations
class UConvaiSubsystem;

/**
 * Connection session proxy class
 * Acts as an intermediary between components and the Convai subsystem
 */
UCLASS()
class CONVAI_API UConvaiConnectionSessionProxy : public UObject
{
    GENERATED_BODY()

public:
    UConvaiConnectionSessionProxy();
    virtual ~UConvaiConnectionSessionProxy();
    
	//~ Begin UObject Interface.
	virtual void BeginDestroy() override;
	//~ End UObject Interface.

    /**
     * Initialize the session with a connection interface
     * @param InConnectionInterface - The interface to receive callbacks
     * @param bInIsPlayer - Whether this is a player session
     * @return True if initialization was successful
     */
    bool Initialize(TScriptInterface<IConvaiConnectionInterface> InConnectionInterface, bool bInIsPlayer);
    
    /**
     * Connect to the Convai service
     * @param CharacterID - The ID of the character to connect to (ignored for player sessions)
     * @return True if connection was initiated successfully
     */
    bool Connect(const FString& CharacterID = TEXT(""));
    
    /**
     * Disconnect from the Convai service
     */
    void Disconnect() const;
    
    /**
     * Send audio data through the session
     * @param AudioData - The audio data to send
     * @param NumFrames - The number of frames in the audio data
     * @return The number of bytes sent, or -1 on failure
     */
    int32 SendAudio(const int16_t* AudioData, size_t NumFrames) const;

    void SendImage(uint32 Width, uint32 Height, TArray<uint8> Data) const;
    
    void SendTextMessage(const FString& Message) const;
    
    void SendTriggerMessage(const FString& Trigger_Name, const FString& Trigger_Message) const;
    
    void UpdateTemplateKeys(const TMap<FString, FString>& Template_Keys) const;
    
    void UpdateDynamicInfo(const FString& Context_Text) const;
    
    /**
     * Get the proxy ID
     * @return The unique ID for this proxy
     */
    FGuid GetProxyID() const { return ProxyID; }
    
    /**
     * Get the connection interface
     * @return The connection interface for this proxy
     */
    TScriptInterface<IConvaiConnectionInterface> GetConnectionInterface() const { return ConnectionInterface; }
    
    /**
     * Check if this is a player session
     * @return True if this is a player session, false otherwise
     */
    bool IsPlayerSession() const { return bIsPlayer; }

private:
    // Unique ID for this proxy
    FGuid ProxyID;
    
    // Connection interface for callbacks
    TScriptInterface<IConvaiConnectionInterface> ConnectionInterface;
    
    // Is this a player session
    bool bIsPlayer;
};