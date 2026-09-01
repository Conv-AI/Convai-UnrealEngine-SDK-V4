// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "ConvaiConnectionInterface.h"
#include "ConvaiDefinitions.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiPushToTalkTestProbe.h"
#include "../Convai.h"
#include "GameFramework/Actor.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ConvaiPushToTalkTestHelpers
{
	/** A player component has to live on an actor; its ctor resolves the owner from its outer. */
	struct FOwnedPlayer
	{
		explicit FOwnedPlayer(UClass* Class = UConvaiPlayerComponent::StaticClass())
			: Owner(NewObject<AActor>(GetTransientPackage()))
			, Player(Cast<UConvaiPlayerComponent>(NewObject<UObject>(Owner.Get(), Class)))
		{
		}

		UConvaiPlayerComponent* Get() const { return Player.Get(); }

		/** The push-to-talk switch, on the subclass that declares it the way a Blueprint does. */
		void SetPushToTalk(bool bEnabled) const
		{
			CastChecked<UConvaiPushToTalkTestPlayerComponent>(Player.Get())->EnablePushToTalk = bEnabled;
		}

		TStrongObjectPtr<AActor> Owner;
		TStrongObjectPtr<UConvaiPlayerComponent> Player;
	};

	FOwnedPlayer MakeBlueprintLikePlayer(bool bPushToTalk)
	{
		FOwnedPlayer Player(UConvaiPushToTalkTestPlayerComponent::StaticClass());
		Player.SetPushToTalk(bPushToTalk);
		return Player;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiPushToTalkStopSecsTest,
	"Convai.PushToTalk.ResolvesVadStopSecs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiPushToTalkStopSecsTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiPushToTalkTestHelpers;

	FOwnedPlayer OpenMic = MakeBlueprintLikePlayer(false);
	FOwnedPlayer PushToTalk = MakeBlueprintLikePlayer(true);
	PushToTalk.Get()->PushToTalkStopSecs = 1234.0f;

	// The switch is a Blueprint variable, so the native side has to find it by name.
	FOwnedPlayer NoSwitchAtAll;
	TestFalse(TEXT("A component whose class has no such variable is not in push-to-talk mode"),
		NoSwitchAtAll.Get()->IsPushToTalkEnabled());
	TestFalse(TEXT("The Blueprint switch reads false when it is off"), OpenMic.Get()->IsPushToTalkEnabled());
	TestTrue(TEXT("The Blueprint switch reads true when it is on"), PushToTalk.Get()->IsPushToTalkEnabled());

	TestEqual(TEXT("No player components means no override"),
		UConvaiPlayerComponent::ResolvePushToTalkStopSecs({}), -1.0f);

	TestEqual(TEXT("An open-mic player asks for no override"),
		UConvaiPlayerComponent::ResolvePushToTalkStopSecs({OpenMic.Get()}), -1.0f);

	TestEqual(TEXT("A push-to-talk player contributes its stop timer"),
		UConvaiPlayerComponent::ResolvePushToTalkStopSecs({OpenMic.Get(), PushToTalk.Get()}), 1234.0f);

	// The registry outlives components that are being torn down.
	TestEqual(TEXT("A stale registry entry is skipped, not dereferenced"),
		UConvaiPlayerComponent::ResolvePushToTalkStopSecs({nullptr, PushToTalk.Get()}), 1234.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiPushToTalkGatesDeliveryTest,
	"Convai.PushToTalk.MuteSetterGatesDelivery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiPushToTalkGatesDeliveryTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiPushToTalkTestHelpers;

	// The button is the mute flag, so the setter has to behave for both modes and has to be
	// safe with no session behind it — a Blueprint can hold the key before anything connects.
	FOwnedPlayer Player = MakeBlueprintLikePlayer(true);
	UConvaiPlayerComponent* Mic = Player.Get();
	Mic->bMute = true;  // what UnmuteStreamingAudio leaves behind under push-to-talk

	Mic->SetMute(false);
	TestFalse(TEXT("Holding the key lets audio through"), Mic->bMute);

	Mic->SetMute(true);
	TestTrue(TEXT("Releasing it closes the mic again"), Mic->bMute);

	// The release carries the end-of-turn signal, so a repeat must not send a second one.
	Mic->SetMute(true);
	TestTrue(TEXT("A repeated release stays closed"), Mic->bMute);

	FOwnedPlayer OpenMic = MakeBlueprintLikePlayer(false);
	OpenMic.Get()->SetMute(true);
	TestTrue(TEXT("An open mic still soft-mutes"), OpenMic.Get()->bMute);
	OpenMic.Get()->SetMute(false);
	TestFalse(TEXT("An open mic still unmutes"), OpenMic.Get()->bMute);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiPushToTalkReleaseAfterServerTurnTest,
	"Convai.PushToTalk.ReleaseSkipsATurnTheServerAlreadyTook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiPushToTalkReleaseAfterServerTurnTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiPushToTalkTestHelpers;

	// The four shapes a hold takes on the wire. The send itself is unobservable here — SetMute
	// returns before it with no session — so the predicate behind it is what gets asserted.
	FOwnedPlayer Player = MakeBlueprintLikePlayer(true);
	UConvaiPlayerComponent* Mic = Player.Get();
	IConvaiConnectionInterface* Connection = static_cast<IConvaiConnectionInterface*>(Mic);
	Mic->bMute = true;

	// The backend can bracket none of a hold and still owe a reply, so silence is no reason to
	// withhold the end of the turn — with PushToTalkStopSecs, nothing else would ever end it.
	Mic->SetMute(false);
	TestTrue(TEXT("A hold the server never bracketed still ends its turn"),
		Mic->ShouldEndTurnOnRelease());

	// Issue 236: the server's own stop fired mid-hold and it answered the turn already.
	Connection->OnStartedTalking();
	Connection->OnTranscriptionReceived(TEXT("Hey, how are you"), true, true);
	TestFalse(TEXT("A turn the server already took is not ended a second time"),
		Mic->ShouldEndTurnOnRelease());

	Connection->OnTranscriptionReceived(TEXT("and also"), false, false);
	TestTrue(TEXT("Speech after the server's stop restores the release signal"),
		Mic->ShouldEndTurnOnRelease());

	// Deliberate: the latch follows the closed bracket, not the stop packet, so a release
	// inside the stop grace still ends the turn.
	Connection->OnFinishedTalking();
	TestTrue(TEXT("A release inside the stop grace still ends the turn"),
		Mic->ShouldEndTurnOnRelease());

	// Closing for real also cancels the grace ticker, so nothing outlives the test.
	Connection->OnTranscriptionReceived(TEXT("and also that"), true, true);
	Mic->SetMute(true);
	Mic->SetMute(false);
	TestTrue(TEXT("A press starts a hold that ends on release"), Mic->ShouldEndTurnOnRelease());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiPushToTalkLeavesVadAloneTest,
	"Convai.PushToTalk.ConnectParamsKeepServerVadDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiPushToTalkLeavesVadAloneTest::RunTest(const FString& Parameters)
{
	// The override the subsystem applies is only meaningful because /connect otherwise sends
	// nothing: bUseServerDefault gates all four VAD fields, and it is on out of the box.
	UConvaiSettings* Settings = Convai::Get().GetConvaiSettings();
	const FConvaiVADSettings Saved = Settings->VADSettings;
	Settings->VADSettings.bUseServerDefault = true;

	const FConvaiConnectionParams Params = FConvaiConnectionParams::Create(nullptr, TEXT("test-character"), nullptr);
	TestEqual(TEXT("Server defaults send no stop_secs of their own"), Params.VADStopSecs, -1.0f);

	Settings->VADSettings = Saved;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
