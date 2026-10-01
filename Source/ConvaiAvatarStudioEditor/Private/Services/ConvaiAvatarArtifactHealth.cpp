// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarArtifactHealth.h"
#include "Services/ConvaiAvatarAssetsClient.h"

namespace
{
	bool EngineNumber(const FString& Text, int32& Major, int32& Minor)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() < 2 || Parts.Num() > 3) return false;
		TArray<int32> Values;
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || Part.Len() > 5) return false;
			int32 Value = 0;
			for (TCHAR C : Part) { if (C < TEXT('0') || C > TEXT('9')) return false; Value = Value * 10 + C - TEXT('0'); }
			Values.Add(Value);
		}
		if (Values[0] == 0) return false;
		Major = Values[0]; Minor = Values[1];
		return true;
	}
	bool VersionNumber(const FString& Version, const FString& Platform, int32& Major, int32& Minor)
	{
		const FString Suffix = TEXT("-") + Platform;
		if (!Version.StartsWith(TEXT("ue-"), ESearchCase::CaseSensitive) || !Version.EndsWith(Suffix, ESearchCase::CaseSensitive)) return false;
		const FString Engine = Version.Mid(3, Version.Len() - 3 - Suffix.Len());
		// Artifact keys are generated from major.minor. Patch numbers describe an engine installation only.
		TArray<FString> Parts; Engine.ParseIntoArray(Parts, TEXT("."), false);
		return Parts.Num() == 2 && EngineNumber(Engine, Major, Minor);
	}
	FString EngineLabel(const FString& Version)
	{ return Version.Mid(3, Version.Len() - 3 - FCString::Strlen(TEXT("-Windows"))); }
	EConvaiAvatarArtifactPresence Presence(const FConvaiAvatarAsset& Asset, const FString& Version)
	{
		if (const auto* Info = Asset.VersionInfo.Find(Version)) return Info->Presence;
		return Asset.bHasVersionInventory && !Asset.Versions.Contains(Version) ? EConvaiAvatarArtifactPresence::Missing : EConvaiAvatarArtifactPresence::Unknown;
	}
	FString Latest(const FConvaiAvatarAsset& Asset, const FString& Platform)
	{
		FString Result;
		int32 BestMajor = -1, BestMinor = -1;
		for (const auto& Pair : Asset.VersionInfo)
		{
			int32 Major = 0, Minor = 0;
			if (Pair.Value.Presence != EConvaiAvatarArtifactPresence::Available || !VersionNumber(Pair.Key, Platform, Major, Minor)) continue;
			if (Major > BestMajor || (Major == BestMajor && Minor > BestMinor)) { Result = Pair.Key; BestMajor = Major; BestMinor = Minor; }
		}
		return Result;
	}
	void SelectSize(const FConvaiAvatarAsset& Asset, const FString& Platform, const FString& TargetEngine, FString& OutVersion, TOptional<int64>& OutBytes)
	{
		const FString Target = TargetEngine.IsEmpty() ? FString() : TEXT("ue-") + TargetEngine + TEXT("-") + Platform;
		if (!Target.IsEmpty() && Presence(Asset, Target) == EConvaiAvatarArtifactPresence::Available) OutVersion = Target;
		else OutVersion = Latest(Asset, Platform);
		if (OutVersion.IsEmpty() && Platform == TEXT("Raw") && Presence(Asset, TEXT("raw")) == EConvaiAvatarArtifactPresence::Available) OutVersion = TEXT("raw");
		const auto* Info = Asset.VersionInfo.Find(OutVersion);
		if (Info && Info->Presence == EConvaiAvatarArtifactPresence::Available && Info->SizeBytes > 0) OutBytes = Info->SizeBytes;
	}
}

