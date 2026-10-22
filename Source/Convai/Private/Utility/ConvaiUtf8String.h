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

	/** The longest start of Source that fits in MaxBytes of UTF-8, cut
	 *  between characters. */
	inline FString TruncateToBytes(const FString& Source, int32 MaxBytes)
	{
		const FTCHARToUTF8 Utf8(*Source);
		if (Utf8.Length() <= MaxBytes)
		{
			return Source;
		}
		int32 Cut = FMath::Max(MaxBytes, 0);
		// A 10xxxxxx byte continues the character that starts before it.
		while (Cut > 0 && (static_cast<uint8>(Utf8.Get()[Cut]) & 0xC0) == 0x80)
		{
			--Cut;
		}
		return Source.Left(FUTF8ToTCHAR(Utf8.Get(), Cut).Length());
	}
}
