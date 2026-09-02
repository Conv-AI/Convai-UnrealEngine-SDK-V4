// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiPlayerComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Tests/ConvaiConversationStateTestProbe.h"
// Complete UPackage definition: the runtime (non-editor) target only
// forward-declares it, and GetTransientPackage()'s UPackage* must upcast
// to UObject* for NewObject's Outer parameter.
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Components must live on an actor: several audio-streamer paths reached by
	// the connection callbacks (e.g. FindFirstLipSyncComponent via OnLLMStarted)
	// dereference GetOwner(), which the component ctor resolves from its outer.
	UConvaiChatbotComponent* MakeOwnedChatbot()
	{
		AActor* Owner = NewObject<AActor>(GetTransientPackage());
		return NewObject<UConvaiChatbotComponent>(Owner);
	}

	UConvaiPlayerComponent* MakeOwnedPlayer()
	{
		AActor* Owner = NewObject<AActor>(GetTransientPackage());
		return NewObject<UConvaiPlayerComponent>(Owner);
	}
}

struct FPlayerStopGraceTestState
{
	FPlayerStopGraceTestState()
		: Owner(NewObject<AActor>(GetTransientPackage()))
		, Player(NewObject<UConvaiPlayerComponent>(Owner.Get()))
	{
	}

	IConvaiConnectionInterface* GetConnection() const
	{
		return static_cast<IConvaiConnectionInterface*>(Player.Get());
	}

	TStrongObjectPtr<AActor> Owner;
	TStrongObjectPtr<UConvaiPlayerComponent> Player;
};

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FConvStateVerifyCancelledStop,
	FAutomationTestBase*, Test,
	TSharedRef<FPlayerStopGraceTestState>, State);

bool FConvStateVerifyCancelledStop::Update()
{
	Test->TestTrue(
		TEXT("Continuing partial cancels the pending stop"),
		State->Player->IsSpeaking());

	State->GetConnection()->OnFinishedTalking();
	Test->TestTrue(
		TEXT("Second stop also waits for grace"),
		State->Player->IsSpeaking());
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FConvStateVerifyExpiredStop,
	FAutomationTestBase*, Test,
	TSharedRef<FPlayerStopGraceTestState>, State);

bool FConvStateVerifyExpiredStop::Update()
{
	Test->TestFalse(
		TEXT("Stop commits once real-time grace expires without more text"),
		State->Player->IsSpeaking());
	return true;
}

// ---------------------------------------------------------------------------
// Test: Is Thinking starts ONLY on bot-llm-started — never on a user turn end
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvStateTest_ThinkingSignal,
	"Convai.ConversationState.ThinkingSignal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvStateTest_ThinkingSignal::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedChatbot();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Chatbot);

	TestFalse(TEXT("Fresh component is not thinking"), Chatbot->IsProcessing());

	// A user turn ending is no promise any character will respond — it must
	// NOT open the thinking window (multiplayer: the user may be talking to
	// another player entirely).
	Connection->OnInterrupt();
	Connection->OnInterruptEnd();
	TestFalse(TEXT("User turn end does not start thinking"), Chatbot->IsProcessing());

	// bot-llm-started is the true signal.
	Connection->OnLLMStarted();
	TestTrue(TEXT("bot-llm-started starts thinking"), Chatbot->IsProcessing());

	// Bot speech starting closes the window (the character is now talking).
	Connection->OnStartedTalking();
	TestFalse(TEXT("Bot speech start clears thinking"), Chatbot->IsProcessing());

	// llm-no-response routes to OnFinishedTalking — that must also clear it.
	Connection->OnLLMStarted();
	TestTrue(TEXT("Thinking again"), Chatbot->IsProcessing());
	Connection->OnFinishedTalking();
	TestFalse(TEXT("Finished-talking (incl. llm-no-response route) clears thinking"), Chatbot->IsProcessing());

	return true;
}

