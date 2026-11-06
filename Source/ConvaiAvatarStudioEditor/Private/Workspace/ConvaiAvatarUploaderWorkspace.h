// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** One exclusive lease covers uploader setup, cooking, and active file transfers. */
class FConvaiAvatarUploaderLease
{
public:
    FConvaiAvatarUploaderLease() = default;
    ~FConvaiAvatarUploaderLease();
    FConvaiAvatarUploaderLease(const FConvaiAvatarUploaderLease&) = delete;
    FConvaiAvatarUploaderLease& operator=(const FConvaiAvatarUploaderLease&) = delete;
    bool Acquire(const FString& Root, FString& Error);
    void Release();
private:
    void* Handle = nullptr;
};

namespace ConvaiAvatarUploaderWorkspace
{
    FString ProjectPath(const FString& Root);
    /** Caller holds the uploader lease. Creates CA, upgrades an owned legacy descriptor,
     *  or restores a descriptor after an interrupted HTTP-only bootstrap. Never follows avatar links. */
    bool EnsureProject(const FString& Root, FString& Error);
}
