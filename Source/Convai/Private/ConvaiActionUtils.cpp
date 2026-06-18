// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "Internationalization/Regex.h"
#include "ConvaiDefinitions.h"
#include "Environment/ConvaiEnvironment.h"

DEFINE_LOG_CATEGORY(ConvaiActionUtilsLog);

namespace
{
	bool FindCharAfterIndex(const FString& SearchString, const char Search, int32& OutIndex, const int32& AfterIndex)
	{
		return SearchString.Left(AfterIndex).FindChar(Search, OutIndex);
	}

	FString RemoveQuotedWords(const FString& Input)
	{

		int32 QuoteStartIndex = -1;
		if (!Input.FindChar('"', QuoteStartIndex))
		{
			return Input;
		}

		FString Result = Input;

		while (Result.FindChar('"', QuoteStartIndex))
		{
			int32 QuoteEndIndex = -1;

			// Start searching for the end quote after the start quote
			if (Result.Left(QuoteStartIndex + 1).FindChar('"', QuoteEndIndex) && QuoteEndIndex > QuoteStartIndex)
			{
				// Remove the quoted string, including the quotes
				Result.RemoveAt(QuoteStartIndex, QuoteEndIndex - QuoteStartIndex + 1);
			}
			else
			{
				// No closing quote found after this point, break the loop
				break;
			}
		}

		return Result;
	}

	int32 CountWords(const FString& str)
	{
		if (str.Len() == 0) return 0;

		TArray<FString> words;
		str.ParseIntoArray(words, TEXT(" "), true);

		return words.Num();
	}

	FString KeepNWords(const FString& StringToBeParsed, const FString& CandidateString)
	{
		TArray<FString> words;
		StringToBeParsed.ParseIntoArray(words, TEXT(" "), true);

		int32 N = CountWords(CandidateString);

		FString result;
		for (int32 i = 0; i < FMath::Min(N, words.Num()); i++)
		{
			result += words[i] + " ";
		}

		// Trim trailing space
		result = result.LeftChop(1);

		return result;
	}

	FString FindClosestString(FString Input, const TArray<FString>& StringArray)
	{
		FString ClosestString;
		int32 MinDistance = MAX_int32;

		for (auto& String : StringArray)
		{
			int32 Distance = UConvaiUtils::LevenshteinDistance(Input, String);
			if (Distance < MinDistance)
			{
				MinDistance = Distance;
				ClosestString = String;
			}
		}

		return ClosestString;
	}

	// Helper function to check if a substring is found outside of quotes.
	bool FindSubstringOutsideQuotes(const FString& SearchString, const FString& SubstringToFind)
	{
		bool bInsideQuotes = false;
		int32 SubstringLength = SubstringToFind.Len();
		int32 SearchStringLength = SearchString.Len();

		if (SubstringLength > SearchStringLength || SubstringLength == 0)
		{
			return false;
		}

		for (int32 i = 0; i <= SearchStringLength - SubstringLength; ++i)
		{
			// Toggle the inside quotes flag if a quote is found
			if (SearchString[i] == '"')
			{
				bInsideQuotes = !bInsideQuotes;
				continue;
			}

			// If we are not inside quotes, perform the search
			if (!bInsideQuotes && SearchString.Mid(i, SubstringLength).Equals(SubstringToFind, ESearchCase::IgnoreCase))
			{
				// Make sure the match is not part of a larger string
				if ((i == 0 || !FChar::IsAlnum(SearchString[i - 1])) &&
					(i + SubstringLength == SearchStringLength || !FChar::IsAlnum(SearchString[i + SubstringLength])))
				{
					return true; // Substring found outside of quotes
				}
			}
		}

		return false; // Substring not found or is within quotes
	}

