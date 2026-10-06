// Copyright Convai Inc. All Rights Reserved.
#include "Services/ConvaiProjectHostingSettings.h"

#include "Dom/JsonObject.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
const TCHAR* KeyRuntimeConfig = TEXT("runtime_config");
const TCHAR* KeyDeploymentConfig = TEXT("deployment_config");
const TCHAR* KeyFrontend = TEXT("ps-frontend-config");
const TCHAR* KeyUnreal = TEXT("ps-ue-config");
const TCHAR* KeyMatchViewportRes = TEXT("MatchViewportRes");
const TCHAR* KeyAfkTimeout = TEXT("AFKTimeout");

FString RenderJson(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object.IsValid() ? Object.ToSharedRef() : MakeShared<FJsonObject>(), Writer);
	return Text;
}

TSharedPtr<FJsonObject> ReadObjectField(const TSharedPtr<FJsonObject>& Owner, const TCHAR* Field)
{
	return Owner.IsValid() && Owner->HasTypedField<EJson::Object>(Field) ? Owner->GetObjectField(Field) : nullptr;
}

/** Numbers round-trip as doubles; print whole values without a trailing ".0". */
FString NumberToString(double Value)
{
	return FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value))
		? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Value)))
		: FString::SanitizeFloat(Value);
}

bool IsPositiveInteger(const FString& Value)
{
	if (Value.IsEmpty() || !Value.IsNumeric()) return false;
	const double Number = FCString::Atod(*Value);
	return Number > 0.0 && FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
}
}

const TArray<FString>& FConvaiProjectHostingSettings::FrontendBooleanKeys()
{
	static const TArray<FString> Keys = { TEXT("UseMic"), TEXT("HoveringMouse"), TEXT("MatchViewportRes"), TEXT("TimeoutIfIdle") };
	return Keys;
}

bool FConvaiProjectHostingSettings::ParseFrontendConfig(const FString& Json, TSharedPtr<FJsonObject>& OutConfig, FString& OutError)
{
	OutError.Reset();
	const FString Trimmed = Json.TrimStartAndEnd();

	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed.IsEmpty() ? TEXT("{}") : Trimmed);
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		OutError = TEXT("Enter valid JSON.");
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Parsed->Values)
	{
		if (!Pair.Value.IsValid()) continue;
		const EJson Type = Pair.Value->Type;

		// Arbitrary keys are allowed, but only scalars: the runtime cannot carry a nested object.
		if (Type != EJson::Boolean && Type != EJson::Number && Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("%s must be a boolean, number, or string."), *FString(*Pair.Key));
			return false;
		}
		if (FrontendBooleanKeys().Contains(Pair.Key) && Type != EJson::Boolean)
		{
			OutError = FString::Printf(TEXT("%s must be true or false."), *FString(*Pair.Key));
			return false;
		}
		if (Pair.Key == KeyAfkTimeout)
		{
			const double Number = Pair.Value->AsNumber();
			if (Type != EJson::Number || Number < 0.0 || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
			{
				OutError = TEXT("AFKTimeout must be a whole number of 0 or more.");
				return false;
			}
		}
	}

	OutConfig = Parsed;
	return true;
}

FConvaiProjectHostingSettings FConvaiProjectHostingSettings::Read(const TSharedPtr<FJsonObject>& Metadata)
{
	FConvaiProjectHostingSettings Settings;

	const TSharedPtr<FJsonObject> Config = ReadObjectField(Metadata, KeyDeploymentConfig);
	auto ReadNumber = [&Config](const TCHAR* Field, const TCHAR* Default) -> FString
	{
		double Value = 0.0;
		return Config.IsValid() && Config->TryGetNumberField(Field, Value) ? NumberToString(Value) : FString(Default);
	};

	// Same first-run defaults the dashboard shows: one machine, one ready stream, sleep after 15.
	Settings.bOnDemandMode = Config.IsValid() && Config->HasTypedField<EJson::Boolean>(TEXT("on_demand_mode"))
		&& Config->GetBoolField(TEXT("on_demand_mode"));
	Settings.TargetFreeStreams = ReadNumber(TEXT("target_free_streams"), TEXT("1"));
	Settings.StreamsPerInstance = ReadNumber(TEXT("streams_per_instance"), TEXT("1"));
	Settings.MinInstances = ReadNumber(TEXT("min_instances"), TEXT("0"));
	Settings.MaxInstances = ReadNumber(TEXT("max_instances"), TEXT("1"));
	Settings.IdleTimeoutMinutes = ReadNumber(TEXT("idle_timeout_minutes"), TEXT("15"));

	const TSharedPtr<FJsonObject> Runtime = ReadObjectField(Metadata, KeyRuntimeConfig);
	const TSharedPtr<FJsonObject> Frontend = ReadObjectField(Runtime, KeyFrontend);
	const TSharedPtr<FJsonObject> Unreal = ReadObjectField(Runtime, KeyUnreal);

	Settings.FrontendConfig = Frontend.IsValid() ? MakeShared<FJsonObject>(*Frontend) : MakeShared<FJsonObject>();
	Settings.FrontendJson = RenderJson(Settings.FrontendConfig);

	FString Resolution;
	if (Unreal.IsValid()) Unreal->TryGetStringField(TEXT("resolution"), Resolution);

	// Matching the viewport makes an explicit resolution meaningless, so it is not shown.
	const bool bMatchViewport = Settings.IsFrontendTrue(KeyMatchViewportRes);
	const FRegexPattern Pattern(TEXT("^(\\d+)x(\\d+)$"));
	FRegexMatcher Matcher(Pattern, Resolution);
	if (!bMatchViewport && Matcher.FindNext())
	{
		Settings.ResolutionWidth = Matcher.GetCaptureGroup(1);
		Settings.ResolutionHeight = Matcher.GetCaptureGroup(2);
	}

	if (Unreal.IsValid())
	{
		double Percentage = 0.0;
		if (Unreal->TryGetNumberField(TEXT("screenPercentage"), Percentage)) Settings.ScreenPercentage = NumberToString(Percentage);
		Unreal->TryGetStringField(TEXT("extraArgs"), Settings.ExtraArgs);
	}
	return Settings;
}

