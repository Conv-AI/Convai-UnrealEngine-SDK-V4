// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiFaceSync.h"
#include "Misc/ScopeLock.h"
#include "ConvaiUtils.h"

DEFINE_LOG_CATEGORY(ConvaiFaceSyncLog);

namespace
{
	// Helper template: Creates a zero frame map from an array of names
	TMap<FName, float> CreateZeroFrameFromNames(const TArray<FString>& Names, float DefaultValue = 0.0f)
	{
		TMap<FName, float> ZeroFrame;
		for (const auto& Name : Names)
		{
			ZeroFrame.Add(*Name, DefaultValue);
		}
		return ZeroFrame;
	}

	TMap<FName, float> CreateZeroBlendshapes()
	{
		return CreateZeroFrameFromNames(ConvaiConstants::MetaHumanCtrlNames);
	}

	TMap<FName, float> CreateZeroArKitBlendshapes()
	{
		return CreateZeroFrameFromNames(ConvaiConstants::ARKitBlendShapesNames);
	}

	TMap<FName, float> CreateZeroVisemes()
	{
		TMap<FName, float> ZeroVisemes = CreateZeroFrameFromNames(ConvaiConstants::VisemeNames);
		ZeroVisemes["sil"] = 1.0f;
		return ZeroVisemes;
	}
	
	// Helper function to get lipsync sequence info for logging
	struct FLipSyncSequenceInfo
	{
		int32 TotalFrames;
		int32 RemainingFrames;
		float TotalDuration;
		float RemainingDuration;
	};

	FLipSyncSequenceInfo GetSequenceInfo(const FAnimationSequence& Sequence, double CurrentTimePassed)
	{
		FLipSyncSequenceInfo Info;
		Info.TotalFrames = Sequence.AnimationFrames.Num();
		Info.TotalDuration = Sequence.Duration;
		Info.RemainingDuration = FMath::Max(0.0f, Info.TotalDuration - (float)CurrentTimePassed);

		// Calculate remaining frames based on time passed
		if (Info.TotalDuration > 0.0f && Info.TotalFrames > 0)
		{
			float ProgressRatio = FMath::Clamp((float)CurrentTimePassed / Info.TotalDuration, 0.0f, 1.0f);
			int32 FramesPlayed = FMath::RoundToInt(ProgressRatio * Info.TotalFrames);
			Info.RemainingFrames = FMath::Max(0, Info.TotalFrames - FramesPlayed);
		}
		else
		{
			Info.RemainingFrames = Info.TotalFrames;
		}

		return Info;
	}
};

const TMap<FName, float> UConvaiFaceSyncComponent::ZeroBlendShapeFrame = CreateZeroBlendshapes();
const TMap<FName, float> UConvaiFaceSyncComponent::ZeroVisemeFrame = CreateZeroVisemes();
const TMap<FName, float> UConvaiFaceSyncComponent::ZeroArKitFrame = CreateZeroArKitBlendshapes();

EC_LipSyncMode UConvaiFaceSyncComponent::GetLipSyncMode() const
{
	return LipSyncMode;
}

bool UConvaiFaceSyncComponent::GeneratesFacialDataAsBlendshapes() const
{
	EC_LipSyncMode Mode = GetLipSyncMode();
	return Mode != EC_LipSyncMode::VisemeBased;
}

TArray<float> UConvaiFaceSyncComponent::ConvaiGetFacialData() const
{
	TArray<float> FacialDataValues;
	CurrentBlendShapesMap.GenerateValueArray(FacialDataValues);
	return FacialDataValues;
}

TArray<FString> UConvaiFaceSyncComponent::ConvaiGetFacialDataNames() const
{
	switch (GetLipSyncMode())
	{
	case EC_LipSyncMode::BS_MHA:
		return ConvaiConstants::MetaHumanCtrlNames;
	case EC_LipSyncMode::BS_ARKit:
		return ConvaiConstants::ARKitBlendShapesNames;
	case EC_LipSyncMode::VisemeBased:
		return ConvaiConstants::VisemeNames;
	case EC_LipSyncMode::BS_CC4_Extended:
		return ConvaiConstants::CC4ExtendedNames;
	default:
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("Lip sync mode is invalid "))
		return TArray<FString>();
	}
}