	bool FindClosePhraseOutsideQuotes(const FString& SearchString, const FString& PhraseToFind, int& OutBestDistance, int NumWordsToSkip = 0)
	{
		if (PhraseToFind.IsEmpty())
		{
			return false; // Can't find an empty phrase.
		}

		int32 MaxLevenshteinDistance = FMath::Clamp(PhraseToFind.Len() / 2, 2, 4);
		TArray<FString> Words;
		FString CurrentWord;
		bool bInsideQuotes = false;
		OutBestDistance = MaxLevenshteinDistance + 1; // Initialize with a value above the limit

		// Collect words outside quotes
		for (TCHAR Char : SearchString)
		{
			if (Char == '"')
			{
				bInsideQuotes = !bInsideQuotes;
				if (!CurrentWord.IsEmpty())
				{
					Words.Add(CurrentWord);
					CurrentWord.Empty();
				}
				continue;
			}

			if (!bInsideQuotes && (Char == ' ' || Char == '\t'))
			{
				if (!CurrentWord.IsEmpty())
				{
					Words.Add(CurrentWord);
					CurrentWord.Empty();
				}
			}
			else if (!bInsideQuotes)
			{
				CurrentWord.AppendChar(Char);
			}
		}

		// Add the last word if there is one
		if (!CurrentWord.IsEmpty())
		{
			Words.Add(CurrentWord);
		}

		// Number of words in the phrase to find
		TArray<FString> PhraseWords;
		PhraseToFind.ParseIntoArray(PhraseWords, TEXT(" "), true);
		int32 NumWordsInPhrase = PhraseWords.Num();

		// Sliding window of words to check for the phrase
		for (int32 i = NumWordsToSkip; i <= Words.Num() - NumWordsInPhrase; ++i)
		{
			FString WindowString;
			for (int32 j = 0; j < NumWordsInPhrase; ++j)
			{
				WindowString += (j > 0 ? TEXT(" ") : TEXT("")) + Words[i + j];
			}

			if (WindowString.Len() - PhraseToFind.Len() >= MaxLevenshteinDistance || PhraseToFind.Len() - WindowString.Len() >= MaxLevenshteinDistance)
			{
				continue;
			}

			int32 Distance = UConvaiUtils::LevenshteinDistance(WindowString, PhraseToFind);
			if (Distance <= MaxLevenshteinDistance && Distance < OutBestDistance)
			{
				OutBestDistance = Distance;
			}
		}

		// The function returns true if we found a match within the acceptable Levenshtein distance
		return OutBestDistance <= MaxLevenshteinDistance;
	}

	static bool FindNearestObjectByName(FString SearchString, TArray<FConvaiObjectEntry> Objects, FConvaiObjectEntry& ObjectMatch)
	{
		FString SearchStringLower = SearchString.ToLower();
		bool Found = false;
		int BestDistance = 100;
		for (auto o : Objects)
		{
			FString ObjectNameLower = o.Name.ToLower();
			int Distance = 0;
			if (FindClosePhraseOutsideQuotes(SearchStringLower, ObjectNameLower, Distance))
			{
				if (Distance < BestDistance)
				{
					ObjectMatch = o;
					BestDistance = Distance;
					Found = true;
				}
			}
		}

		// Try to search in objects using each word in the SearchString
		if (!Found)
		{
			TArray<FString> words;
			FString SearchStringWithoutQuotes = RemoveQuotedWords(SearchStringLower);
			SearchStringWithoutQuotes.ParseIntoArray(words, TEXT(" "), true);
			bool breakFromLoop = false;
			for (auto o : Objects)
			{
				if (breakFromLoop)
					break;
				if (CountWords(o.Name) <= 1)
					continue; // consider only names consisting of 1+ words

				FString ObjectNameLower = o.Name.ToLower();
				for (auto w : words)
				{
					if (w.Len() <= 3)
						continue; // consider only words greater than 3 letters

					int Distance = 0;
					if (FindClosePhraseOutsideQuotes(ObjectNameLower, w, Distance))
					{
						ObjectMatch = o;
						BestDistance = Distance;
						Found = true;
						breakFromLoop = true;
						break;
					}
				}
			}
		}

		return Found;
	}

	TArray<FConvaiObjectEntry> BubbleSortEntriesByNumberOfWords(TArray<FConvaiObjectEntry> Entries) 
	{
		bool swapped;
		int n = Entries.Num();
		for (int i = 0; i < n - 1; i++) {
			swapped = false;
			for (int j = 0; j < n - i - 1; j++) {
				if (CountWords(Entries[j].Name) > CountWords(Entries[j+1].Name)) {
					Entries.Swap(j, j + 1);
					swapped = true;
				}
			}
			// If no two elements were swapped by inner loop, then break
			if (!swapped) {
				break;
			}
		}
		return Entries;
	}
};

TArray<FString> UConvaiActions::SmartSplit(const FString& SequenceString)
{
	TArray<FString> Result;
	FString CurrentString = "";
	bool InQuotes = false;

	for (int i = 0; i < SequenceString.Len(); ++i)
	{
		TCHAR CurrentChar = SequenceString[i];

		if (CurrentChar == '\"')
		{
			InQuotes = !InQuotes;
		}

		if (CurrentChar == ',' && !InQuotes)
		{
			Result.Add(CurrentString.TrimStartAndEnd());
			CurrentString = "";
		}
		else
		{
			CurrentString += CurrentChar;
		}
	}

	if (!CurrentString.IsEmpty())
	{
		Result.Add(CurrentString.TrimStartAndEnd());
	}

	return Result;
}

