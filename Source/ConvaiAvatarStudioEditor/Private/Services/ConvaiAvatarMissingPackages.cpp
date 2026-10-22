// Copyright 2026 Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarMissingPackages.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"

namespace ConvaiAvatarMissingPackages
{
namespace
{
	constexpr int32 MaximumPackages = 15000;
	constexpr const TCHAR* Field = TEXT("acknowledged_missing_packages");
	bool ValidPath(const FString& Path, const FString& PluginName)
	{
		for (TCHAR Character : Path) if (Character < 32 || Character == 127) return false;
		// The dependency's plugin need not be mounted while an archive is staged.
		return Path.Len() < NAME_SIZE && FPackageName::IsValidTextForLongPackageName(Path)
			&& Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1) != INDEX_NONE
			&& !Path.StartsWith(TEXT("/Script/")) && !Path.StartsWith(TEXT("/") + PluginName + TEXT("/"));
	}
	bool Invalid(FString& Error)
	{
		Error = TEXT("The source's missing-reference review is invalid. It must list exact external asset package paths. Missing files inside the avatar plugin cannot be approved.");
		return false;
	}
}

bool Validate(const TArray<FName>& Packages, const FString& PluginName, FString& Error)
{
	if (Packages.Num() > MaximumPackages) return Invalid(Error);
	TSet<FName> Seen;
	for (FName Package : Packages)
	{
		if (!ValidPath(Package.ToString(), PluginName) || Seen.Contains(Package)) return Invalid(Error);
		Seen.Add(Package);
	}
	return true;
}

bool Read(const FJsonObject& Json, const FString& PluginName, TOptional<TArray<FName>>& OutPackages, FString& Error)
{
	OutPackages.Reset();
	const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
	if (!Value.IsValid()) return true; // Legacy source has no implied approval.
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Value->Type != EJson::Array || !Value->TryGetArray(Values) || Values->Num() > MaximumPackages) return Invalid(Error);
	TArray<FName> Packages;
	for (const auto& Item : *Values)
	{
		FString Path;
		if (!Item.IsValid() || Item->Type != EJson::String || !Item->TryGetString(Path) || !ValidPath(Path, PluginName)) return Invalid(Error);
		Packages.Add(FName(*Path));
	}
	if (!Validate(Packages, PluginName, Error)) return false;
	OutPackages = MoveTemp(Packages);
	return true;
}

void Write(FJsonObject& Json, const TOptional<TArray<FName>>& Packages)
{
	if (!Packages.IsSet()) return;
	TArray<TSharedPtr<FJsonValue>> Values;
	for (FName Package : Packages.GetValue()) Values.Add(MakeShared<FJsonValueString>(Package.ToString()));
	Json.SetArrayField(Field, Values);
}
}
