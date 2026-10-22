// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class FJsonObject;

/** Actual worker byte counters. Unknown total remains unset; bytes sent are not server acceptance. */
struct CONVAIAVATARSTUDIOEDITOR_API FConvaiAvatarTransferProgress
{
	int64 CompletedBytes = 0;
	TOptional<int64> TotalBytes;
	TOptional<float> Fraction() const;
	/** Worker protocol uses decimal strings to preserve counts beyond 32 bits. */
	static bool Parse(const FJsonObject& Json, FConvaiAvatarTransferProgress& Out);
};