FString UConvaiActions::ExtractText(const FString& ActionResult)
{
	// Match a double-quoted substring first ("..."), fall back to single-quoted ('...').
	// Strip the surrounding quote chars (LeftChop+RightChop trim 1 char from each end).
	{
		const FRegexPattern DoublePattern(TEXT("\"[^\"]*\""));
		FRegexMatcher Matcher(DoublePattern, ActionResult);
		if (Matcher.FindNext())
		{
			return Matcher.GetCaptureGroup(0).LeftChop(1).RightChop(1);
		}
	}
	{
		const FRegexPattern SinglePattern(TEXT("'[^']*'"));
		FRegexMatcher Matcher(SinglePattern, ActionResult);
		if (Matcher.FindNext())
		{
			return Matcher.GetCaptureGroup(0).LeftChop(1).RightChop(1);
		}
	}
	return FString();
}

float UConvaiActions::ExtractNumber(const FString& ActionResult)
{
	float ExtraNumber = 0;
	const FRegexPattern NumericPattern(TEXT("\\d+"));
	FRegexMatcher NumberMatcher(NumericPattern, ActionResult);
	if (NumberMatcher.FindNext())
	{
		ExtraNumber = FCString::Atof(*NumberMatcher.GetCaptureGroup(0));
	}
	return ExtraNumber;
}

FString UConvaiActions::RemoveDesc(FString str)
{
	FRegexPattern DescPattern(TEXT("<.*>"));
	FRegexMatcher DescMatcher = FRegexMatcher(DescPattern, str);
	int i = 0;
	while (DescMatcher.FindNext())
	{
		int start = DescMatcher.GetMatchBeginning();
		str.ReplaceInline(*DescMatcher.GetCaptureGroup(i++), *FString(""));
		str.TrimEndInline();
		str.TrimStartInline();
	}
	//CONVAI_LOG(ConvaiActionUtilsLog, Warning, TEXT("str:%s"), *str);
	return str;
}

FString UConvaiActions::FindAction(FString ActionToBeParsed, TArray<FString> Actions)
{
	FString ClosestAction = "None";
	int32 MinDistance = 4;
	for (auto a : Actions)
	{
		FString TrimmedAction = ActionToBeParsed;
		a = RemoveDesc(a);
		TrimmedAction = KeepNWords(ActionToBeParsed, a);
		int32 Distance = UConvaiUtils::LevenshteinDistance(TrimmedAction, a);
		if (Distance < MinDistance)
		{
			MinDistance = Distance;
			ClosestAction = a;
		}
	}
	return ClosestAction;
}

const FConvaiAction* UConvaiActions::FindActionTemplate(const FString& FilledIn, const TArray<FConvaiAction>& Templates)
{
	// Mirrors the Levenshtein logic in FindAction but keyed by FConvaiAction.Name.
	const FConvaiAction* Best = nullptr;
	int32 MinDistance = 4;
	for (const FConvaiAction& T : Templates)
	{
		if (T.Name.IsEmpty())
		{
			continue;
		}
		const FString Window = KeepNWords(FilledIn, T.Name);
		const int32 Distance = UConvaiUtils::LevenshteinDistance(Window, T.Name);
		if (Distance < MinDistance)
		{
			MinDistance = Distance;
			Best = &T;
		}
	}
	return Best;
}

FString UConvaiActions::StripActionPrefix(const FString& FilledIn, const FString& CanonicalName)
{
	if (CanonicalName.IsEmpty())
	{
		return FilledIn;
	}
	const FString Trimmed = FilledIn.TrimStartAndEnd();
	if (Trimmed.Len() < CanonicalName.Len() ||
		!Trimmed.Left(CanonicalName.Len()).Equals(CanonicalName, ESearchCase::IgnoreCase))
	{
		return Trimmed;
	}
	return Trimmed.RightChop(CanonicalName.Len()).TrimStartAndEnd();
}

