// Copyright 2022 Convai Inc. All Rights Reserved.

#if WITH_TESTS
#include "ConvaiDefinitions.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiEmotionStateBotEmotionNamesTest,
	"Convai.Emotion.BotEmotionNamesMapToBasicEmotions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Every name the backend can put in a bot-emotion message (utils/emotion_utils.py
// in core-service): the eight Plutchik basics plus their scale-1 and scale-3 variants.
bool FConvaiEmotionStateBotEmotionNamesTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Name; int32 Scale; EBasicEmotions Expected; };
	static const FCase Cases[] = {
		{TEXT("Serenity"),     1, EBasicEmotions::Joy},
		{TEXT("Joy"),          2, EBasicEmotions::Joy},
		{TEXT("Ecstasy"),      3, EBasicEmotions::Joy},
		{TEXT("Acceptance"),   1, EBasicEmotions::Trust},
		{TEXT("Trust"),        2, EBasicEmotions::Trust},
		{TEXT("Admiration"),   3, EBasicEmotions::Trust},
		{TEXT("Apprehension"), 1, EBasicEmotions::Fear},
		{TEXT("Fear"),         2, EBasicEmotions::Fear},
		{TEXT("Terror"),       3, EBasicEmotions::Fear},
		{TEXT("Distraction"),  1, EBasicEmotions::Surprise},
		{TEXT("Surprise"),     2, EBasicEmotions::Surprise},
		{TEXT("Amazement"),    3, EBasicEmotions::Surprise},
		{TEXT("Pensiveness"),  1, EBasicEmotions::Sadness},
		{TEXT("Sadness"),      2, EBasicEmotions::Sadness},
		{TEXT("Grief"),        3, EBasicEmotions::Sadness},
		{TEXT("Boredom"),      1, EBasicEmotions::Disgust},
		{TEXT("Disgust"),      2, EBasicEmotions::Disgust},
		{TEXT("Loathing"),     3, EBasicEmotions::Disgust},
		{TEXT("Annoyance"),    1, EBasicEmotions::Anger},
		{TEXT("Anger"),        2, EBasicEmotions::Anger},
		{TEXT("Rage"),         3, EBasicEmotions::Anger},
		{TEXT("Interest"),     1, EBasicEmotions::Anticipation},
		{TEXT("Anticipation"), 2, EBasicEmotions::Anticipation},
		{TEXT("Vigilance"),    3, EBasicEmotions::Anticipation},
		// Legacy TTS-style names must keep working.
		{TEXT("Calm"),         2, EBasicEmotions::Trust},
		{TEXT("Bored"),        2, EBasicEmotions::Disgust},
	};

	for (const FCase& Case : Cases)
	{
		FConvaiEmotionState State;
		State.SetEmotionDataSingleEmotion(FString::Printf(TEXT("%s %d"), Case.Name, Case.Scale), /*EmotionOffset*/ 0.f);

		const float Expected = Case.Scale / 3.f;
		TestEqual(FString::Printf(TEXT("%s %d -> expected basic emotion score"), Case.Name, Case.Scale),
			State.GetEmotionScore(Case.Expected), Expected);
		TestEqual(FString::Printf(TEXT("%s %d -> None must carry no score"), Case.Name, Case.Scale),
			State.GetEmotionScore(EBasicEmotions::None), 0.f);
	}

	for (const TCHAR* Neutral : { TEXT("neutral 2"), TEXT("Neutral 2") })
	{
		FConvaiEmotionState State;
		State.SetEmotionDataSingleEmotion(Neutral, 0.f);
		for (int32 i = 0; i <= static_cast<int32>(EBasicEmotions::Anticipation); ++i)
		{
			TestEqual(FString::Printf(TEXT("%s leaves emotion %d at zero"), Neutral, i),
				State.GetEmotionScore(static_cast<EBasicEmotions>(i)), 0.f);
		}
	}
	// The backend interleaves neutral reports with real ones throughout a reply,
	// so a neutral must leave the last real emotion standing rather than reset it.
	{
		FConvaiEmotionState State;
		State.SetEmotionDataSingleEmotion(TEXT("Joy 3"), 0.f);
		State.SetEmotionDataSingleEmotion(TEXT("neutral 2"), 0.f);
		TestEqual(TEXT("neutral leaves the previous emotion in place"),
			State.GetEmotionScore(EBasicEmotions::Joy), 1.f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