TOptional<bool> FConvaiProjectHostingSettings::GetFrontendBool(const FString& Key) const
{
	if (FrontendConfig.IsValid() && FrontendConfig->HasTypedField<EJson::Boolean>(Key))
		return FrontendConfig->GetBoolField(Key);
	return TOptional<bool>();
}

bool FConvaiProjectHostingSettings::IsFrontendTrue(const FString& Key) const
{
	const TOptional<bool> Value = GetFrontendBool(Key);
	return Value.IsSet() && Value.GetValue();
}

int32 FConvaiProjectHostingSettings::GetAfkTimeout() const
{
	double Value = 0.0;
	return FrontendConfig.IsValid() && FrontendConfig->TryGetNumberField(KeyAfkTimeout, Value)
		? static_cast<int32>(Value) : -1;
}

void FConvaiProjectHostingSettings::SetFrontendBool(const FString& Key, TOptional<bool> Value)
{
	if (!FrontendConfig.IsValid()) FrontendConfig = MakeShared<FJsonObject>();

	// Unset removes the key entirely. Writing an explicit false instead would force the flag off
	// where the user asked for the runtime's own default -- a different behaviour.
	if (!Value.IsSet()) FrontendConfig->RemoveField(Key);
	else FrontendConfig->SetBoolField(Key, Value.GetValue());

	// Turning on viewport matching retires any explicit resolution, exactly as the dashboard does.
	if (Key == KeyMatchViewportRes && Value.IsSet() && Value.GetValue())
	{
		ResolutionWidth.Reset();
		ResolutionHeight.Reset();
	}
	FrontendJson = RenderJson(FrontendConfig);
}

void FConvaiProjectHostingSettings::SetAfkTimeout(const FString& Value)
{
	if (!FrontendConfig.IsValid()) FrontendConfig = MakeShared<FJsonObject>();
	const FString Trimmed = Value.TrimStartAndEnd();
	// An empty box means "unset", which has to remove the key rather than send zero.
	if (Trimmed.IsEmpty()) FrontendConfig->RemoveField(KeyAfkTimeout);
	else FrontendConfig->SetNumberField(KeyAfkTimeout, FCString::Atod(*Trimmed));
	FrontendJson = RenderJson(FrontendConfig);
}

void FConvaiProjectHostingSettings::SetFrontendJson(const FString& Json)
{
	FrontendJson = Json;

	// Keep invalid text on screen so the user can correct it; Validate() reports why.
	TSharedPtr<FJsonObject> Parsed;
	FString Error;
	if (ParseFrontendConfig(Json, Parsed, Error)) FrontendConfig = Parsed;
}

namespace
{
const TCHAR* VpxComputeFlag = TEXT("-PixelStreamingVPXUseCompute");

/** True when this whitespace-separated token is the VPX flag, bare or with a value. */
bool IsVpxComputeToken(const FString& Token)
{
	return Token.StartsWith(VpxComputeFlag, ESearchCase::IgnoreCase)
		&& (Token.Len() == FCString::Strlen(VpxComputeFlag) || Token.Mid(FCString::Strlen(VpxComputeFlag), 1) == TEXT("="));
}
}

