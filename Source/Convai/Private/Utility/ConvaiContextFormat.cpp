// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiContextFormat.h"

FString ConvaiContextFormat::SanitizeKey(const FString& Key)
{
	// Fast path: no whitespace, nothing to do.
	bool bHasWhitespace = false;
	for (const TCHAR Ch : Key)
	{
		if (FChar::IsWhitespace(Ch))
		{
			bHasWhitespace = true;
			break;
		}
	}
	if (!bHasWhitespace)
	{
		return Key;
	}

	// Pascal-case each dot-segment that contains whitespace: every word's first
	// letter is uppercased, including the first ("door state" → "DoorState").
	// Whitespace-free segments pass through untouched so existing keys like
	// "bLocked" or "Stats.HP" are never mangled.
	TArray<FString> Segments;
	Key.ParseIntoArray(Segments, TEXT("."), /*bCullEmpty*/ false);
	for (FString& Segment : Segments)
	{
		bool bSegmentHasWhitespace = false;
		for (const TCHAR Ch : Segment)
		{
			if (FChar::IsWhitespace(Ch))
			{
				bSegmentHasWhitespace = true;
				break;
			}
		}
		if (!bSegmentHasWhitespace)
		{
			continue;
		}

		FString Rebuilt;
		Rebuilt.Reserve(Segment.Len());
		bool bCapitalizeNext = true;
		for (const TCHAR Ch : Segment)
		{
			if (FChar::IsWhitespace(Ch))
			{
				bCapitalizeNext = true;
				continue;
			}
			Rebuilt.AppendChar(bCapitalizeNext ? FChar::ToUpper(Ch) : Ch);
			bCapitalizeNext = false;
		}
		Segment = MoveTemp(Rebuilt);
	}
	return FString::Join(Segments, TEXT("."));
}

FString ConvaiContextFormat::FormatValueForPrompt(const FString& Value)
{
	bool bNeedsQuoting = Value.IsEmpty();
	for (const TCHAR Ch : Value)
	{
		if (FChar::IsWhitespace(Ch))
		{
			bNeedsQuoting = true;
			break;
		}
	}
	if (!bNeedsQuoting)
	{
		return Value;
	}

	FString Escaped;
	Escaped.Reserve(Value.Len() + 2);
	for (const TCHAR Ch : Value)
	{
		switch (Ch)
		{
			case TEXT('"'):  Escaped += TEXT("\\\""); break;
			case TEXT('\\'): Escaped += TEXT("\\\\"); break;
			case TEXT('\n'): Escaped += TEXT("\\n");  break; // keep the context one-line-per-entry
			case TEXT('\t'): Escaped += TEXT("\\t");  break;
			case TEXT('\r'): break;                          // fold CRLF to the \n above
			default:         Escaped.AppendChar(Ch); break;
		}
	}
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}

bool ConvaiContextFormat::ValueMatchesPromptLiteral(
	const FString& Candidate, const FString& RawValue)
{
	return Candidate.Equals(RawValue, ESearchCase::IgnoreCase)
		|| Candidate.Equals(FormatValueForPrompt(RawValue), ESearchCase::IgnoreCase);
}

bool ConvaiContextFormat::WouldWatchMatch(const FString& TargetValue,
	const FString& ExistingValue, const FString& NewValue,
	bool bObservedTargetDeparture)
{
	if (TargetValue.IsEmpty())
	{
		return ExistingValue != NewValue;
	}
	return ValueMatchesPromptLiteral(TargetValue, NewValue)
		&& (ExistingValue != NewValue || bObservedTargetDeparture);
}

bool ConvaiContextFormat::IsTargetWatchDeparture(const FString& TargetValue,
	const FString& ExistingValue, const FString& NewValue)
{
	return !TargetValue.IsEmpty() && ExistingValue != NewValue
		&& !ValueMatchesPromptLiteral(TargetValue, NewValue);
}
