// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiSpeechToTextProxy.h"
#include "ConvaiUtils.h"
#include "Sound/SoundWave.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AudioDecompress.h"
#include "Engine.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonSerializer.h"
#include "../Convai.h"

namespace
{	
	static FString TextToSpeechURL() { return UConvaiURL::GetFullURL(TEXT("tts"), false); }
	static FString SpeechToTextURL() { return UConvaiURL::GetFullURL(TEXT("stt/"), false); }
}


DEFINE_LOG_CATEGORY(ConvaiS2THttpLog);

UConvaiSpeechToTextProxy* UConvaiSpeechToTextProxy::CreateSpeech2TextFromFileNameQueryProxy(UObject* WorldContextObject, FString filename)
{
	UConvaiSpeechToTextProxy* Proxy = NewObject<UConvaiSpeechToTextProxy>();
	Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	Proxy->URL = SpeechToTextURL();

	if (!FPaths::FileExists(filename))
	{
		// check if the file is relative to the save/BouncedWavFiles directory
		FString SaveDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		filename = FPaths::Combine(SaveDir, FString("BouncedWavFiles"), filename);
		if (!FPaths::FileExists(filename))
		{
			CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("File does not exist!, %s"), *filename);
			// A null proxy would fire neither pin; Activate fails the empty payload once they are bound.
			return Proxy;
		}
	}
	
	// Read the file into a byte array
	FFileHelper::LoadFileToArray(Proxy->Payload, *filename, 0);

	Proxy->bStereo = true;
	return Proxy;
}

UConvaiSpeechToTextProxy* UConvaiSpeechToTextProxy::CreateSpeech2TextFromSoundWaveQueryProxy(UObject* WorldContextObject, USoundWave* SoundWave)
{
	UConvaiSpeechToTextProxy* Proxy = NewObject<UConvaiSpeechToTextProxy>();
	Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	Proxy->URL = SpeechToTextURL();

	if (SoundWave == nullptr)
	{
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("Sound wave is invalid!"));
		// A null proxy would fire neither pin; Activate fails the empty payload once they are bound.
		return Proxy;
	}


	int32 SampleRate = 0;
	int32 NumChannels = 0;
	TArray<uint8> PCMData = UConvaiUtils::ExtractPCMDataFromSoundWave(SoundWave, SampleRate, NumChannels);
	// No PCM leaves the payload empty, and Activate fails it once, after the node has bound its pins.
	UConvaiUtils::PCMDataToWav(MoveTemp(PCMData), Proxy->Payload, NumChannels, SampleRate);
	Proxy->bStereo = NumChannels > 1;
	return Proxy;
}

UConvaiSpeechToTextProxy* UConvaiSpeechToTextProxy::CreateSpeech2TextFromArrayQueryProxy(UObject* WorldContextObject, TArray<uint8> Payload)
{
	UConvaiSpeechToTextProxy* Proxy = NewObject<UConvaiSpeechToTextProxy>();
	Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	Proxy->URL = SpeechToTextURL();
	Proxy->Payload = Payload;
	Proxy->bStereo = true;
	return Proxy;
}


void UConvaiSpeechToTextProxy::Activate()
{
	bFinished = false;
	UWorld* World = WorldPtr.Get();

	if (!World)
	{
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("Could not get a pointer to world!"));
		failed();
		return;
	}

	FHttpModule* Http = &FHttpModule::Get();
	if (!Http)
	{
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("Could not get a pointer to http module!"));
		failed();
		return;
	}

	if (Payload.Num() <= 44)
	{
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("Payload size is too small, %d bytes!"), Payload.Num());
		failed();
		return;
	}


	TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	FString AuthKey = AuthHeaderAndKey.Value;
	FString AuthHeader = AuthHeaderAndKey.Key;

	// Form Validation
	if (!UConvaiFormValidation::ValidateAuthKey(AuthKey) || !UConvaiFormValidation::ValidateInputVoice(Payload))
	{
		failed();
		return;
	}

	TArray<uint8> monoWavBytes;
	if (bStereo)
	{
		// Change the wav file from 2 channels to 1 channel
		UConvaiUtils::StereoToMono(Payload, monoWavBytes);
	}
	else
	{
		monoWavBytes = Payload;
	}

	// Create the request
	FHttpRequestRef Request = Http->CreateRequest();
	Request->OnProcessRequestComplete().BindUObject(this, &UConvaiSpeechToTextProxy::onHttpRequestComplete);

	// Set request fields
	Request->SetURL(URL);
	Request->SetVerb("POST");
	Request->SetHeader(TEXT("User-Agent"), TEXT("X-UnrealEngine-Agent"));

	Request->SetHeader("Content-Type", "multipart/form-data; boundary=blahblahsomeboundary");
	Request->SetHeader(AuthHeader, AuthKey);

	// prepare request content data
	//FString a = "\r\n--blahblahsomeboundary\r\n";
	//FString b = "Content-Disposition: form-data; name=\"Request->SetHeader(AuthHeader, AuthKey);\"\r\n\r\n";
	// Request->SetHeader(AuthHeader, AuthKey);
	FString c = "\r\n--blahblahsomeboundary\r\n";
	FString d = "Content-Disposition: form-data; name=\"file\";  filename=\"out.wav\"\r\n\r\n";

	// UpFileRawData
	FString e = "\r\n--blahblahsomeboundary--\r\n";

	TArray<uint8> data;
	//data.Append((uint8*)TCHAR_TO_UTF8(*a), a.Len());
	//data.Append((uint8*)TCHAR_TO_UTF8(*b), b.Len());
	//data.Append((uint8*)TCHAR_TO_UTF8(*Request->SetHeader(AuthHeader, AuthKey);), Request->SetHeader(AuthHeader, AuthKey);.Len());
	data.Append((uint8*)TCHAR_TO_UTF8(*c), c.Len());
	data.Append((uint8*)TCHAR_TO_UTF8(*d), d.Len());
	data.Append(monoWavBytes);
	data.Append((uint8*)TCHAR_TO_UTF8(*e), e.Len());


	Request->SetContent(data);

	// Run the request
	if (!Request->ProcessRequest()) failed();
}


void UConvaiSpeechToTextProxy::onHttpRequestComplete(FHttpRequestPtr RequestPtr, FHttpResponsePtr ResponsePtr, bool bWasSuccessful)
{
	if (!ResponsePtr)
	{
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("HTTP request failed - Response pointer is invalid"));
		failed();
		return;
	}
	if (!bWasSuccessful || ResponsePtr->GetResponseCode() < 200 || ResponsePtr->GetResponseCode() > 299)
	{
		this->Response = ResponsePtr->GetContentAsString();
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("HTTP request failed with code %d"), ResponsePtr->GetResponseCode());
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("Response:%s"), *this->Response);
		failed();
		return;
	}

	this->Response = ResponsePtr->GetContentAsString();

	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(this->Response);
	FString Transcript;
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid() || !Json->TryGetStringField(TEXT("result"), Transcript))
	{
		CONVAI_LOG(ConvaiS2THttpLog, Warning, TEXT("Reply has no transcript: %s"), *this->Response);
		failed();
		return;
	}

	this->Response = MoveTemp(Transcript);
	success();
}


void UConvaiSpeechToTextProxy::failed()
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	OnFailure.Broadcast(Response);
	finish();
}


void UConvaiSpeechToTextProxy::success()
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	OnSuccess.Broadcast(Response);
	finish();
}


void UConvaiSpeechToTextProxy::finish()
{
}

