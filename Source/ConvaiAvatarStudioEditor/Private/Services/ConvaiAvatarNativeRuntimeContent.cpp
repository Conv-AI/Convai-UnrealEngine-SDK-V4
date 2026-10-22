// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiAvatarNativeRuntimeContent.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersion.h"
namespace ConvaiAvatarNativeRuntimeContent
{
const FString& BuiltinGroomContract()
{
    static const FString Id = TEXT("avatar-studio-ue58-windows-groom-v1");
    return Id;
}
bool IsBuiltinGroomRoot(const FString& Package)
{
    return Package == TEXT("/HairStrands/Emitters/StableSpringsSystem") || Package == TEXT("/HairStrands/Emitters/StableRodsSystem");
}
bool IsValidRequirement(const FString& Contract)
{
    if (Contract.IsEmpty() || Contract.Len()>128) return false;
    const TCHAR First = Contract[0];
    if (!(First>=TEXT('a') && First<=TEXT('z')) && !(First>=TEXT('0') && First<=TEXT('9'))) return false;
    for (TCHAR C:Contract) if (!(C>=TEXT('a') && C<=TEXT('z')) && !(C>=TEXT('0') && C<=TEXT('9')) && C!=TEXT('-') && C!=TEXT('.')) return false;
    return true;
}
bool Supports(const FString& Contract)
{
    return Contract == BuiltinGroomContract() && FEngineVersion::Current().GetMajor() == 5 && FEngineVersion::Current().GetMinor() == 8;
}
bool IsSharedBuiltinGroomRoot(const FString& Package, const FString& Contract, bool bPortableSourceImport)
{
    return IsBuiltinGroomRoot(Package) && (bPortableSourceImport ? Contract == BuiltinGroomContract() : Supports(Contract));
}
FString ReadDeclaration(const TSharedPtr<FJsonObject>& Version, const FString& AssetsRoot)
{
    if (!Version) return {};
    const TSharedPtr<FJsonObject>* Declaration = nullptr;
    if (!Version->TryGetObjectField(TEXT("runtime-content-contract"), Declaration)) return {};
    FString Id, Engine, Platform, Product, Target;
    FString Actual = AssetsRoot;
    while (Actual.EndsWith(TEXT("/"))) Actual.LeftChopInline(1);
    if (!(*Declaration)->TryGetStringField(TEXT("id"), Id) || !Supports(Id) ||
        !(*Declaration)->TryGetStringField(TEXT("engine"), Engine) || Engine != TEXT("5.8") ||
        !(*Declaration)->TryGetStringField(TEXT("platform"), Platform) || Platform != TEXT("Windows") ||
        !(*Declaration)->TryGetStringField(TEXT("product"), Product) || Product != TEXT("Avatar Studio") ||
        !(*Declaration)->TryGetStringField(TEXT("assets_root"), Target) || Target != TEXT("https://beta.convai.com/assets") || Target != Actual)
        return {};
    FString SupportedEngine;
    if (!Version->TryGetStringField(TEXT("current-ue-version"), SupportedEngine) || SupportedEngine != Engine) return {};
    return Id;
}
bool CheckTarget(const FString& Required, const FString& Available, bool bIncludesLinux, FString& Error)
{
    if (Required.IsEmpty()) return true;
    if (!Supports(Required) || Required != Available || bIncludesLinux)
    {
        Error = TEXT("This avatar needs built-in groom physics support on the target runtime. Support could not be confirmed for the selected target. This version supports Avatar Studio Unreal 5.8 Windows only. Check your connection, refresh, and retry; if it still fails, contact Convai support.");
        return false;
    }
    return true;
}
}
