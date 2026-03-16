// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiConnectionSessionProxy.h"
#include "Core/ConvaiConnectionManager.h"
#include "ConvaiSubsystem.h"
#include "ConvaiUtils.h"

UConvaiConnectionSessionProxy::UConvaiConnectionSessionProxy()
    : ProxyID(FGuid::NewGuid())
    , bIsPlayer(false)
{
}

UConvaiConnectionSessionProxy::~UConvaiConnectionSessionProxy()
{
}

UConvaiConnectionSessionProxy* UConvaiConnectionSessionProxy::RequestSessionProxy(
    UObject* WorldContextObject,
    const FString& CharacterID,
    TScriptInterface<IConvaiConnectionInterface> ConnectionInterface)
{
    if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(WorldContextObject))
    {
        if (UConvaiConnectionManager* Manager = Subsystem->GetConnectionManager())
        {
            return Manager->AcquireConnection(CharacterID, ConnectionInterface);
        }
    }
    return nullptr;
}

void UConvaiConnectionSessionProxy::ReturnSessionProxy(
    UObject* WorldContextObject,
    const FString& CharacterID,
    UConvaiConnectionSessionProxy* Proxy)
{
    if (!IsValid(Proxy))
    {
        return;
    }

    if (UConvaiSubsystem* Subsystem = UConvaiUtils::GetConvaiSubsystem(WorldContextObject))
    {
        if (UConvaiConnectionManager* Manager = Subsystem->GetConnectionManager())
        {
            Manager->ReleaseConnection(CharacterID, Proxy);
            return;
        }
    }

    Proxy->Disconnect();
}

bool UConvaiConnectionSessionProxy::Initialize(const TScriptInterface<IConvaiConnectionInterface> InConnectionInterface, const bool bInIsPlayer)
{
    if (!InConnectionInterface.GetObject())
    {
        CONVAI_LOG(LogTemp, Error, TEXT("Failed to initialize connection session: Invalid connection interface"));
        return false;
    }
    
    ConnectionInterface = InConnectionInterface;
    bIsPlayer = bInIsPlayer;
    
    return true;
}

bool UConvaiConnectionSessionProxy::Connect(const FString& CharacterID)
{
    if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        return ConvaiSubsystem->ConnectSession(this, CharacterID);
    }
    
    return false;
}

void UConvaiConnectionSessionProxy::Disconnect()
{
    if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        // DisconnectSession handles attendee disconnection and cleanup
        ConvaiSubsystem->DisconnectSession(this);
    }

    // Clear the attendee ID
    AttendeeId.Empty();
}

int32 UConvaiConnectionSessionProxy::SendAudio(const int16_t* AudioData, const size_t NumFrames) const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        return ConvaiSubsystem->SendAudio(this, AudioData, NumFrames);
    }
    
    return -1;
}

void UConvaiConnectionSessionProxy::SendImage(const uint32 Width, const uint32 Height, TArray<uint8> Data) const
{
    if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->SendImage(this, Width, Height, Data);
    }
}

void UConvaiConnectionSessionProxy::StopVideoPublishing() const
{
    if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->StopVideoPublishing(this);
    }
}

void UConvaiConnectionSessionProxy::SendTextMessage(const FString& Message) const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->SendTextMessage(this, Message);
    }
}

void UConvaiConnectionSessionProxy::SendTriggerMessage(const FString& Trigger_Name, const FString& Trigger_Message) const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->SendTriggerMessage(this, Trigger_Name, Trigger_Message);
    }
}

void UConvaiConnectionSessionProxy::UpdateTemplateKeys(const TMap<FString, FString>& Template_Keys) const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->UpdateTemplateKeys(this, Template_Keys);
    }
}

void UConvaiConnectionSessionProxy::UpdateDynamicInfo(const FString& Context_Text) const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->UpdateDynamicInfo(this, Context_Text);
    }
}

void UConvaiConnectionSessionProxy::ToggleSTT(bool bMuted) const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->ToggleSTT(this, bMuted);
    }
}


EC_ConnectionState UConvaiConnectionSessionProxy::GetConnectionState() const
{
    if (const UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        return ConvaiSubsystem->GetSessionConnectionState(this);
    }
    return EC_ConnectionState::Disconnected;
}

void UConvaiConnectionSessionProxy::RebindConnectionInterface(TScriptInterface<IConvaiConnectionInterface> NewConnectionInterface)
{
    ConnectionInterface = NewConnectionInterface;
}

void UConvaiConnectionSessionProxy::ClearConnectionInterface()
{
    ConnectionInterface = TScriptInterface<IConvaiConnectionInterface>();
}

void UConvaiConnectionSessionProxy::BeginDestroy()
{
	Disconnect();
	Super::BeginDestroy();
}
