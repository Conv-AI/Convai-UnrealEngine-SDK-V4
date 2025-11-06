// Fill out your copyright notice in the Description page of Project Settings.


#include "ConvaiWebcamBase.h"
#include "ConvaiVisionBaseUtils.h"
#include "TimerManager.h"


DEFINE_LOG_CATEGORY(ConvaiWebcamLog);


UConvaiWebcamBase::UConvaiWebcamBase(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	
	LastErrorCode = -1;
	m_MaxFPS = 15;
	bFirstFrame = true;
	bUpdateOnFetch = true;
	CurrentState = EVisionState::Stopped;
}

void UConvaiWebcamBase::EndPlay(EEndPlayReason::Type Reason)
{
	Stop();
	Super::EndPlay(Reason);
}


// Start VisionInterface
void UConvaiWebcamBase::Start()
{
	SetState(EVisionState::Capturing);
}

void UConvaiWebcamBase::Stop()
{
	SetState(EVisionState::Stopped);

	CleanTimers();
	CleanDelegates();

	bFirstFrame = true;
}

void UConvaiWebcamBase::SetState(EVisionState NewState)
{
	if(NewState != CurrentState)
	{
		CurrentState = NewState;
		OnVisionStateChanged.ExecuteIfBound(CurrentState);
	}

	if (NewState == EVisionState::Stopped)
	{
		OnFramesStopped.ExecuteIfBound();
	}
}

EVisionState UConvaiWebcamBase::GetState() const
{
	return CurrentState;
}

void UConvaiWebcamBase::SetMaxFPS(int MaxFPS)
{
	if (MaxFPS == 0)
	{
		SetErrorCodeAndMessage(-1, TEXT("MaxFPS cannot be 0"));
		return;
	}
	m_MaxFPS = MaxFPS;
}

int UConvaiWebcamBase::GetMaxFPS() const
{
	return m_MaxFPS;
}

bool UConvaiWebcamBase::IsCompressedDataAvailable() const
{
	return false;
}

bool UConvaiWebcamBase::GetCompressedData(int& width, int& height, TArray<uint8>& data)
{
	return false;
}

bool UConvaiWebcamBase::CaptureCompressed(int& width, int& height, TArray<uint8>& data, float ForceCompressionRatio)
{
	return false;
}

bool UConvaiWebcamBase::CaptureRaw(int& width, int& height, TArray<uint8>& data)
{
	return false;
}

UTexture* UConvaiWebcamBase::GetImageTexture(ETextureSourceType& TextureSourceType)
{
	return nullptr;
}

FString UConvaiWebcamBase::GetLastErrorMessage() const
{
	return LastErrorMessage;
}

int UConvaiWebcamBase::GetLastErrorCode() const
{
	return LastErrorCode;
}

// END VisionInterface


void UConvaiWebcamBase::SetErrorCodeAndMessage(int ErrorCode, const FString& ErrorMessage, bool bPrintToLog)
{
	LastErrorMessage = ErrorMessage;
	LastErrorCode = ErrorCode;

	if (bPrintToLog)
	{
		UE_LOG(ConvaiWebcamLog, Error, TEXT("ErrorCode : %d, ErroMessage : %s"), LastErrorCode, *LastErrorMessage);
	}
}

void UConvaiWebcamBase::ProcessFrameStopped()
{
	if (const UWorld* World = GetWorld())
	{
		TWeakObjectPtr<UConvaiWebcamBase> WeakThis(this);
		World->GetTimerManager().SetTimer(
			FrameStoppedTimerHandle,
			[WeakThis]()
			{
				if (!WeakThis.IsValid()) return;
				auto* Self = WeakThis.Get();

				Self->bFirstFrame = true;
				Self->OnFramesStopped.ExecuteIfBound();
				UE_LOG(ConvaiWebcamLog, Warning, TEXT("FrameStopped"));
			},
			5.0f,
			false
		);
	}
}

void UConvaiWebcamBase::CleanTimers()
{
	UWorld* TempWorld = GetWorld();
	if (!TempWorld)
		return;
	TempWorld->GetTimerManager().ClearTimer(FrameStoppedTimerHandle);
}

void UConvaiWebcamBase::CleanDelegates()
{
	OnVisionStateChanged.Unbind();
	OnFirstFrameCaptured.Unbind();
	OnFramesStopped.Unbind();
}

bool UConvaiWebcamBase::CanStart()
{
	if (GetState() == EVisionState::Starting || GetState() == EVisionState::Capturing)
		return false;

	return true;
}

bool UConvaiWebcamBase::CanStop()
{
	if (GetState() == EVisionState::Stopping || GetState() == EVisionState::Stopped)
		return false;

	return true;
}