// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiAvatarAssetsClient.h"

namespace ConvaiAvatarAssetsClientPrivate
{
/** Appends a library page's avatars whose ids Library does not hold yet: the first record of an id wins, and order is kept. */
void AppendNewAvatars(TArray<FConvaiAvatarAsset>& Library, TArray<FConvaiAvatarAsset> Page);
/** The error a request reports for an HTTP status; empty for 2xx. */
FString StatusError(int32 Code);
/** True only for the error a 404 gives: the service says the asset, or its file, does not exist. */
bool IsNotFound(const FString& Error);
}
