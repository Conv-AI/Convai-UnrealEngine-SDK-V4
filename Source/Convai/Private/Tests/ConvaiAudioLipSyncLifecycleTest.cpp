// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiFaceSync.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	UConvaiChatbotComponent* MakeOwnedLipSyncChatbot()
	{
		AActor* Owner = NewObject<AActor>(GetTransientPackage());
		return NewObject<UConvaiChatbotComponent>(Owner);
	}

	TArray<int16> MakeSignal(const int32 NumFrames)
	{
		TArray<int16> Signal;
		Signal.SetNumUninitialized(NumFrames);
		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			Signal[Index] = (Index & 1) == 0 ? 12000 : -12000;
		}
		return Signal;
	}

	FAnimationSequence MakeFaceSequence(const int32 FirstFrame, const int32 NumFrames)
	{
		constexpr int32 FrameRate = 60;
		FAnimationSequence Sequence;
		Sequence.FrameRate = FrameRate;
		Sequence.Duration = static_cast<double>(NumFrames) / FrameRate;
		Sequence.AnimationFrames.Reserve(NumFrames);
		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			FAnimationFrame& Frame = Sequence.AnimationFrames.AddDefaulted_GetRef();
			Frame.FrameIndex = FirstFrame + Index;
			Frame.BlendShapes.Add(TEXT("jawOpen"), 0.5f);
		}
		return Sequence;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiAudio_IdleSilenceIsNotForwarded,
	"Convai.Audio.Lifecycle.IdleSilenceIsNotForwarded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiAudio_IdleSilenceIsNotForwarded::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedLipSyncChatbot();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Chatbot);
	TArray<int16> Silence;
	Silence.SetNumZeroed(4800); // 100 ms at 48 kHz.

	for (int32 Index = 0; Index < 40; ++Index)
	{
		Connection->OnAudioDataReceived(Silence.GetData(), Silence.Num(), 48000, 16, 1);
	}

	TestEqual(TEXT("Continuous idle PCM never reaches playback"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), 0u);
	TestEqual(TEXT("Idle silence is not counted as response audio"),
		Chatbot->TotalAudioFramesReceived, 0u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiAudio_EarlySignalOpensLocalGate,
	"Convai.Audio.Lifecycle.EarlySignalOpensLocalGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiAudio_EarlySignalOpensLocalGate::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedLipSyncChatbot();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Chatbot);
	Connection->OnLLMStartedForResponse(TEXT("response-early"));

	const TArray<int16> Signal = MakeSignal(960); // 20 ms at 48 kHz.
	Connection->OnAudioDataReceived(Signal.GetData(), Signal.Num(), 48000, 16, 1);

	TestEqual(TEXT("The first real PCM chunk is retained and forwarded"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), static_cast<uint32>(Signal.Num() * sizeof(int16)));
	TestEqual(TEXT("Forwarded frame accounting includes the retained prefix"),
		Chatbot->TotalAudioFramesReceived, static_cast<uint32>(Signal.Num()));
	TestTrue(TEXT("Actual response PCM opens the local speaking gate before the delayed marker"),
		Chatbot->IsInConversation());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiLifecycle_ResponseOwnershipAndDrain,
	"Convai.Audio.Lifecycle.ResponseOwnershipAndDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiLifecycle_ResponseOwnershipAndDrain::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedLipSyncChatbot();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Chatbot);
	Connection->OnLLMStartedForResponse(TEXT("response-current"));
	const TArray<int16> Signal = MakeSignal(960);
	Connection->OnAudioDataReceived(Signal.GetData(), Signal.Num(), 48000, 16, 1);
	const uint32 BufferedBytes = Chatbot->AudioRingBuffer.GetAvailableBytes();
	Connection->OnStartedTalkingForResponse(FString());

	Connection->OnBotTurnCompleted(TEXT("response-other"), true, false, FString());
	TestEqual(TEXT("A legacy speaking marker cannot erase the response owner"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), BufferedBytes);

	Connection->OnBotTurnCompleted(FString(), true, false, FString());
	TestEqual(TEXT("An unowned cancellation cannot stop an identified response"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), BufferedBytes);

	Connection->OnBotTurnCompleted(TEXT("response-stale"), false, false, FString());
	TestTrue(TEXT("A stale completion cannot clear the current speaking state"), Chatbot->IsInConversation());
	TestEqual(TEXT("A stale completion cannot alter current audio"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), BufferedBytes);

	Connection->OnBotTurnCompleted(TEXT("response-current"), false, false, FString());
	TestEqual(TEXT("Normal turn completion preserves locally buffered audio for drain"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), BufferedBytes);
	TestTrue(TEXT("Queued local media keeps the component in conversation after turn completion"),
		Chatbot->IsInConversation());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiLifecycle_MatchingCancellationStopsMedia,
	"Convai.Audio.Lifecycle.MatchingCancellationStopsMedia",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiLifecycle_MatchingCancellationStopsMedia::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedLipSyncChatbot();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Chatbot);
	Connection->OnLLMStartedForResponse(TEXT("response-cancelled"));
	const TArray<int16> Signal = MakeSignal(960);
	Connection->OnAudioDataReceived(Signal.GetData(), Signal.Num(), 48000, 16, 1);
	TestTrue(TEXT("Test setup queued audio"), Chatbot->AudioRingBuffer.GetAvailableBytes() > 0);
	Connection->OnStartedTalkingForResponse(TEXT("response-stale"));

	Connection->OnBotTurnCompleted(TEXT("response-cancelled"), true, false, FString());
	TestEqual(TEXT("A stale speaking marker cannot steal ownership from a matching cancellation"),
		Chatbot->AudioRingBuffer.GetAvailableBytes(), 0u);

	UConvaiChatbotComponent* LegacyChatbot = MakeOwnedLipSyncChatbot();
	IConvaiConnectionInterface* LegacyConnection = static_cast<IConvaiConnectionInterface*>(LegacyChatbot);
	LegacyConnection->OnStartedTalking();
	LegacyConnection->OnAudioDataReceived(Signal.GetData(), Signal.Num(), 48000, 16, 1);
	const uint32 LegacyBufferedBytes = LegacyChatbot->AudioRingBuffer.GetAvailableBytes();
	LegacyConnection->OnBotTurnCompleted(TEXT("unowned-response"), true, false, FString());
	TestEqual(TEXT("A named cancellation cannot stop ownerless legacy media"),
		LegacyChatbot->AudioRingBuffer.GetAvailableBytes(), LegacyBufferedBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiFaceSync_StarvationRecovers,
	"Convai.FaceSync.StarvationRecovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiFaceSync_StarvationRecovers::RunTest(const FString& Parameters)
{
	AActor* Owner = NewObject<AActor>(GetTransientPackage());
	UConvaiFaceSyncComponent* FaceSync = NewObject<UConvaiFaceSyncComponent>(Owner);
	double AudioTime = 5.012;
	FGetAudioPlaybackTimeDelegate PlaybackTimeProvider;
	PlaybackTimeProvider.BindLambda([&AudioTime]() { return AudioTime; });
	FaceSync->SetAudioPlaybackTimeProvider(PlaybackTimeProvider);

	FaceSync->ConvaiApplyPrecomputedFacialAnimation(nullptr, 0, 0, 0,
		MakeFaceSequence(0, 200));
	FFrameSelectionResult Selection;
	TestFalse(TEXT("Audio beyond the current face buffer reports temporary starvation"),
		FaceSync->CalculateFrameSelection(Selection));

	FaceSync->ConvaiApplyPrecomputedFacialAnimation(nullptr, 0, 0, 0,
		MakeFaceSequence(200, 220));
	TestTrue(TEXT("New frames make the starved sequence playable without an explicit resume"),
		FaceSync->HasPlayableFrames());
	TestTrue(TEXT("Selection recovers against the current audio clock"),
		FaceSync->CalculateFrameSelection(Selection));
	TestTrue(TEXT("Recovery skips to the current timeline rather than replaying stale frames"),
		Selection.BufferIndex > 280 && Selection.BufferIndex < 340);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiFaceSync_StopClearsPendingIngress,
	"Convai.FaceSync.StopClearsPendingIngress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiFaceSync_StopClearsPendingIngress::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedLipSyncChatbot();
	Chatbot->LipSyncBuffer.Enqueue(MakeFaceSequence(0, 10));
	TestTrue(TEXT("Test setup queued a precomputed sequence"), Chatbot->LipSyncBuffer.HasData());

	Chatbot->StopVoice();
	TestFalse(TEXT("Explicit stop retires face data that has not reached the component"),
		Chatbot->LipSyncBuffer.HasData());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
