// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace ConvaiUtf8
{
	/**
	 * Owns a null-terminated UTF-8 representation without imposing an arbitrary
	 * payload-size limit. The returned pointer remains valid for the lifetime of
	 * the array.
	 */
	inline TArray<ANSICHAR> ConvertToNullTerminatedBytes(const FString& Source)
	{
		const FTCHARToUTF8 Converter(*Source);
		TArray<ANSICHAR> Bytes;
		Bytes.SetNumUninitialized(Converter.Length() + 1);
		if (Converter.Length() > 0)
		{
			FMemory::Memcpy(Bytes.GetData(), Converter.Get(), Converter.Length());
		}
		Bytes[Converter.Length()] = '\0';
		return Bytes;
	}
}
