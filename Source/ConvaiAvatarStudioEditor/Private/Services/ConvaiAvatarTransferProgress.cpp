// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarTransferProgress.h"
#include "Dom/JsonObject.h"

namespace
{
	// Overflow guard for the worker's decimal byte counts; keep at or above
	// ConvaiAvatarFileTransferPrivate::MaximumBytes or large transfers stop reporting progress.
	constexpr int64 MaximumParsedBytes = 10485760000LL;
	bool ReadBytes(const TSharedPtr<FJsonValue>& Value, int64& Out)
	{
		if (!Value || Value->Type != EJson::String) return false;
		const FString Text = Value->AsString();
		if (Text.IsEmpty() || Text.Len() > 20) return false;
		int64 Bytes = 0;
		for (TCHAR C : Text)
		{
			if (C < TEXT('0') || C > TEXT('9') || Bytes > (MaximumParsedBytes - (C - TEXT('0'))) / 10) return false;
			Bytes = Bytes * 10 + (C - TEXT('0'));
		}
		Out = Bytes;
		return true;
	}
}

TOptional<float> FConvaiAvatarTransferProgress::Fraction() const
{
	if (!TotalBytes.IsSet() || TotalBytes.GetValue() <= 0 || CompletedBytes < 0 || CompletedBytes > TotalBytes.GetValue()) return {};
	return static_cast<float>(static_cast<double>(CompletedBytes) / static_cast<double>(TotalBytes.GetValue()));
}

bool FConvaiAvatarTransferProgress::Parse(const FJsonObject& Json, FConvaiAvatarTransferProgress& Out)
{
	Out = {};
	FConvaiAvatarTransferProgress Result;
	if (!ReadBytes(Json.TryGetField(TEXT("bytes")), Result.CompletedBytes)) return false;
	if (const TSharedPtr<FJsonValue> Total = Json.TryGetField(TEXT("total")))
	{
		int64 TotalBytes = 0;
		if (!ReadBytes(Total, TotalBytes)) return false;
		if (TotalBytes > 0) Result.TotalBytes = TotalBytes;
	}
	if (Result.TotalBytes.IsSet() && Result.CompletedBytes > Result.TotalBytes.GetValue()) return false;
	Out = Result;
	return true;
}
