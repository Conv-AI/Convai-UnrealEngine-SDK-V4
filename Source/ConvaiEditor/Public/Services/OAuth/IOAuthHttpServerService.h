/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * IOAuthHttpServerService.h
 *
 * Interface for local OAuth HTTP listener.
 */

#pragma once

#include "CoreMinimal.h"
#include "ConvaiEditor.h"
#include "Templates/SharedPointer.h"

/**
 * Local HTTP server for OAuth redirect handling.
 */
class CONVAIEDITOR_API IOAuthHttpServerService : public IConvaiService
{
public:
    virtual ~IOAuthHttpServerService() override = default;

    virtual bool StartServer(const TArray<int32> &PreferredPorts) = 0;
    virtual void StopServer() = 0;
    virtual bool IsRunning() const = 0;
    virtual int32 GetPort() const = 0;

    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnApiKeyReceived, const FString & /*EncryptedKey*/, const FString & /*EncryptedUserInfo*/);
    virtual FOnApiKeyReceived &OnApiKeyReceived() = 0;

    static FName StaticType() { return TEXT("IOAuthHttpServerService"); }
};