bool FConvaiProjectHostingSettings::GetDisableVpxCompute() const
{
	TArray<FString> Tokens;
	ExtraArgs.ParseIntoArrayWS(Tokens);
	for (const FString& Token : Tokens)
	{
		// Any explicit value counts as a user override; only "=false" means disabled.
		if (IsVpxComputeToken(Token)) return Token.EndsWith(TEXT("=false"), ESearchCase::IgnoreCase);
	}
	return false;
}

void FConvaiProjectHostingSettings::SetDisableVpxCompute(bool bDisable)
{
	// Rebuild from whole tokens so an unrelated quoted argument is never split or reordered.
	TArray<FString> Tokens;
	ExtraArgs.ParseIntoArrayWS(Tokens);
	Tokens.RemoveAll([](const FString& Token) { return IsVpxComputeToken(Token); });
	if (bDisable) Tokens.Add(FString(VpxComputeFlag) + TEXT("=false"));

	ExtraArgs = FString::Join(Tokens, TEXT(" "));
}

TMap<FString, FString> FConvaiProjectHostingSettings::Validate() const
{
	TMap<FString, FString> Errors;

	TSharedPtr<FJsonObject> Parsed;
	FString FrontendError;
	if (!ParseFrontendConfig(FrontendJson, Parsed, FrontendError)) Errors.Add(TEXT("frontend_json"), FrontendError);

	if (!IsFrontendTrue(KeyMatchViewportRes))
	{
		const bool bHasWidth = !ResolutionWidth.TrimStartAndEnd().IsEmpty();
		const bool bHasHeight = !ResolutionHeight.TrimStartAndEnd().IsEmpty();

		// A half-specified resolution is a mistake, not a partial override.
		if (bHasWidth != bHasHeight)
		{
			if (!bHasWidth) Errors.Add(TEXT("resolutionWidth"), TEXT("Enter a width when height is set."));
			if (!bHasHeight) Errors.Add(TEXT("resolutionHeight"), TEXT("Enter a height when width is set."));
		}
		if (bHasWidth && !IsPositiveInteger(ResolutionWidth)) Errors.Add(TEXT("resolutionWidth"), TEXT("Enter a positive whole number."));
		if (bHasHeight && !IsPositiveInteger(ResolutionHeight)) Errors.Add(TEXT("resolutionHeight"), TEXT("Enter a positive whole number."));
	}

	if (!ScreenPercentage.TrimStartAndEnd().IsEmpty())
	{
		const FString Trimmed = ScreenPercentage.TrimStartAndEnd();
		if (!Trimmed.IsNumeric() || FCString::Atod(*Trimmed) <= 0.0)
			Errors.Add(TEXT("screenPercentage"), TEXT("Enter a number greater than 0."));
	}
	return Errors;
}

TSharedRef<FJsonObject> FConvaiProjectHostingSettings::SerializeDeploymentConfig() const
{
	const TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	Config->SetBoolField(TEXT("on_demand_mode"), bOnDemandMode);

	// Only set fields are written, so an untouched field keeps whatever the server already had.
	auto Write = [&Config](const TCHAR* Field, const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (!Trimmed.IsEmpty()) Config->SetNumberField(Field, FCString::Atod(*Trimmed));
	};
	Write(TEXT("target_free_streams"), TargetFreeStreams);
	Write(TEXT("streams_per_instance"), StreamsPerInstance);
	Write(TEXT("min_instances"), MinInstances);
	Write(TEXT("max_instances"), MaxInstances);
	Write(TEXT("idle_timeout_minutes"), IdleTimeoutMinutes);
	return Config;
}

bool FConvaiProjectHostingSettings::SerializeRuntimeConfig(TSharedRef<FJsonObject>& OutRuntime, FString& OutError) const
{
	TSharedPtr<FJsonObject> Frontend;
	if (!ParseFrontendConfig(FrontendJson, Frontend, OutError)) return false;

	const TSharedRef<FJsonObject> Unreal = MakeShared<FJsonObject>();
	if (!IsFrontendTrue(KeyMatchViewportRes)
		&& !ResolutionWidth.TrimStartAndEnd().IsEmpty()
		&& !ResolutionHeight.TrimStartAndEnd().IsEmpty())
	{
		Unreal->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%sx%s"),
			*ResolutionWidth.TrimStartAndEnd(), *ResolutionHeight.TrimStartAndEnd()));
	}
	if (!ScreenPercentage.TrimStartAndEnd().IsEmpty())
		Unreal->SetNumberField(TEXT("screenPercentage"), FCString::Atod(*ScreenPercentage.TrimStartAndEnd()));
	if (!ExtraArgs.TrimStartAndEnd().IsEmpty())
		Unreal->SetStringField(TEXT("extraArgs"), ExtraArgs.TrimStartAndEnd());

	OutRuntime = MakeShared<FJsonObject>();
	OutRuntime->SetObjectField(KeyFrontend, Frontend);
	OutRuntime->SetObjectField(KeyUnreal, Unreal);
	return true;
}