int32 UConvaiFaceSyncComponent::ConvaiGetFacialDataCount() const
{
	switch (GetLipSyncMode())
	{
		case EC_LipSyncMode::BS_MHA:
			return ConvaiConstants::MetaHumanCtrlNames.Num();
		case EC_LipSyncMode::BS_ARKit:
			return ConvaiConstants::ARKitBlendShapesNames.Num();
		case EC_LipSyncMode::VisemeBased:
			return ConvaiConstants::VisemeNames.Num();
		case EC_LipSyncMode::BS_CC4_Extended:
			return ConvaiConstants::CC4ExtendedNames.Num();
		default:
			return 0;
	}
}

const TMap<FName, float>& UConvaiFaceSyncComponent::GetZeroFrameForMode(EC_LipSyncMode Mode)
{
	switch (Mode)
	{
	case EC_LipSyncMode::BS_MHA:
		return ZeroBlendShapeFrame;
	case EC_LipSyncMode::BS_ARKit:
		return ZeroArKitFrame;
	case EC_LipSyncMode::VisemeBased:
	default:
		return ZeroVisemeFrame;
	}
}

UConvaiFaceSyncComponent::UConvaiFaceSyncComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	CurrentSequenceTimePassed = 0;
	bAutoActivate = true;
	bIsPaused = false;
	PauseStartTime = 0;
	TotalPausedDuration = 0;
}

void UConvaiFaceSyncComponent::BeginPlay()
{
	Super::BeginPlay();
	CurrentBlendShapesMap = GenerateZeroFrame();
}

bool UConvaiFaceSyncComponent::CalculateFrameSelection(FFrameSelectionResult& OutResult)
{
	FScopeLock ScopeLock(&SequenceCriticalSection);

	// Calculate time passed - prefer audio playback time for robustness to app freezes,
	// fall back to wall-clock time if audio time provider is not available
	if (AudioPlaybackTimeProvider.IsBound())
	{
		// Use actual audio playback position from the audio streamer
		// This is robust to app freezes because it tracks what the audio device has actually played
		CurrentSequenceTimePassed = AudioPlaybackTimeProvider.Execute();
	}
	else
	{
		// Fallback to wall-clock time calculation
		const double CurrentTime = FPlatformTime::Seconds();
		CurrentSequenceTimePassed = (CurrentTime - StartTime) - TotalPausedDuration;
	}
	CurrentSequenceTimePassed += UConvaiUtils::GetLipSyncTimeOffset();
	CurrentSequenceTimePassed = FMath::Max(0.0, CurrentSequenceTimePassed);

	if (CurrentSequenceTimePassed > MainSequenceBuffer.Duration)
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("Tick: Can't play lipsync - CurrentSequenceTimePassed: %f - MainSequenceBuffer.Duration: %f"), 
			CurrentSequenceTimePassed, 
			MainSequenceBuffer.Duration);
		OutResult.bValid = false;
		return false;
	}

	bIsPlaying = true;
	OutResult.bValid = true;

	// Calculate frame duration and offsets
	const float FrameDuration = 1.0f / double(MainSequenceBuffer.FrameRate);
	const float FrameOffset = FrameDuration * 0.5f;

	// Choose the current and next BlendShapes based on time position
	if (CurrentSequenceTimePassed <= FrameOffset)
	{
		// Beginning of sequence - blend from current to first frame
		OutResult.StartFrame = GetCurrentFrame();
		OutResult.EndFrame = MainSequenceBuffer.AnimationFrames[0].BlendShapes;
		OutResult.Alpha = CurrentSequenceTimePassed / FrameOffset + 0.5f;
		OutResult.FrameIndex = MainSequenceBuffer.AnimationFrames[0].FrameIndex;
		OutResult.BufferIndex = 0;
	}
	else if (CurrentSequenceTimePassed >= MainSequenceBuffer.Duration - FrameOffset)
	{
		// End of sequence - blend from last frame to zero
		const int32 LastFrameIdx = MainSequenceBuffer.AnimationFrames.Num() - 1;
		OutResult.StartFrame = MainSequenceBuffer.AnimationFrames[LastFrameIdx].BlendShapes;
		OutResult.EndFrame = GenerateZeroFrame();
		OutResult.Alpha = (CurrentSequenceTimePassed - (MainSequenceBuffer.Duration - FrameOffset)) / FrameOffset;
		OutResult.FrameIndex = MainSequenceBuffer.AnimationFrames[LastFrameIdx].FrameIndex;
		OutResult.BufferIndex = LastFrameIdx;
	}
	else
	{
		// Middle of sequence - blend between adjacent frames
		int32 CurrentFrameIndex = FMath::FloorToInt((CurrentSequenceTimePassed - FrameOffset) / FrameDuration);
		CurrentFrameIndex = FMath::Min(CurrentFrameIndex, MainSequenceBuffer.AnimationFrames.Num() - 1);
		const int32 NextFrameIndex = FMath::Min(CurrentFrameIndex + 1, MainSequenceBuffer.AnimationFrames.Num() - 1);
		
		OutResult.StartFrame = MainSequenceBuffer.AnimationFrames[CurrentFrameIndex].BlendShapes;
		OutResult.EndFrame = MainSequenceBuffer.AnimationFrames[NextFrameIndex].BlendShapes;
		OutResult.Alpha = (CurrentSequenceTimePassed - FrameOffset - (CurrentFrameIndex * FrameDuration)) / FrameDuration;
		OutResult.FrameIndex = MainSequenceBuffer.AnimationFrames[CurrentFrameIndex].FrameIndex;
		OutResult.BufferIndex = CurrentFrameIndex;

		Apply_StartEndFrames_PostProcessing(CurrentFrameIndex, NextFrameIndex, OutResult.Alpha, OutResult.StartFrame, OutResult.EndFrame);
	}

	return true;
}

void UConvaiFaceSyncComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Skip if no valid sequence or paused
	if (!IsValidSequence(MainSequenceBuffer) || bIsPaused)
	{
		return;
	}

	// Calculate which frames to interpolate between
	FFrameSelectionResult FrameSelection;
	if (!CalculateFrameSelection(FrameSelection))
	{
		return;
	}

	// Apply interpolation if enabled, otherwise just use the end frame for better performance
	if (bEnableInterpolation)
	{
		CurrentBlendShapesMap = InterpolateFrames(FrameSelection.StartFrame, FrameSelection.EndFrame, FrameSelection.Alpha);
	}
	else
	{
		CurrentBlendShapesMap = FrameSelection.EndFrame;
	}

	ApplyPostProcessing();

	// Trigger the delegate
	OnFacialDataReady.ExecuteIfBound();
}

void UConvaiFaceSyncComponent::ConvaiApplyPrecomputedFacialAnimation(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels, FAnimationSequence FaceSequence)
{
	{
		FScopeLock ScopeLock(&SequenceCriticalSection);
		MainSequenceBuffer.AnimationFrames.Append(FaceSequence.AnimationFrames);
		MainSequenceBuffer.Duration += FaceSequence.Duration;
		MainSequenceBuffer.FrameRate = FaceSequence.FrameRate;
	}

	if (bIsRecordingLipSync)
		AddSequenceToRecordingBuffer(FaceSequence);
}

void UConvaiFaceSyncComponent::ConvaiApplyFacialFrame(FAnimationFrame FaceFrame, double Duration)
{
	{
		FScopeLock ScopeLock(&SequenceCriticalSection);
		
		// For viseme-based mode, check for "sil" blendshape to detect silence/stop
		if (GetLipSyncMode() == EC_LipSyncMode::VisemeBased)
		{
			float* sil = FaceFrame.BlendShapes.Find("sil");
			if (sil != nullptr && *sil < 0)
			{
				// Clear inline to avoid deadlock (we already hold the lock)
				MainSequenceBuffer.AnimationFrames.Empty();
				MainSequenceBuffer.Duration = 0;
				bStopping = true;
				return;
			}
		}

		MainSequenceBuffer.AnimationFrames.Add(FaceFrame);
		MainSequenceBuffer.Duration += Duration;
		MainSequenceBuffer.FrameRate = Duration == 0 ? -1 : FMath::RoundToInt(1.0f / Duration);
	}
	if (bIsRecordingLipSync)
		AddFrameToRecordingBuffer(FaceFrame, Duration);
}

void UConvaiFaceSyncComponent::AddFrameToRecordingBuffer(const FAnimationFrame& Frame, double Duration)
{
	FScopeLock ScopeLock(&RecordingCriticalSection);
	RecordedSequenceBuffer.AnimationFrames.Add(Frame);
	RecordedSequenceBuffer.Duration += Duration;
	RecordedSequenceBuffer.FrameRate = Duration == 0 ? -1 : FMath::RoundToInt(1.0f / Duration);
}

void UConvaiFaceSyncComponent::AddSequenceToRecordingBuffer(const FAnimationSequence& Sequence)
{
	FScopeLock ScopeLock(&RecordingCriticalSection);
	RecordedSequenceBuffer.AnimationFrames.Append(Sequence.AnimationFrames);
	RecordedSequenceBuffer.Duration += Sequence.Duration;
	RecordedSequenceBuffer.FrameRate = Sequence.FrameRate;
}