TArray<FString> UConvaiActions::SplitParamValues(const FString& Blob, int32 ExpectedCount)
{
	TArray<FString> Out;
	const FString Trimmed = Blob.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || ExpectedCount <= 0)
	{
		return Out;
	}

	// Brace-aware first: the prompt now teaches the LLM to wrap values as {v1} {v2} ...
	// When at least one brace segment exists, treat that set as authoritative.
	{
		const FRegexPattern Pattern(TEXT("\\{([^}]*)\\}"));
		FRegexMatcher Matcher(Pattern, Trimmed);
		while (Matcher.FindNext())
		{
			Out.Add(Matcher.GetCaptureGroup(1));
		}
		if (Out.Num() > 0)
		{
			return Out;
		}
	}

	// Quote-aware fallback: legacy / fine-tuned models may still emit "v1" "v2" ...
	{
		const FRegexPattern Pattern(TEXT("\"([^\"]*)\""));
		FRegexMatcher Matcher(Pattern, Trimmed);
		while (Matcher.FindNext())
		{
			Out.Add(Matcher.GetCaptureGroup(1));
		}
		if (Out.Num() > 0)
		{
			return Out;
		}
	}

	// Whitespace fallback. Single expected slot → whole blob (preserve multi-word values).
	if (ExpectedCount == 1)
	{
		Out.Add(Trimmed);
		return Out;
	}

	// Multi-slot: split into N tokens, last one absorbs trailing tokens.
	TArray<FString> Tokens;
	Trimmed.ParseIntoArrayWS(Tokens);
	if (Tokens.Num() <= ExpectedCount)
	{
		return Tokens;
	}
	for (int32 i = 0; i < ExpectedCount - 1; ++i)
	{
		Out.Add(Tokens[i]);
	}
	FString Tail;
	for (int32 i = ExpectedCount - 1; i < Tokens.Num(); ++i)
	{
		if (!Tail.IsEmpty()) Tail += TEXT(" ");
		Tail += Tokens[i];
	}
	Out.Add(Tail);
	return Out;
}

TArray<FString> UConvaiActions::SplitParamValues(const FString& Blob, const TArray<FConvaiActionParam>& Params)
{
	const int32 N = Params.Num();
	TArray<FString> Out;
	const FString Trimmed = Blob.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || N == 0)
	{
		Out.SetNum(N);
		return Out;
	}

	// Pass 1: brace-wrapped {v1} {v2}.
	{
		const FRegexPattern Pattern(TEXT("\\{([^}]*)\\}"));
		FRegexMatcher Matcher(Pattern, Trimmed);
		while (Matcher.FindNext())
		{
			Out.Add(Matcher.GetCaptureGroup(1));
		}
		if (Out.Num() > 0)
		{
			Out.SetNum(N);
			return Out;
		}
	}

	// Pass 2: named-key — find each "<paramName>:" as an anchor and carve out the value
	// up to the next anchor (or end of blob). Word-boundary check on the left so
	// "name:" doesn't match inside "surname:".
	{
		struct FAnchor { int32 NameIdx; int32 StartIdx; int32 ValueIdx; };
		TArray<FAnchor> Anchors;
		for (int32 i = 0; i < N; ++i)
		{
			const FString& PName = Params[i].Name;
			if (PName.IsEmpty()) continue;
			const FString Needle = PName + TEXT(":");
			int32 Idx = Trimmed.Find(Needle, ESearchCase::IgnoreCase);
			while (Idx != INDEX_NONE)
			{
				const bool bWordStart = (Idx == 0) || !FChar::IsAlnum(Trimmed[Idx - 1]);
				if (bWordStart) break;
				Idx = Trimmed.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart, Idx + 1);
			}
			if (Idx == INDEX_NONE) continue;
			Anchors.Add({ i, Idx, Idx + Needle.Len() });
		}

		if (Anchors.Num() > 0)
		{
			Anchors.Sort([](const FAnchor& A, const FAnchor& B) { return A.StartIdx < B.StartIdx; });

			Out.SetNum(N);
			for (int32 a = 0; a < Anchors.Num(); ++a)
			{
				const int32 ValStart = Anchors[a].ValueIdx;
				const int32 ValEnd   = (a + 1 < Anchors.Num()) ? Anchors[a + 1].StartIdx : Trimmed.Len();
				FString Value = Trimmed.Mid(ValStart, ValEnd - ValStart).TrimStartAndEnd();

				// Trim a trailing connector (next param's Connector) or a common conjunction.
				if (a + 1 < Anchors.Num())
				{
					const int32 NextNameIdx = Anchors[a + 1].NameIdx;
					const FString& NextConn = Params[NextNameIdx].Connector;
					bool bStripped = false;
					if (!NextConn.IsEmpty())
					{
						const FString Suffix = TEXT(" ") + NextConn;
						if (Value.EndsWith(Suffix, ESearchCase::IgnoreCase))
						{
							Value = Value.LeftChop(Suffix.Len()).TrimEnd();
							bStripped = true;
						}
					}
					if (!bStripped)
					{
						static const TArray<FString> Fillers = { TEXT(" and"), TEXT(" or"), TEXT(",") };
						for (const FString& F : Fillers)
						{
							if (Value.EndsWith(F, ESearchCase::IgnoreCase))
							{
								Value = Value.LeftChop(F.Len()).TrimEnd();
								break;
							}
						}
					}
				}
				Out[Anchors[a].NameIdx] = Value;
			}
			return Out;
		}
	}

	// Pass 3: connector-as-separator — when every inter-param Connector is set, use them
	// to carve the blob (e.g. template `Put {ball} on {table}` → response `ball on table`).
	if (N >= 2)
	{
		bool bAllConnectorsSet = true;
		for (int32 i = 1; i < N; ++i)
		{
			if (Params[i].Connector.IsEmpty()) { bAllConnectorsSet = false; break; }
		}
		if (bAllConnectorsSet)
		{
			TArray<FString> Carved;
			Carved.Reserve(N);
			FString Remaining = Trimmed;
			bool bAllFound = true;
			for (int32 i = 1; i < N; ++i)
			{
				const FString Sep = TEXT(" ") + Params[i].Connector + TEXT(" ");
				const int32 Idx = Remaining.Find(Sep, ESearchCase::IgnoreCase);
				if (Idx == INDEX_NONE) { bAllFound = false; break; }
				Carved.Add(Remaining.Left(Idx).TrimStartAndEnd());
				Remaining = Remaining.RightChop(Idx + Sep.Len());
			}
			if (bAllFound)
			{
				Carved.Add(Remaining.TrimStartAndEnd());
				Carved.SetNum(N);
				return Carved;
			}
		}
	}

	// Pass 4 + 5: delegate to positional splitter (quotes, then whitespace).
	Out = SplitParamValues(Trimmed, N);
	Out.SetNum(N);
	return Out;
}