// ---------------------------------------------------------------------------
// Test: Is Listening reflects the server-VAD user-speaking bracket
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvStateTest_ListeningBracket,
	"Convai.ConversationState.ListeningBracket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvStateTest_ListeningBracket::RunTest(const FString& Parameters)
{
	UConvaiChatbotComponent* Chatbot = MakeOwnedChatbot();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Chatbot);

	TestFalse(TEXT("Fresh component is not listening"), Chatbot->IsListening());
	Connection->OnInterrupt();
	TestTrue(TEXT("User speaking = listening"), Chatbot->IsListening());
	TestTrue(TEXT("Listening counts as in conversation"), Chatbot->IsInConversation());
	Connection->OnInterruptEnd();
	TestFalse(TEXT("User stopped = not listening"), Chatbot->IsListening());
	TestFalse(TEXT("Idle between turns is honest: not in conversation"), Chatbot->IsInConversation());

	return true;
}

// ---------------------------------------------------------------------------
// Test: a non-empty partial repairs a missing start; final repairs missing stop
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvStateTest_PlayerTranscriptFallback,
	"Convai.ConversationState.PlayerTranscriptFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvStateTest_PlayerTranscriptFallback::RunTest(const FString& Parameters)
{
	UConvaiPlayerComponent* Player = MakeOwnedPlayer();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Player);
	UConvaiConversationStateTestProbe* Probe =
		NewObject<UConvaiConversationStateTestProbe>(GetTransientPackage());
	Player->OnStartedTalkingDelegate.AddDynamic(
		Probe,
		&UConvaiConversationStateTestProbe::HandleStartedTalking);
	Player->OnFinishedTalkingDelegate.AddDynamic(
		Probe,
		&UConvaiConversationStateTestProbe::HandleFinishedTalking);
	Player->OnTranscriptionReceivedDelegate.AddDynamic(
		Probe,
		&UConvaiConversationStateTestProbe::HandleTranscription);

	TestFalse(TEXT("Fresh player is not speaking"), Player->IsSpeaking());

	Connection->OnTranscriptionReceived(TEXT("   "), false, false);
	TestFalse(TEXT("Whitespace-only partial does not synthesize start"), Player->IsSpeaking());

	Connection->OnTranscriptionReceived(TEXT("hello"), false, false);
	TestTrue(TEXT("First non-empty partial synthesizes start"), Player->IsSpeaking());

	Connection->OnTranscriptionReceived(TEXT("hello there"), false, false);
	TestTrue(TEXT("Further partials keep the same speaking bracket open"), Player->IsSpeaking());

	Connection->OnTranscriptionReceived(TEXT("hello there"), true, true);
	TestFalse(TEXT("Non-empty final synthesizes end"), Player->IsSpeaking());
	TestEqual(TEXT("First partial stream broadcasts Started once"), Probe->StartedCount, 1);
	TestEqual(TEXT("First final broadcasts Finished once"), Probe->FinishedCount, 1);

	Connection->OnTranscriptionReceived(TEXT("second"), false, false);
	Connection->OnTranscriptionReceived(TEXT("second utterance"), false, false);
	Connection->OnTranscriptionReceived(TEXT("second utterance"), true, true);
	TestEqual(TEXT("Second utterance broadcasts a new Started edge"), Probe->StartedCount, 2);
	TestEqual(TEXT("Second utterance broadcasts a new Finished edge"), Probe->FinishedCount, 2);

	TestEqual(TEXT("Two utterances emit six ordered lifecycle observations"), Probe->EventOrder.Num(), 6);
	if (Probe->EventOrder.Num() == 6)
	{
		TestEqual(TEXT("First event is Started"), Probe->EventOrder[0], FName(TEXT("Started")));
		TestEqual(TEXT("First final text precedes Finished"), Probe->EventOrder[1], FName(TEXT("FinalTranscript")));
		TestEqual(TEXT("First Finished follows final text"), Probe->EventOrder[2], FName(TEXT("Finished")));
		TestEqual(TEXT("Second event is Started"), Probe->EventOrder[3], FName(TEXT("Started")));
		TestEqual(TEXT("Second final text precedes Finished"), Probe->EventOrder[4], FName(TEXT("FinalTranscript")));
		TestEqual(TEXT("Second Finished follows final text"), Probe->EventOrder[5], FName(TEXT("Finished")));
	}

	Connection->OnTranscriptionReceived(TEXT("late final"), true, true);
	TestFalse(TEXT("A late final cannot reopen an already closed bracket"), Player->IsSpeaking());
	TestEqual(TEXT("Late final does not broadcast another Started edge"), Probe->StartedCount, 2);
	TestEqual(TEXT("Late final does not broadcast another Finished edge"), Probe->FinishedCount, 2);

	return true;
}

