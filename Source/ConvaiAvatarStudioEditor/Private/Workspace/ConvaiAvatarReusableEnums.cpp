// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarReusableEnums.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/UserDefinedEnum.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace ConvaiAvatarReusableEnumsPrivate
{
	// Kept independent of UObject loading so package-family boundaries have direct tests.
	bool SamePackageFamily(const FString& Stem, const TMap<FString, FString>& Before,
		const TMap<FString, FString>& After)
	{
		const FString* BeforeMain = Before.Find(Stem + TEXT(".uasset"));
		const FString* AfterMain = After.Find(Stem + TEXT(".uasset"));
		if (!BeforeMain || !AfterMain || BeforeMain->IsEmpty() || AfterMain->IsEmpty()) return false;
		const FString Prefix = Stem + TEXT(".");
		auto ValidHash = [](const FString& Hash)
		{
			if (Hash.Len() != 32) return false;
			for (TCHAR Character : Hash) if (!FChar::IsHexDigit(Character)) return false;
			return true;
		};
		auto Matches = [&](const TMap<FString, FString>& A, const TMap<FString, FString>& B)
		{
			for (const auto& Pair : A)
			{
				if (!Pair.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
				const FString* Other = B.Find(Pair.Key);
				if (!Other || !ValidHash(Pair.Value) || !Pair.Value.Equals(*Other, ESearchCase::IgnoreCase)) return false;
			}
			return true;
		};
		return !Before.Contains(Stem + TEXT(".umap")) && !After.Contains(Stem + TEXT(".umap")) &&
			Matches(Before, After) && Matches(After, Before);
	}
}

namespace ConvaiAvatarReusableEnums
{
namespace
{
	bool AllowedPackage(const FString& Name, FName OwnPackage)
	{
		return Name == OwnPackage.ToString() || Name.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive);
	}
	bool RegistryLeaf(IAssetRegistry& Registry, FName Name)
	{
		TArray<FName> Dependencies;
		// No query flags: include hard AND soft, game AND editor package dependencies.
		if (!Registry.GetDependencies(Name, Dependencies, UE::AssetRegistry::EDependencyCategory::Package)) return false;
		for (FName Dependency : Dependencies) if (!AllowedPackage(Dependency.ToString(), Name)) return false;
		return true;
	}
	bool OwnedMetadata(UObject* Object, UPackage* Package)
	{
		// UE5.6+ uses FMetaData rather than a metadata UObject. Older versions may
		// contain the native nonasset export. Avoid referring to the removed C++ type.
		if (Object->IsAsset() || Object->GetOuter() != Package ||
			Object->GetClass()->GetPathName() != TEXT("/Script/CoreUObject.MetaData")) return false;
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		const UMetaData* Metadata = Cast<UMetaData>(Object);
#else
		const UDEPRECATED_MetaData* Metadata = Cast<UDEPRECATED_MetaData>(Object);
#endif
		if (!Metadata) return false;
		for (const auto& Pair : Metadata->ObjectMetaDataMap)
		{
			const UObject* Owner = Pair.Key.Get();
			if (!Owner || !AllowedPackage(Owner->GetOutermost()->GetName(), Package->GetFName())) return false;
		}
		return true;
	}
	bool LoadedLeaf(UEnum* Enum, FName PackageName)
	{
		UPackage* Package = Enum->GetPackage();
		if (Package->IsDirty() || Package->GetFName() != PackageName || Enum->GetOuter() != Package ||
			Enum->GetClass() != UUserDefinedEnum::StaticClass()) return false;
#if WITH_METADATA && !UE_VERSION_OLDER_THAN(5, 6, 0)
		// Modern metadata keys are soft paths, outside the UObject property walk.
		// Reading the existing package-owned FMetaData does not create an export.
		for (const auto& Pair : Package->GetMetaData().ObjectMetaDataMap)
			if (!Pair.Key.IsNull() && !AllowedPackage(Pair.Key.GetLongPackageName(), PackageName)) return false;
#endif
		TArray<UObject*> Objects;
		GetObjectsWithPackage(Package, Objects);
		int32 EnumCount = 0;
		for (UObject* Object : Objects)
		{
			if (Object == Enum) ++EnumCount;
			else if (Object != Package && !OwnedMetadata(Object, Package)) return false;
			TArray<UObject*> References;
			FReferenceFinder Finder(References, nullptr, false, false, false, false);
			Finder.FindReferences(Object);
			for (UObject* Reference : References)
				if (Reference && !AllowedPackage(Reference->GetOutermost()->GetName(), PackageName)) return false;
			// Saved registry data is not sufficient for a live object that acquired a
			// new reference. Inspect soft paths without resolving/loading their targets.
			for (FPropertyValueIterator It(FProperty::StaticClass(), Object->GetClass(), Object); It; ++It)
			{
				FSoftObjectPath Path;
				if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(It.Key()))
					Path = Soft->GetPropertyValue(It.Value()).ToSoftObjectPath();
				else if (const FStructProperty* Struct = CastField<FStructProperty>(It.Key()))
				{
					if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
						Path = *static_cast<const FSoftObjectPath*>(It.Value());
				}
				if (!Path.IsNull() && !AllowedPackage(Path.GetLongPackageName(), PackageName)) return false;
			}
		}
		return EnumCount == 1 && !Package->IsDirty();
	}
}

