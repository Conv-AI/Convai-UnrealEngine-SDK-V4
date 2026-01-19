// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiConnectionSessionProxy.h"
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

void UConvaiConnectionSessionProxy::Disconnect() const
{
    if (UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(this))
    {
        ConvaiSubsystem->DisconnectSession(this);
    }
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


void UConvaiConnectionSessionProxy::BeginDestroy()
{
	// Ensure we're disconnected
	Disconnect();

	Super::BeginDestroy();
}