void UConvaiFaceSyncComponent::StartRecordingLipSync()
{
	if (bIsRecordingLipSync)
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("Cannot start Recording LipSync while already recording LipSync"));
		return;
	}

	CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("Started Recording LipSync"));
	bIsRecordingLipSync = true;
}

FAnimationSequenceBP UConvaiFaceSyncComponent::FinishRecordingLipSync()
{
	CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("Finished Recording LipSync - Total Frames: %d - Duration: %f"), RecordedSequenceBuffer.AnimationFrames.Num(), RecordedSequenceBuffer.Duration);

	if (!bIsRecordingLipSync)
	{
		return FAnimationSequenceBP();
	}

	FScopeLock ScopeLock(&RecordingCriticalSection);
	bIsRecordingLipSync = false;

	FAnimationSequenceBP AnimationSequenceBP;
	AnimationSequenceBP.AnimationSequence = RecordedSequenceBuffer;
	RecordedSequenceBuffer.AnimationFrames.Empty();
	RecordedSequenceBuffer.Duration = 0;
	return AnimationSequenceBP;
}

bool UConvaiFaceSyncComponent::PlayRecordedLipSync(FAnimationSequenceBP RecordedLipSync, int StartFrame, int EndFrame, float OverwriteDuration)
{
	if (!IsValidSequence(RecordedLipSync.AnimationSequence))
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("Recorded LipSync is not valid - Total Frames: %d - Duration: %f"), RecordedLipSync.AnimationSequence.AnimationFrames.Num(), RecordedLipSync.AnimationSequence.Duration);
		return false;
	}

	if (bIsRecordingLipSync)
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("Cannot Play Recorded LipSync while Recording LipSync"));
		return false;
	}

	if (IsValidSequence(MainSequenceBuffer))
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("Playing Recorded LipSync and stopping currently playing LipSync"));
		ConvaiStopLipSync();
	}

	if (StartFrame > 0 && StartFrame > RecordedLipSync.AnimationSequence.AnimationFrames.Num() - 1)
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("StartFrame is greater than the recorded LipSync - StartFrame: %d Total Frames: %d - Duration: %f"), StartFrame, RecordedLipSync.AnimationSequence.AnimationFrames.Num(), RecordedLipSync.AnimationSequence.Duration);
		return false;
	}


	if (EndFrame > 0 && EndFrame < StartFrame)
	{
		CONVAI_LOG(ConvaiFaceSyncLog, Warning, TEXT("StartFrame cannot be greater than the EndFrame"));
		return false;
	}

	if (EndFrame > 0 && EndFrame < RecordedLipSync.AnimationSequence.AnimationFrames.Num()-1)
	{
		float FrameDuration = 1.0f / double(RecordedLipSync.AnimationSequence.FrameRate);
		int NumRemovedFrames = RecordedLipSync.AnimationSequence.AnimationFrames.Num() - EndFrame - 1;
		RecordedLipSync.AnimationSequence.AnimationFrames.RemoveAt(EndFrame + 1, NumRemovedFrames);
		float RemovedDuration = NumRemovedFrames * FrameDuration;
		RecordedLipSync.AnimationSequence.Duration -= RemovedDuration;
	}

	if (StartFrame > 0)
	{
		float FrameDuration = 1.0f / double(RecordedLipSync.AnimationSequence.FrameRate);
		int NumRemovedFrames = StartFrame;
		RecordedLipSync.AnimationSequence.AnimationFrames.RemoveAt(0, NumRemovedFrames);
		float RemovedDuration = NumRemovedFrames * FrameDuration;
		RecordedLipSync.AnimationSequence.Duration -= RemovedDuration;
	}

	if (OverwriteDuration > 0)
	{
		RecordedLipSync.AnimationSequence.Duration = OverwriteDuration;
	}

	CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("Playing Recorded LipSync - Total Frames: %d - Duration: %f"), RecordedLipSync.AnimationSequence.AnimationFrames.Num(), RecordedLipSync.AnimationSequence.Duration);
	ConvaiApplyPrecomputedFacialAnimation(nullptr, 0, 0, 0, RecordedLipSync.AnimationSequence);
	return true;
}
 
bool UConvaiFaceSyncComponent::IsValidSequence(const FAnimationSequence& Sequence)
{
	return Sequence.Duration > 0 
		&& Sequence.AnimationFrames.Num() > 0 
		&& Sequence.AnimationFrames[0].BlendShapes.Num() > 0;
}

bool UConvaiFaceSyncComponent::IsPlaying() const
{
	return bIsPlaying;
}