FString UConvaiActions::StripParamNameMimicry(const FString& Value, const FString& ParamName)
{
	FString Working = Value.TrimStartAndEnd();
	if (Working.IsEmpty())
	{
		return Working;
	}

	// 1. Strip every [choices] block. The LLM may echo the prompt's `[a|b|c]` literal.
	{
		const FRegexPattern Pattern(TEXT("\\[[^\\]]*\\]"));
		FRegexMatcher Matcher(Pattern, Working);
		while (Matcher.FindNext())
		{
			Working = (Working.Left(Matcher.GetMatchBeginning())
				+ Working.RightChop(Matcher.GetMatchEnding())).TrimStartAndEnd();
			Matcher = FRegexMatcher(Pattern, Working);
		}
	}

	// 2. Split on ':' and drop tokens that are mimicry artifacts (param name / type word).
	if (!Working.Contains(TEXT(":")))
	{
		return Working;
	}

	TArray<FString> Tokens;
	Working.ParseIntoArray(Tokens, TEXT(":"), false);

	static const TSet<FString> TypeWords = {
		TEXT("ref"), TEXT("string"), TEXT("number"), TEXT("bool"), TEXT("enum")
	};

	TArray<FString> Kept;
	Kept.Reserve(Tokens.Num());
	for (FString& T : Tokens)
	{
		T.TrimStartAndEndInline();
		if (T.IsEmpty())
		{
			continue;
		}
		if (!ParamName.IsEmpty() && T.Equals(ParamName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (TypeWords.Contains(T.ToLower()))
		{
			continue;
		}
		Kept.Add(T);
	}

	// Nothing left after stripping → fall back to the cleaned-but-not-split form.
	FString Result = (Kept.Num() == 0) ? Working : FString::Join(Kept, TEXT(":"));

	// 3. Leading-prefix-word strip — the LLM sometimes abbreviates the param name as
	//    a label inside the value (e.g. password → "pass"): "pass kk 111 kkkk".
	//    Conservative heuristic to avoid false positives: only fires when ParamName
	//    is >=6 chars (so short names like "name"/"id" are exempt), the first word
	//    is >=3 chars, and that word is a case-insensitive prefix of ParamName.
	if (!ParamName.IsEmpty() && ParamName.Len() >= 6)
	{
		int32 SpaceIdx = INDEX_NONE;
		if (Result.FindChar(TEXT(' '), SpaceIdx) && SpaceIdx >= 3)
		{
			const FString FirstWord = Result.Left(SpaceIdx);
			if (ParamName.Len() >= FirstWord.Len() &&
				ParamName.Left(FirstWord.Len()).Equals(FirstWord, ESearchCase::IgnoreCase))
			{
				Result = Result.RightChop(SpaceIdx + 1).TrimStart();
			}
		}
	}
	return Result;
}

namespace
{
	bool ParseBoolish(const FString& Value, bool& OutMatched)
	{
		const FString L = Value.TrimStartAndEnd().ToLower();
		if (L == TEXT("true") || L == TEXT("yes") || L == TEXT("1"))
		{
			OutMatched = true; return true;
		}
		if (L == TEXT("false") || L == TEXT("no") || L == TEXT("0"))
		{
			OutMatched = true; return false;
		}
		OutMatched = false; return false;
	}

	bool ValueLooksFullyNumeric(const FString& Value)
	{
		const FString T = Value.TrimStartAndEnd();
		if (T.IsEmpty()) return false;
		bool bSeenDigit = false;
		bool bSeenDot   = false;
		for (int32 i = 0; i < T.Len(); ++i)
		{
			const TCHAR C = T[i];
			if (i == 0 && (C == TEXT('-') || C == TEXT('+'))) { continue; }
			if (FChar::IsDigit(C)) { bSeenDigit = true; continue; }
			if (C == TEXT('.') && !bSeenDot) { bSeenDot = true; continue; }
			return false;
		}
		return bSeenDigit;
	}
}

TArray<FString> UConvaiActions::GetEnumLabels(const UEnum* EnumType)
{
	// Must stay in lockstep with what the prompt actually exposes to the LLM —
	// otherwise we'd accept values for enum entries the LLM was never told about.
	TArray<FString> Labels;
	if (!EnumType)
	{
		return Labels;
	}
	const int32 Max = EnumType->NumEnums();
	Labels.Reserve(Max);
	for (int32 i = 0; i < Max; ++i)
	{
		// Skip the auto-generated _MAX sentinel UE adds at the end. Conditional on
		// ContainsExistingMax() because enums without UENUM-style _MAX (e.g. raw enums
		// that flow through here) shouldn't lose their last legitimate value.
		if (i == Max - 1 && EnumType->ContainsExistingMax())
		{
			continue;
		}
#if WITH_EDITORONLY_DATA
		// UEnum::HasMetaData is editor-only (gated by WITH_EDITORONLY_DATA on 5.0-5.4
		// and WITH_METADATA on 5.5+, both of which compile out in cooked/runtime builds).
		if (EnumType->HasMetaData(TEXT("Hidden"), i))
		{
			continue;
		}
#endif
		FString Label = EnumType->GetDisplayNameTextByIndex(i).ToString();
		if (Label.IsEmpty()) { Label = EnumType->GetNameStringByIndex(i); }
		Labels.Add(MoveTemp(Label));
	}
	return Labels;
}

int32 UConvaiActions::FuzzyFindLabel(const FString& Query, const TArray<FString>& Labels, int32& OutDistance)
{
	OutDistance = 0;
	if (Query.IsEmpty() || Labels.Num() == 0)
	{
		return INDEX_NONE;
	}

	for (int32 i = 0; i < Labels.Num(); ++i)
	{
		if (Labels[i].Equals(Query, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}

	// Fuzzy: LLMs sometimes return paraphrased or mis-cased labels
	// (e.g. "rotating clockwise" vs "RotatingClockwise"). Accept the closest
	// candidate within a length-relative Levenshtein bound.
	const FString QueryLower = Query.ToLower();
	// LevenshteinDistance uses substitution cost 2 (see ConvaiUtils.cpp), so
	// MaxDistance>=2 already allows one substitution. For very short queries
	// (<4 chars) that means a quarter of the string can change and still
	// match — produces false positives like "Go"->"Do"/"To"/"No". Require
	// exact match for those.
	const int32 MaxDistance = QueryLower.Len() < 4 ? 0 : FMath::Clamp(QueryLower.Len() / 2, 2, 4);
	int32 BestIndex = INDEX_NONE;
	int32 BestDistance = MaxDistance + 1;
	for (int32 i = 0; i < Labels.Num(); ++i)
	{
		const int32 D = UConvaiUtils::LevenshteinDistance(QueryLower, Labels[i].ToLower());
		if (D < BestDistance)
		{
			BestDistance = D;
			BestIndex = i;
		}
	}
	if (BestIndex != INDEX_NONE && BestDistance <= MaxDistance)
	{
		OutDistance = BestDistance;
		return BestIndex;
	}

	// Substring fallback: LLMs sometimes glue extra tokens onto a valid choice
	// ("wave:User", "nod:\"appreciative\"") which pushes Levenshtein past MaxDistance
	// even though the intended label is right there. Accept a label that appears as a
	// whole word inside Query. Word-boundary discipline (no raw Contains) does the
	// real safety work — verb conjugations like "nodding"/"nodded" and embedded
	// occurrences like "anode"/"goodbye" all fail the boundary check. MinLen 3 still
	// keeps two-letter bigrams like "no"/"up"/"on" out, where the bigram coincides
	// with too many common English words to trust even with boundaries.
	// Tie-break: longest label, then earliest position, then first-declared.
	constexpr int32 MinSubstringLen = 3;
	auto IsWordChar = [](TCHAR C) { return FChar::IsAlnum(C) || FChar::IsUnderscore(C); };

	int32 SubBestIndex = INDEX_NONE;
	int32 SubBestLen   = -1;
	int32 SubBestPos   = MAX_int32;
	for (int32 i = 0; i < Labels.Num(); ++i)
	{
		const FString& Label = Labels[i];
		if (Label.Len() < MinSubstringLen) { continue; }
		const FString LabelLower = Label.ToLower();

		int32 SearchFrom = 0;
		while (SearchFrom + LabelLower.Len() <= QueryLower.Len())
		{
			const int32 Pos = QueryLower.Find(LabelLower, ESearchCase::CaseSensitive,
											   ESearchDir::FromStart, SearchFrom);
			if (Pos == INDEX_NONE) { break; }

			const bool bLeftOk  = (Pos == 0) || !IsWordChar(QueryLower[Pos - 1]);
			const int32 RightAt = Pos + LabelLower.Len();
			const bool bRightOk = (RightAt >= QueryLower.Len()) || !IsWordChar(QueryLower[RightAt]);

			if (bLeftOk && bRightOk)
			{
				const bool bBetter =
					Label.Len() > SubBestLen ||
					(Label.Len() == SubBestLen && Pos < SubBestPos);
				if (bBetter)
				{
					SubBestIndex = i;
					SubBestLen   = Label.Len();
					SubBestPos   = Pos;
				}
				break; // first whole-word hit per label is enough
			}
			SearchFrom = Pos + 1;
		}
	}

	if (SubBestIndex != INDEX_NONE)
	{
		OutDistance = 0;
		return SubBestIndex;
	}
	return INDEX_NONE;
}

FConvaiResultParam UConvaiActions::CoerceParam(const FString& Value,
	EConvaiActionParamType DeclaredType, const FConvaiEnvironmentData& Env,
	const UEnum* EnumType, const TArray<FString>* Choices, bool* bOutConstraintMatched)
{
	FConvaiResultParam P;
	if (bOutConstraintMatched) { *bOutConstraintMatched = true; }

	// Strip surrounding quotes (either kind) so the user-visible string isn't noisy.
	FString Clean = Value.TrimStartAndEnd();
	if (Clean.Len() >= 2)
	{
		const TCHAR First = Clean[0];
		const TCHAR Last  = Clean[Clean.Len() - 1];
		if ((First == TEXT('"') && Last == TEXT('"')) ||
			(First == TEXT('\'') && Last == TEXT('\'')))
		{
			Clean = Clean.Mid(1, Clean.Len() - 2);
		}
	}

	// Always populate every value field — designers can read whichever interpretation suits.
	P.StringValue = Clean;
	P.NumberValue = FCString::Atof(*Clean);

	bool bBoolMatched = false;
	P.BoolValue = ParseBoolish(Clean, bBoolMatched);

	// Resolve against env objects then characters. RefreshSnapshot reads live
	// actor/component transforms and bounds, which are game-thread-only — this
	// parse path is invoked from the WebRTC data callback which can land on a
	// transport thread (see UConvaiSubsystem::OnDataPacketReceived). Only run
	// the resolver eagerly when we're already on the game thread; otherwise
	// designers' first call to "Resolve Goal Location" from BP will do the resolve
	// on the game thread.
	const bool bSafeToResolve = IsInGameThread();
	if (const FConvaiObjectEntry* Obj = Env.FindObject(Clean))
	{
		P.RefValue = *Obj;
		if (bSafeToResolve) { P.RefValue.RefreshSnapshot(); }
	}
	else if (const FConvaiObjectEntry* Chr = Env.FindCharacter(Clean))
	{
		P.RefValue = *Chr;
		if (bSafeToResolve) { P.RefValue.RefreshSnapshot(); }
	}

	// Type set: pass through declared type unless Auto, in which case infer from
	// what coerced cleanly (Reference > Number > Bool > String).
	if (DeclaredType != EConvaiActionParamType::Auto)
	{
		P.Type = DeclaredType;
	}
	else
	{
		if (!P.RefValue.Name.IsEmpty())          { P.Type = EConvaiActionParamType::Reference; }
		else if (ValueLooksFullyNumeric(Clean))  { P.Type = EConvaiActionParamType::Number; }
		else if (bBoolMatched)                   { P.Type = EConvaiActionParamType::Bool; }
		else                                     { P.Type = EConvaiActionParamType::String; }
	}

	// Constraint validation: enum byte resolution when the template declared an EnumType
	// (ByteValue holds the underlying enum value on match so handlers can Byte-to-Enum<…>
	// in BP), otherwise a Choices fuzzy match when the caller supplied a list. Either path
	// flips bOutConstraintMatched to false on miss so the caller can warn with full
	// action/param context — we don't drop the value, just signal.
	// Enum/Choices are declared constraints — on match, canonicalize StringValue to the
	// declared label so handlers see a value that's actually in the set (avoids casing /
	// paraphrase drift like "rotating clockwise" vs "RotatingClockwise"). On miss the raw
	// LLM string passes through and the caller is signaled via bOutConstraintMatched.
	if (DeclaredType == EConvaiActionParamType::Enum && EnumType != nullptr)
	{
		const TArray<FString> Labels = UConvaiActions::GetEnumLabels(EnumType);
		int32 Distance = 0;
		const int32 Idx = UConvaiActions::FuzzyFindLabel(Clean, Labels, Distance);
		if (Idx != INDEX_NONE)
		{
			P.ByteValue = static_cast<uint8>(EnumType->GetValueByIndex(Idx));
			P.StringValue = Labels[Idx];
		}
		else if (bOutConstraintMatched)
		{
			*bOutConstraintMatched = false;
		}
	}
	else if (Choices && Choices->Num() > 0)
	{
		int32 Distance = 0;
		const int32 Idx = UConvaiActions::FuzzyFindLabel(Clean, *Choices, Distance);
		if (Idx != INDEX_NONE)
		{
			P.StringValue = (*Choices)[Idx];
		}
		else if (bOutConstraintMatched)
		{
			*bOutConstraintMatched = false;
		}
	}

	return P;
}

// ── BP accessors for FConvaiResultAction.Parameters ──────────────────

FConvaiResultParam UConvaiActions::GetFirstParam(const FConvaiResultAction& Action)
{
	// Return the first parameter in insertion order. Use an iterator rather than a
	// range-for with an immediate return: the latter has an unreachable loop increment,
	// which Android/Clang flags as an error (-Wunreachable-code-loop-increment, promoted
	// to -Werror on UE 5.8). MSVC/Win64 is unaffected either way.
	auto It = Action.Parameters.CreateConstIterator();
	if (It)
	{
		return It->Value;
	}
	return FConvaiResultParam();
}

FConvaiResultParam UConvaiActions::GetParam(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return *Found;
	}
	return FConvaiResultParam();
}

EConvaiActionParamType UConvaiActions::GetParamType(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return Found->Type;
	}
	return EConvaiActionParamType::String;
}

FString UConvaiActions::GetParamAsString(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return Found->StringValue;
	}
	return FString();
}

float UConvaiActions::GetParamAsNumber(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return Found->NumberValue;
	}
	return 0.f;
}

