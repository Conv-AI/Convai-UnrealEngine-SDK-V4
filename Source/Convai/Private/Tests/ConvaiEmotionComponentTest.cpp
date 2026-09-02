// Copyright 2022 Convai Inc. All Rights Reserved.

#if WITH_TESTS
#include "ConvaiChatbotComponent.h"
#include "ConvaiConnectionInterface.h"
#include "ConvaiDefinitions.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiEmotionComponentDecodeTest,
	"Convai.Emotion.ComponentDecodesBotEmotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// The subsystem hands the chatbot the "<emotion> <scale>" string it built from a
// bot-emotion packet through IConvaiConnectionInterface; this drives that entry
// point directly and reads back what a Blueprint would via GetEmotionScore.
bool FConvaiEmotionComponentDecodeTest::RunTest(const FString& Parameters)
{
	AActor* Owner = NewObject<AActor>(GetTransientPackage());
	UConvaiChatbotComponent* Chatbot = NewObject<UConvaiChatbotComponent>(Owner);
	IConvaiConnectionInterface* Connection = Chatbot;

	Connection->OnEmotionReceived(TEXT("Ecstasy 3"), FAnimationFrame(), /*MultipleEmotions*/ false);
	TestEqual(TEXT("Ecstasy 3 -> Joy at full scale"), Chatbot->GetEmotionScore(EBasicEmotions::Joy), 1.f);

	Connection->OnEmotionReceived(TEXT("Annoyance 1"), FAnimationFrame(), false);
	TestEqual(TEXT("Annoyance 1 -> Anger at a third"), Chatbot->GetEmotionScore(EBasicEmotions::Anger), 1.f / 3.f);
	TestEqual(TEXT("a new emotion clears the previous one"), Chatbot->GetEmotionScore(EBasicEmotions::Joy), 0.f);

	Chatbot->EmotionOffset = 0.5f;
	Connection->OnEmotionReceived(TEXT("Acceptance 1"), FAnimationFrame(), false);
	TestEqual(TEXT("EmotionOffset is added to the scale"), Chatbot->GetEmotionScore(EBasicEmotions::Trust), 1.f / 3.f + 0.5f);

	Chatbot->LockEmotionState = true;
	Connection->OnEmotionReceived(TEXT("Rage 3"), FAnimationFrame(), false);
	TestEqual(TEXT("locked state ignores new emotions"), Chatbot->GetEmotionScore(EBasicEmotions::Anger), 0.f);
	TestEqual(TEXT("locked state keeps the old one"), Chatbot->GetEmotionScore(EBasicEmotions::Trust), 1.f / 3.f + 0.5f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