// ---------------------------------------------------------------------------
// Test: real VAD remains primary and duplicate/empty events are idempotent
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvStateTest_PlayerVadIdempotence,
	"Convai.ConversationState.PlayerVadIdempotence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvStateTest_PlayerVadIdempotence::RunTest(const FString& Parameters)
{
	UConvaiPlayerComponent* Player = MakeOwnedPlayer();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Player);
	UConvaiConversationStateTestProbe* Probe =
		NewObject<UConvaiConversationStateTestProbe>(GetTransientPackage());
	Player->OnStartedTalkingDelegate.AddDynamic(
		Probe,
		&UConvaiConversationStateTestProbe::HandleStartedTalking);
	Player->OnFinishedTalkingDelegate.AddDynamic(
		Probe,
		&UConvaiConversationStateTestProbe::HandleFinishedTalking);

	Connection->OnStartedTalking();
	Connection->OnStartedTalking();
	TestTrue(TEXT("Duplicate starts keep one speaking bracket open"), Player->IsSpeaking());
	TestEqual(TEXT("Duplicate starts broadcast one edge"), Probe->StartedCount, 1);

	Connection->OnTranscriptionReceived(TEXT(""), true, true);
	TestTrue(TEXT("Empty final sentinel does not close speech by itself"), Player->IsSpeaking());

	Connection->OnFinishedTalking();
	Connection->OnFinishedTalking();
	TestTrue(TEXT("Duplicate stops share one pending grace window"), Player->IsSpeaking());
	TestEqual(TEXT("Pending stop has not broadcast Finished"), Probe->FinishedCount, 0);

	Connection->OnTranscriptionReceived(TEXT("continued"), false, false);
	TestTrue(TEXT("A continuing partial cancels the duplicate stop"), Player->IsSpeaking());
	TestEqual(TEXT("Continuing partial does not rebroadcast Started"), Probe->StartedCount, 1);

	Connection->OnTranscriptionReceived(TEXT("continued"), true, true);
	TestFalse(TEXT("Final closes the bracket"), Player->IsSpeaking());
	Connection->OnFinishedTalking();
	TestEqual(TEXT("Final plus late stop broadcast one Finished edge"), Probe->FinishedCount, 1);

	return true;
}

// ---------------------------------------------------------------------------
// Test: premature stop is cancelled by a continuing partial during grace
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvStateTest_PlayerStopGrace,
	"Convai.ConversationState.PlayerStopGrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvStateTest_PlayerStopGrace::RunTest(const FString& Parameters)
{
	TSharedRef<FPlayerStopGraceTestState> State = MakeShared<FPlayerStopGraceTestState>();
	IConvaiConnectionInterface* Connection = State->GetConnection();

	Connection->OnStartedTalking();
	Connection->OnFinishedTalking();
	TestTrue(TEXT("Server stop remains pending during grace"), State->Player->IsSpeaking());

	Connection->OnTranscriptionReceived(TEXT("still speaking"), false, false);
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.35f));
	ADD_LATENT_AUTOMATION_COMMAND(FConvStateVerifyCancelledStop(this, State));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.35f));
	ADD_LATENT_AUTOMATION_COMMAND(FConvStateVerifyExpiredStop(this, State));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