bool Find(const FConvaiAvatarPreparedAsset& Previous, const FConvaiAvatarPreparedAsset& Current,
	const TMap<FString, FString>& CurrentPreparedFiles, TSet<FName>& OutDestinations, FString& Error)
{
	check(IsInGameThread());
	OutDestinations.Reset(); Error.Reset();
	if (Previous.PluginName != Current.PluginName || Previous.AssetId != Current.AssetId ||
		!FConvaiAvatarWorkspace::IsSafePluginName(Current.PluginName)) return true;
	const FString Mount = TEXT("/") + Current.PluginName + TEXT("/");
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.WaitForCompletion();
	for (const auto& Pair : Current.SourceToDestinationPackages)
	{
		const FName* OldDestination = Previous.SourceToDestinationPackages.Find(Pair.Key);
		const FString SourceName = Pair.Key.ToString(), DestinationName = Pair.Value.ToString();
		if (!OldDestination || OldDestination->ToString() != DestinationName ||
			!DestinationName.StartsWith(Mount, ESearchCase::CaseSensitive) ||
			Pair.Value != FConvaiAvatarWorkspace::MakeDestinationPackage(Pair.Key, Current.PluginName) ||
			!ConvaiAvatarReusableEnumsPrivate::SamePackageFamily(SourceName, Previous.SourceFileHashes, Current.SourceFileHashes) ||
			!ConvaiAvatarReusableEnumsPrivate::SamePackageFamily(DestinationName.Mid(Mount.Len()), Previous.PreparedFileHashes, CurrentPreparedFiles)) continue;
		UPackage* SourcePackage = FindPackage(nullptr, *SourceName);
		UPackage* DestinationPackage = FindPackage(nullptr, *DestinationName);
		if ((SourcePackage && SourcePackage->IsDirty()) || (DestinationPackage && DestinationPackage->IsDirty())) continue;
		TArray<FAssetData> SourceAssets, DestinationAssets;
		Registry.GetAssetsByPackageName(Pair.Key, SourceAssets);
		Registry.GetAssetsByPackageName(Pair.Value, DestinationAssets);
		if (SourceAssets.Num() != 1 || DestinationAssets.Num() != 1 ||
			SourceAssets[0].AssetClassPath != UUserDefinedEnum::StaticClass()->GetClassPathName() ||
			DestinationAssets[0].AssetClassPath != UUserDefinedEnum::StaticClass()->GetClassPathName() ||
			SourceAssets[0].AssetName != DestinationAssets[0].AssetName ||
			!RegistryLeaf(Registry, Pair.Key) || !RegistryLeaf(Registry, Pair.Value)) continue;
		UObject* SourceObject = SourceAssets[0].GetAsset();
		UObject* DestinationObject = DestinationAssets[0].GetAsset();
		if (!SourceObject || !DestinationObject)
		{
			Error = TEXT("Could not read an unchanged avatar enum while checking its prepared copy: ") +
				(!SourceObject ? SourceName : DestinationName);
			OutDestinations.Reset(); return false;
		}
		UEnum* SourceEnum = Cast<UEnum>(SourceObject);
		UEnum* DestinationEnum = Cast<UEnum>(DestinationObject);
		if (!SourceEnum || !DestinationEnum || SourceEnum->GetFName() != SourceAssets[0].AssetName ||
			DestinationEnum->GetFName() != DestinationAssets[0].AssetName ||
			!LoadedLeaf(SourceEnum, Pair.Key) || !LoadedLeaf(DestinationEnum, Pair.Value)) continue;
		OutDestinations.Add(Pair.Value);
	}
	return true;
}
}