bool UConvaiActions::GetParamAsBool(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return Found->BoolValue;
	}
	return false;
}

FConvaiObjectEntry UConvaiActions::GetParamAsRef(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return Found->RefValue;
	}
	return FConvaiObjectEntry();
}

uint8 UConvaiActions::GetParamAsByte(const FConvaiResultAction& Action, const FString& Name)
{
	if (const FConvaiResultParam* Found = Action.Parameters.Find(Name))
	{
		return Found->ByteValue;
	}
	return 0;
}

bool UConvaiActions::HasParam(const FConvaiResultAction& Action, const FString& Name)
{
	return Action.Parameters.Contains(Name);
}

void UConvaiActions::ResolveGoalLocation(FConvaiObjectEntry& Entry,
	AActor* SourceActor,
	AActor*& OutGoalActor, USceneComponent*& OutGoalComponent,
	FVector& OutGoalLocation, float& OutAcceptanceRadius,
	EConvaiMoveTarget& OutMode, bool& bOutSuccess,
	bool& bOutAlreadyThere,
	bool& bOutReachable, FVector& OutPathEndPoint, TArray<FVector>& OutPathPoints,
	bool bForceRefresh)
{
	// Thin BP wrapper — the struct method owns all the logic (position math +
	// Actor→Vector promotion on Step Onto Bounds + arrival check + reachability
	// path query).
	Entry.ResolveGoalLocation(SourceActor, bForceRefresh,
		OutGoalActor, OutGoalComponent, OutGoalLocation, OutAcceptanceRadius,
		OutMode, bOutSuccess,
		bOutAlreadyThere, bOutReachable, OutPathEndPoint, OutPathPoints);
}

// ── DEPRECATED — kept for back-compat with prior-iteration BP graphs ──

FString UConvaiActions::GetActionParam(const FConvaiExtraParams& Params, const FString& Name)
{
	const FString* Found = Params.NamedParams.Find(Name);
	return Found ? *Found : FString();
}

float UConvaiActions::GetActionParamAsNumber(const FConvaiExtraParams& Params, const FString& Name)
{
	const FString* Found = Params.NamedParams.Find(Name);
	if (!Found) { return 0.f; }
	return FCString::Atof(**Found);
}

bool UConvaiActions::HasActionParam(const FConvaiExtraParams& Params, const FString& Name)
{
	return Params.NamedParams.Contains(Name);
}