FConvaiAvatarArtifactHealth ConvaiAvatarArtifactHealth::Evaluate(const FConvaiAvatarAsset& Asset, const FString& TargetEngineVersion)
{
	FConvaiAvatarArtifactHealth Result;
	int32 Major = 0, Minor = 0;
	const bool bKnownTarget = EngineNumber(TargetEngineVersion, Major, Minor);
	const FString Target = bKnownTarget ? FString::Printf(TEXT("%d.%d"), Major, Minor) : FString();
	if (bKnownTarget) Result.TargetWindowsVersion = TEXT("ue-") + Target + TEXT("-Windows");
	if (!Asset.bHasArtifactDetails)
	{
		Result.Warning = TEXT("Uploaded file availability has not been checked. Select this avatar to check its files.");
		return Result;
	}
	Result.LatestCompletedWindowsVersion = Latest(Asset, TEXT("Windows"));
	SelectSize(Asset, TEXT("Raw"), Target, Result.SourceVersion, Result.SourceBytes);
	SelectSize(Asset, TEXT("Windows"), Target, Result.WindowsVersion, Result.WindowsBytes);
	SelectSize(Asset, TEXT("Linux"), Target, Result.LinuxVersion, Result.LinuxBytes);

	bool bUnknownWindows = !Asset.bHasVersionInventory;
	TSet<FString> Candidates;
	for (const FString& Version : Asset.Versions) Candidates.Add(Version);
	for (const auto& Pair : Asset.VersionInfo) Candidates.Add(Pair.Key);
	for (const FString& Version : Candidates)
	{
		int32 CandidateMajor = 0, CandidateMinor = 0;
		if (VersionNumber(Version, TEXT("Windows"), CandidateMajor, CandidateMinor) && Presence(Asset, Version) == EConvaiAvatarArtifactPresence::Unknown) bUnknownWindows = true;
	}
	bool bUnknownSource = !Asset.bHasVersionInventory;
	for (const FString& Version : Candidates)
	{
		int32 SourceMajor = 0, SourceMinor = 0;
		if ((Version == TEXT("raw") || VersionNumber(Version, TEXT("Raw"), SourceMajor, SourceMinor)) &&
			Presence(Asset, Version) == EConvaiAvatarArtifactPresence::Unknown) bUnknownSource = true;
	}
	const FString EngineInstruction = bKnownTarget ? TEXT(" in Unreal ") + Target : FString();
	const FString ProjectInstruction = TEXT("the project containing this avatar") + EngineInstruction + TEXT(", then upload a Windows package.");
	const FString UploadInstruction = TEXT("Open ") + ProjectInstruction;
	FString NextStep;
	if (Asset.HasSource()) NextStep = TEXT("Download its source first if needed. ") + UploadInstruction;
	else if (!Result.SourceVersion.IsEmpty()) NextStep = TEXT("Its source download link is unavailable. Refresh, or open ") + ProjectInstruction;
	else if (bUnknownSource) NextStep = TEXT("Source availability could not be verified. Refresh, or open ") + ProjectInstruction;
	else NextStep = TEXT("Source is not uploaded. ") + UploadInstruction;
	if (Result.LatestCompletedWindowsVersion.IsEmpty() && !bUnknownWindows)
	{
		Result.Status = EConvaiAvatarArtifactHealth::NoCompletedWindows;
		Result.Warning = TEXT("Windows streaming is unavailable: no completed Windows package is uploaded. ") + NextStep;
	}
	else if (!bKnownTarget)
		Result.Warning = TEXT("The current published engine version is unavailable, so Windows package compatibility could not be checked. Refresh to check again.");
	else if (Presence(Asset, Result.TargetWindowsVersion) == EConvaiAvatarArtifactPresence::Available)
		Result.Status = EConvaiAvatarArtifactHealth::Ready;
	else if (Presence(Asset, Result.TargetWindowsVersion) == EConvaiAvatarArtifactPresence::Missing && !Result.LatestCompletedWindowsVersion.IsEmpty())
	{
		Result.Status = EConvaiAvatarArtifactHealth::TargetWindowsMissing;
		Result.Warning = FString::Printf(TEXT("Windows streaming is unavailable for Unreal %s: its Windows package is not uploaded. The latest uploaded Windows package is for Unreal %s. %s"),
			*Target, *EngineLabel(Result.LatestCompletedWindowsVersion), *NextStep);
	}
	else
		Result.Warning = TEXT("Windows package availability could not be verified. Refresh before deciding whether to upload again.");
	return Result;
}