void UConvaiFaceSyncComponent::ResetTimeState()
{
	StartTime = FPlatformTime::Seconds();
	TotalPausedDuration = 0;
	PauseStartTime = 0;
	bIsPaused = false;
}

void UConvaiFaceSyncComponent::CalculateStartingTime()
{
	if (!bIsPlaying)
	{
		ResetTimeState();
	}
}

void UConvaiFaceSyncComponent::ForceRecalculateStartTime()
{
	ResetTimeState();
}

void UConvaiFaceSyncComponent::ClearMainSequence()
{
	FScopeLock ScopeLock(&SequenceCriticalSection);
	MainSequenceBuffer.AnimationFrames.Empty();
	MainSequenceBuffer.Duration = 0;
}

TMap<FName, float> UConvaiFaceSyncComponent::InterpolateFrames(const TMap<FName, float>& StartFrame, const TMap<FName, float>& EndFrame, float Alpha)
{
	const TArray<FString>& CurveNames = ConvaiGetFacialDataNames();
	TMap<FName, float> Result;
	for (const auto& CurveName : CurveNames)
	{
		float StartValue = StartFrame.Contains(*CurveName) ? StartFrame[*CurveName] : 0.0;
		float EndValue = EndFrame.Contains(*CurveName) ? EndFrame[*CurveName] : 0.0;
		Result.Add(*CurveName, FMath::Lerp(StartValue, EndValue, Alpha));
	}
	return Result;
}

void UConvaiFaceSyncComponent::ConvaiStopLipSync()
{
	FLipSyncSequenceInfo Info;
	{
		FScopeLock ScopeLock(&SequenceCriticalSection);
		Info = GetSequenceInfo(MainSequenceBuffer, CurrentSequenceTimePassed);
	}

	CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("ConvaiStopLipSync: Stopping lipsync - bIsPlaying: %s, bIsPaused: %s, TotalFrames: %d, FramesRemaining: %d, TotalDuration: %.3f, TimePlayed: %.3f, TimeRemaining: %.3f, TotalPausedDuration: %.3f"),
		bIsPlaying ? TEXT("true") : TEXT("false"),
		bIsPaused ? TEXT("true") : TEXT("false"),
		Info.TotalFrames,
		Info.RemainingFrames,
		Info.TotalDuration,
		CurrentSequenceTimePassed,
		Info.RemainingDuration,
		TotalPausedDuration);

	bIsPlaying = false;
	bIsPaused = false;
	CurrentSequenceTimePassed = 0;
	TotalPausedDuration = 0;
	PauseStartTime = 0;
	ClearMainSequence();
}

void UConvaiFaceSyncComponent::ConvaiPauseLipSync()
{
	if (!bIsPaused)
	{
		bIsPaused = true;
		PauseStartTime = FPlatformTime::Seconds();

		FLipSyncSequenceInfo Info;
		{
			FScopeLock ScopeLock(&SequenceCriticalSection);
			Info = GetSequenceInfo(MainSequenceBuffer, CurrentSequenceTimePassed);
		}

		CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("ConvaiPauseLipSync: Pausing lipsync - TotalFrames: %d, FramesRemaining: %d, TotalDuration: %.3f, TimePlayed: %.3f, TimeRemaining: %.3f, TotalPausedDuration: %.3f"),
			Info.TotalFrames,
			Info.RemainingFrames,
			Info.TotalDuration,
			CurrentSequenceTimePassed,
			Info.RemainingDuration,
			TotalPausedDuration);
	}
}

void UConvaiFaceSyncComponent::ConvaiResumeLipSync()
{
	if (bIsPaused)
	{
		bIsPaused = false;
		// Add the duration we were paused to the total paused duration
		double PauseDuration = FPlatformTime::Seconds() - PauseStartTime;
		TotalPausedDuration += PauseDuration;
		PauseStartTime = 0;

		FLipSyncSequenceInfo Info;
		{
			FScopeLock ScopeLock(&SequenceCriticalSection);
			Info = GetSequenceInfo(MainSequenceBuffer, CurrentSequenceTimePassed);
		}

		CONVAI_LOG(ConvaiFaceSyncLog, Log, TEXT("ConvaiResumeLipSync: Resuming lipsync - PauseDuration: %.3f, TotalPausedDuration: %.3f, TotalFrames: %d, FramesRemaining: %d, TotalDuration: %.3f, TimePlayed: %.3f, TimeRemaining: %.3f"),
			PauseDuration,
			TotalPausedDuration,
			Info.TotalFrames,
			Info.RemainingFrames,
			Info.TotalDuration,
			CurrentSequenceTimePassed,
			Info.RemainingDuration);
	}
}
