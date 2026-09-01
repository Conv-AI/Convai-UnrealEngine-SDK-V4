// Copyright 2022 Convai Inc. All Rights Reserved.


#if WITH_TESTS
#include "Utility/ConvaiSpatial.h"
#include "Utility/ConvaiContextFormat.h"
#include "DynamicContext/ConvaiDynamicContextTracker.h"
#include "DynamicContext/ConvaiPendingContextBatch.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiInitialSpatialSnapshotResponseTest,
	"Convai.DynamicContext.Spatial.InitialSnapshotResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiInitialSpatialSnapshotResponseTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiSpatial;
	for (const EC_RunLLMOption Configured :
		{ EC_RunLLMOption::Never, EC_RunLLMOption::Auto,
		  EC_RunLLMOption::Always })
	{
		TestEqual(TEXT("Initial snapshot is always silent"),
			ResolveSnapshotResponse(Configured, /*bInitialSnapshotDelivered*/ false),
			EC_RunLLMOption::Never);
		TestEqual(TEXT("Later changes retain their configured response"),
			ResolveSnapshotResponse(Configured, /*bInitialSnapshotDelivered*/ true),
			Configured);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMotionStateHysteresisTest,
	"Convai.DynamicContext.Spatial.MotionStateHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMotionStateHysteresisTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiSpatial;

	bool bMoving = false;
	double CandidateSince = -1.0;
	TestEqual(TEXT("First moving sample only starts confirmation"),
		UpdateMotionState(/*above start*/ true, /*below stop*/ false,
			0.0, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::None);
	TestFalse(TEXT("Still stopped during start confirmation"), bMoving);
	TestEqual(TEXT("Sustained motion confirms start"),
		UpdateMotionState(true, false, 0.25, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::Started);
	TestTrue(TEXT("Moving after confirmed start"), bMoving);

	TestEqual(TEXT("Dead-band motion does not begin stopping"),
		UpdateMotionState(/*above start*/ false, /*below stop*/ false,
			0.50, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::None);
	TestTrue(TEXT("Dead band preserves moving state"), bMoving);

	TestEqual(TEXT("First quiet sample only starts stop confirmation"),
		UpdateMotionState(false, true, 0.75, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::None);
	TestEqual(TEXT("Brief quiet period does not stop"),
		UpdateMotionState(false, true, 1.20, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::None);
	TestEqual(TEXT("Sustained quiet confirms stop"),
		UpdateMotionState(false, true, 1.25, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::Stopped);
	TestFalse(TEXT("Stopped after confirmation"), bMoving);

	// A one-sample twitch must not leave a latent candidate that fires later.
	TestEqual(TEXT("Jitter begins but does not confirm"),
		UpdateMotionState(true, false, 2.0, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::None);
	TestEqual(TEXT("Quiet sample cancels the candidate"),
		UpdateMotionState(false, true, 2.1, bMoving, CandidateSince, 0.25, 0.50),
		EMotionTransition::None);
	TestTrue(TEXT("Candidate reset"), CandidateSince < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMovementSensitivityTest,
	"Convai.DynamicContext.Spatial.MovementSensitivity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMovementSensitivityTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiSpatial;

	struct FExpectedProfile
	{
		EConvaiMovementSensitivity Sensitivity;
		float Start;
		float Stop;
		float Excursion;
	};
	const FExpectedProfile Expected[] = {
		{EConvaiMovementSensitivity::VeryLow, 20.0f, 8.0f, 10.0f},
		{EConvaiMovementSensitivity::Low, 10.0f, 4.0f, 5.0f},
		{EConvaiMovementSensitivity::Medium, 5.0f, 2.0f, 2.5f},
		{EConvaiMovementSensitivity::High, 2.5f, 1.0f, 1.25f},
		{EConvaiMovementSensitivity::VeryHigh, 1.25f, 0.5f, 0.625f}
	};

	float PreviousStart = TNumericLimits<float>::Max();
	float PreviousStop = TNumericLimits<float>::Max();
	for (const FExpectedProfile& Profile : Expected)
	{
		const FMotionThresholds Thresholds =
			GetMotionThresholds(Profile.Sensitivity);
		TestEqual(TEXT("Linear start threshold matches its profile"),
			Thresholds.StartLinearSpeed, Profile.Start);
		TestEqual(TEXT("Angular start threshold matches its profile"),
			Thresholds.StartAngularSpeed, Profile.Start);
		TestEqual(TEXT("Linear stop threshold matches its profile"),
			Thresholds.StopLinearSpeed, Profile.Stop);
		TestEqual(TEXT("Angular stop threshold matches its profile"),
			Thresholds.StopAngularSpeed, Profile.Stop);
		TestEqual(TEXT("Linear excursion threshold matches its profile"),
			Thresholds.LinearExcursionFloor, Profile.Excursion);
		TestEqual(TEXT("Angular excursion threshold matches its profile"),
			Thresholds.AngularExcursionFloor, Profile.Excursion);
		TestTrue(TEXT("Increasing sensitivity never raises the start threshold"),
			Thresholds.StartLinearSpeed <= PreviousStart);
		TestTrue(TEXT("Increasing sensitivity never raises the stop threshold"),
			Thresholds.StopLinearSpeed <= PreviousStop);
		PreviousStart = Thresholds.StartLinearSpeed;
		PreviousStop = Thresholds.StopLinearSpeed;
	}

	const FMotionThresholds Medium =
		GetMotionThresholds(EConvaiMovementSensitivity::Medium);
	TestEqual(TEXT("Medium retains the original start threshold"),
		Medium.StartLinearSpeed, MotionStartLinearSpeed);
	TestEqual(TEXT("Medium retains the original stop threshold"),
		Medium.StopLinearSpeed, MotionStopLinearSpeed);

	const FMotionTrend EndpointBounce = ComputeMotionTrend(
		FVector::ZeroVector, FQuat::Identity, 0.0,
		FVector::ZeroVector, FQuat::Identity, 0.50,
		/*MaxLinearExcursion*/ 8.0f, /*MaxAngularExcursion*/ 0.0f);
	TestFalse(TEXT("Medium still notices a large physics bounce"),
		IsMotionTrendQuiet(EndpointBounce, Medium));
	TestTrue(TEXT("Very Low filters a large endpoint physics bounce"),
		IsMotionTrendQuiet(EndpointBounce,
			GetMotionThresholds(EConvaiMovementSensitivity::VeryLow)));
	TestFalse(TEXT("Very Low endpoint bounce vetoes a raw restart spike"),
		IsMotionStartEvidence(/*raw above start*/ true, EndpointBounce,
			GetMotionThresholds(EConvaiMovementSensitivity::VeryLow)));

	const FMotionTrend SlowTravel = ComputeMotionTrend(
		FVector::ZeroVector, FQuat::Identity, 0.0,
		FVector(0.0f, 0.0f, 1.5f), FQuat::Identity, 0.50,
		/*MaxLinearExcursion*/ 1.5f, /*MaxAngularExcursion*/ 0.0f);
	TestFalse(TEXT("High detects coherent slow travel"),
		IsMotionTrendQuiet(SlowTravel,
			GetMotionThresholds(EConvaiMovementSensitivity::High)));
	TestTrue(TEXT("High permits a coherent slow start"),
		IsMotionStartEvidence(/*raw above start*/ true, SlowTravel,
			GetMotionThresholds(EConvaiMovementSensitivity::High)));
	TestTrue(TEXT("Very Low intentionally ignores the same slow travel"),
		IsMotionTrendQuiet(SlowTravel,
			GetMotionThresholds(EConvaiMovementSensitivity::VeryLow)));

	const FMotionTrend ClearTravel = ComputeMotionTrend(
		FVector::ZeroVector, FQuat::Identity, 0.0,
		FVector(0.0f, 0.0f, 12.0f), FQuat::Identity, 0.50,
		/*MaxLinearExcursion*/ 12.0f, /*MaxAngularExcursion*/ 0.0f);
	TestFalse(TEXT("Very Low still detects clear movement"),
		IsMotionTrendQuiet(ClearTravel,
			GetMotionThresholds(EConvaiMovementSensitivity::VeryLow)));
	TestTrue(TEXT("Very Low still permits a clear start"),
		IsMotionStartEvidence(/*raw above start*/ true, ClearTravel,
			GetMotionThresholds(EConvaiMovementSensitivity::VeryLow)));

	const FMotionTrend SubtleRotation = ComputeMotionTrend(
		FVector::ZeroVector, FQuat::Identity, 0.0,
		FVector::ZeroVector, FRotator(0.0f, 3.0f, 0.0f).Quaternion(), 0.50,
		/*MaxLinearExcursion*/ 0.0f, /*MaxAngularExcursion*/ 3.0f);
	TestFalse(TEXT("High detects subtle coherent rotation"),
		IsMotionTrendQuiet(SubtleRotation,
			GetMotionThresholds(EConvaiMovementSensitivity::High)));
	TestTrue(TEXT("Very Low filters the same subtle rotation"),
		IsMotionTrendQuiet(SubtleRotation,
			GetMotionThresholds(EConvaiMovementSensitivity::VeryLow)));

	TestEqual(TEXT("Very High retains direction for sub-Medium motion"),
		MotionStateValue(true, FVector(0.0f, 0.0f, 0.75f), false,
			FTransform::Identity, FVector::ZeroVector), TEXT("Upward"));

	// Low deliberately has a wide 4..10 cm/s hysteresis band. A tiny endpoint
	// rebound in that band may postpone the coarse stop while its rolling window
	// settles, but it must not rewrite the completed upward trip as Downward.
	const FMotionThresholds Low =
		GetMotionThresholds(EConvaiMovementSensitivity::Low);
	FVector SemanticVelocity(0.0f, 0.0f, 100.0f);
	const FVector TinyDownwardRebound(0.0f, 0.0f, -6.0f);
	TestTrue(TEXT("Low endpoint rebound is above the stop threshold"),
		TinyDownwardRebound.Size() > Low.StopLinearSpeed);
	TestTrue(TEXT("Low endpoint rebound is below the start threshold"),
		TinyDownwardRebound.Size() < Low.StartLinearSpeed);
	TestTrue(TEXT("The tiny rebound points in a meaningfully different direction"),
		HasMeaningfulMotionDirectionChange(
			SemanticVelocity, false, TinyDownwardRebound, false));
	TestFalse(TEXT("The tiny rebound lacks clear direction evidence"),
		HasClearMotionDirectionEvidence(
			TinyDownwardRebound, 0.0f,
			TinyDownwardRebound, 0.0f, false, Low));
	if (HasMeaningfulMotionDirectionChange(
			SemanticVelocity, false, TinyDownwardRebound, false)
		&& HasClearMotionDirectionEvidence(
			TinyDownwardRebound, 0.0f,
			TinyDownwardRebound, 0.0f, false, Low))
	{
		SemanticVelocity = TinyDownwardRebound;
	}
	TestEqual(TEXT("Settling tail retains the prior upward semantic"),
		MotionStateValue(true, SemanticVelocity, false,
			FTransform::Identity, FVector::ZeroVector), TEXT("Upward"));
	TestEqual(TEXT("The eventual coarse stop still becomes Stopped"),
		MotionStateValue(false, SemanticVelocity, false,
			FTransform::Identity, FVector::ZeroVector), TEXT("Stopped"));

	// The ready rolling trend can retain a fast endpoint rebound for one poll
	// after the object is already quiet. It may stay above Low's start speed,
	// but current raw motion must still agree before replacing the trip.
	const FVector StaleFastDownwardTrend(
		0.0f, 0.0f, -(Low.StartLinearSpeed * 4.0f));
	TestTrue(TEXT("Stale rebound trend itself clears Low's start speed"),
		StaleFastDownwardTrend.Size() >= Low.StartLinearSpeed);
	TestFalse(TEXT("Quiet latest sample vetoes the stale rebound direction"),
		HasClearMotionDirectionEvidence(
			/*raw now quiet*/ FVector::ZeroVector, 0.0f,
			StaleFastDownwardTrend, 0.0f, false, Low));
	TestFalse(TEXT("Opposing latest sample vetoes the stale rebound direction"),
		HasClearMotionDirectionEvidence(
			/*raw returned upward*/ -StaleFastDownwardTrend, 0.0f,
			StaleFastDownwardTrend, 0.0f, false, Low));
	TestFalse(TEXT("Quiet raw rotation vetoes a stale rolling rotation"),
		HasClearMotionDirectionEvidence(
			FVector::ZeroVector, /*raw angular*/ 0.0f,
			FVector::ZeroVector,
			/*rolling angular*/ Low.StartAngularSpeed * 2.0f,
			/*rotation only*/ true, Low));
	TestTrue(TEXT("Sustained raw and rolling rotation remains clear"),
		HasClearMotionDirectionEvidence(
			FVector::ZeroVector,
			/*raw angular*/ Low.StartAngularSpeed,
			FVector::ZeroVector,
			/*rolling angular*/ Low.StartAngularSpeed,
			/*rotation only*/ true, Low));
	TestEqual(TEXT("Stale fast rebound still retains the upward trip"),
		MotionStateValue(true, SemanticVelocity, false,
			FTransform::Identity, FVector::ZeroVector), TEXT("Upward"));

	const FVector ClearDownwardReversal(
		0.0f, 0.0f, -(Low.StartLinearSpeed + 1.0f));
	TestTrue(TEXT("A clear reversal reaches Low's direction-evidence threshold"),
		HasClearMotionDirectionEvidence(
			/*raw*/ ClearDownwardReversal, 0.0f,
			/*rolling*/ ClearDownwardReversal, 0.0f, false, Low));
	FMotionThresholds LowContinuation = Low;
	LowContinuation.StartLinearSpeed = Low.StopLinearSpeed;
	LowContinuation.StartAngularSpeed = Low.StopAngularSpeed;
	const FVector SustainedSlowReversal(
		0.0f, 0.0f, -(Low.StopLinearSpeed + 4.0f));
	TestFalse(TEXT("Slow reversal cannot start a Low direction candidate"),
		HasClearMotionDirectionEvidence(
			SustainedSlowReversal, 0.0f,
			SustainedSlowReversal, 0.0f, false, Low));
	TestTrue(TEXT("An active Low direction candidate continues above Stop"),
		HasClearMotionDirectionEvidence(
			SustainedSlowReversal, 0.0f,
			SustainedSlowReversal, 0.0f, false, LowContinuation));
	const float LowDirectionProgressFloor =
		MotionDirectionProgressMultiplier * Low.LinearExcursionFloor;
	TestEqual(TEXT("Low uses 25 cm of candidate confirmation progress"),
		LowDirectionProgressFloor, 25.0f);
	const float LowSettlingEnvelope =
		MotionSettlingEnvelopeMultiplier * Low.LinearExcursionFloor;
	TestEqual(TEXT("Low bounded settling enters within 60 cm"),
		LowSettlingEnvelope, 60.0f);
	const float LowSettlingReleaseEnvelope =
		MotionSettlingReleaseMultiplier * Low.LinearExcursionFloor;
	TestEqual(TEXT("Low bounded settling releases beyond 70 cm"),
		LowSettlingReleaseEnvelope, 70.0f);
	TestFalse(TEXT("Captured 20.5 cm endpoint rebound cannot replace the trip"),
		IsMotionDirectionCandidateConfirmed(
			/*currently coherent*/ true,
			/*coherent seconds*/ 0.30,
			/*integrated progress*/ 20.5f,
			LowDirectionProgressFloor));
	TestFalse(TEXT("A rebased episode cannot inherit the trip's confirmation"),
		IsMotionDirectionCandidateConfirmed(
			/*currently coherent*/ true,
			/*coherent seconds reset*/ 0.0,
			/*integrated progress reset*/ 0.0f,
			LowDirectionProgressFloor));
	TestTrue(TEXT("Sustained hysteresis-band travel eventually confirms"),
		IsMotionDirectionCandidateConfirmed(
			/*currently coherent*/ true,
			/*coherent seconds*/ 3.20,
			/*integrated progress*/ 25.6f,
			LowDirectionProgressFloor));
	TestFalse(TEXT("Sustained hysteresis-band travel cannot bounded-settle"),
		CanUseBoundedMotionSettling(
			/*had coherence loss*/ true,
			/*escaped*/ true,
			/*confirmed*/ true,
			/*current direction stable*/ true,
			/*episode seconds*/ 3.20,
			/*angular quiet*/ true));
	TestEqual(TEXT("Rotation continuation contributes progress"),
		MotionDirectionCandidateProgressSpeed(
			FVector::ZeroVector, /*rotation only*/ true,
			FVector::ZeroVector, 0.0f, 0,
			/*qualified angular speed*/ 8.0f),
		8.0f);
	TestEqual(TEXT("Directionless merge uses qualified scalar progress"),
		MotionDirectionCandidateProgressSpeed(
			FVector::ZeroVector, /*rotation only*/ false,
			FVector::ZeroVector,
			/*two opposing qualified members*/ 16.0f, 2, 0.0f),
		8.0f);
	TestEqual(TEXT("Stale unqualified member cannot inflate progress"),
		MotionDirectionCandidateProgressSpeed(
			FVector(0.0f, 0.0f, -1.0f), /*rotation only*/ false,
			/*only the valid continuation*/ FVector(0.0f, 0.0f, -8.0f),
			/*only the valid continuation*/ 8.0f, 1, 0.0f),
		8.0f);
	TestTrue(TEXT("Low contains the captured 55 cm endpoint orbit"),
		IsInsideMotionSettlingEnvelope(
			FVector::ZeroVector, FQuat::Identity,
			FVector(44.0f, 33.0f, 0.0f), FQuat::Identity,
			Low));
	TestFalse(TEXT("Low settling episode detects travel beyond 60 cm"),
		IsInsideMotionSettlingEnvelope(
			FVector::ZeroVector, FQuat::Identity,
			FVector(60.1f, 0.0f, 0.0f), FQuat::Identity,
			Low));
	TestTrue(TEXT("Low latch release margin contains 69 cm"),
		IsInsideMotionSettlingEnvelope(
			FVector::ZeroVector, FQuat::Identity,
			FVector(69.0f, 0.0f, 0.0f), FQuat::Identity,
			Low, MotionSettlingReleaseMultiplier));
	TestFalse(TEXT("Low latch releases travel beyond 70 cm"),
		IsInsideMotionSettlingEnvelope(
			FVector::ZeroVector, FQuat::Identity,
			FVector(70.1f, 0.0f, 0.0f), FQuat::Identity,
			Low, MotionSettlingReleaseMultiplier));
	TestTrue(TEXT("Angular envelope uses the shortest quaternion arc"),
		IsInsideMotionSettlingEnvelope(
			FVector::ZeroVector,
			FRotator(0.0f, 179.0f, 0.0f).Quaternion(),
			FVector::ZeroVector,
			FRotator(0.0f, -179.0f, 0.0f).Quaternion(),
			Low));
	TestFalse(TEXT("Angular-only travel escapes the Low envelope"),
		IsInsideMotionSettlingEnvelope(
			FVector::ZeroVector, FQuat::Identity,
			FVector::ZeroVector,
			FRotator(0.0f, 60.1f, 0.0f).Quaternion(),
			Low));
	TestFalse(TEXT("One ordinary L-turn is not direction churn"),
		HasMotionSettlingEvidence(
			/*became incoherent*/ false,
			/*direction switches*/ 1,
			/*current direction coherent*/ true,
			/*current direction stable*/ false));
	TestTrue(TEXT("Three-direction churn is settling evidence"),
		HasMotionSettlingEvidence(
			/*became incoherent*/ false,
			/*direction switches*/ 2,
			/*current direction coherent*/ true,
			/*current direction stable*/ false));
	TestTrue(TEXT("A quiet candidate is settling evidence"),
		HasMotionSettlingEvidence(
			/*became incoherent*/ true,
			/*direction switches*/ 0,
			/*current direction coherent*/ false,
			/*current direction stable*/ false));
	TestFalse(TEXT("A resumed coherent departure does not inherit quiet"),
		HasMotionSettlingEvidence(
			/*became incoherent*/ true,
			/*direction switches*/ 0,
			/*current direction coherent*/ true,
			/*current direction stable*/ false));
	TestFalse(TEXT("Stable travel does not use churn settling"),
		HasMotionSettlingEvidence(
			/*became incoherent*/ false,
			/*direction switches*/ 2,
			/*current direction coherent*/ true,
			/*current direction stable*/ true));
	TestTrue(TEXT("A different segment rebases an escaped trip episode"),
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ true,
			/*current direction coherent*/ true,
			/*matches current candidate*/ false,
			/*resumed after coherence loss*/ false));
	TestFalse(TEXT("The same sustained direction retains its escape"),
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ true,
			/*current direction coherent*/ true,
			/*matches current candidate*/ true,
			/*resumed after coherence loss*/ false));
	TestTrue(TEXT("The same direction after a quiet gap starts a local episode"),
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ true,
			/*current direction coherent*/ true,
			/*matches current candidate*/ true,
			/*resumed after coherence loss*/ true));
	TestFalse(TEXT("Evidence loss alone does not discard an escaped candidate"),
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ true,
			/*current direction coherent*/ false,
			/*matches current candidate*/ false,
			/*resumed after coherence loss*/ false));
	TestFalse(TEXT("An ordinary local switch remains in its unescaped episode"),
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ false,
			/*current direction coherent*/ true,
			/*matches current candidate*/ false,
			/*resumed after coherence loss*/ false));
	bool bLossPending = true;
	const bool bRebasedBeforeEscape =
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ false,
			/*current direction coherent*/ true,
			/*matches current candidate*/ true,
			/*resumed after coherence loss*/ bLossPending);
	TestFalse(TEXT("A coherent resume inside the envelope does not rebase"),
		bRebasedBeforeEscape);
	if (!bRebasedBeforeEscape)
	{
		// Mirrors the subsystem: the first coherent resume consumes a pre-escape
		// gap. A later uninterrupted escape must therefore remain authoritative.
		bLossPending = false;
	}
	TestFalse(TEXT("A later continuous escape does not inherit an old gap"),
		ShouldRebaseEscapedMotionEpisode(
			/*episode active*/ true,
			/*escaped*/ true,
			/*current direction coherent*/ true,
			/*matches current candidate*/ true,
			/*resumed after coherence loss*/ bLossPending));
	const bool bLocalEndpointChurn = HasMotionSettlingEvidence(
		/*became incoherent*/ false,
		/*direction switches after rebase*/ 2,
		/*current direction coherent*/ true,
		/*current direction stable*/ false);
	TestTrue(TEXT("Rebased endpoint churn can settle inside its local envelope"),
		CanUseBoundedMotionSettling(
			bLocalEndpointChurn,
			/*escaped after rebase*/ false,
			/*confirmed*/ false,
			/*current direction stable*/ false,
			/*episode seconds*/ 0.50,
			/*angular quiet*/ true));
	TestFalse(TEXT("Bounded rebound waits for the normal settling interval"),
		CanUseBoundedMotionSettling(
			/*had coherence loss*/ true,
			/*escaped*/ false,
			/*confirmed*/ false,
			/*current direction stable*/ false,
			/*episode seconds*/ 0.49,
			/*angular quiet*/ true));
	TestTrue(TEXT("Bounded rebound settles after its candidate loses coherence"),
		CanUseBoundedMotionSettling(
			/*had coherence loss*/ true,
			/*escaped*/ false,
			/*confirmed*/ false,
			/*current direction stable*/ false,
			/*episode seconds*/ 0.50,
			/*angular quiet*/ true));
	TestFalse(TEXT("A stable real turn defers bounded settling"),
		CanUseBoundedMotionSettling(
			/*had coherence loss*/ true,
			/*escaped*/ false,
			/*confirmed*/ false,
			/*current direction stable*/ true,
			/*episode seconds*/ 1.0,
			/*angular quiet*/ true));
	TestFalse(TEXT("Rotation prevents translation-only bounded settling"),
		CanUseBoundedMotionSettling(
			/*had coherence loss*/ true,
			/*escaped*/ false,
			/*confirmed*/ false,
			/*current direction stable*/ false,
			/*episode seconds*/ 1.0,
			/*angular quiet*/ false));
	TestFalse(TEXT("Motion outside the envelope remains moving"),
		CanUseBoundedMotionSettling(
			/*had coherence loss*/ true,
			/*escaped*/ true,
			/*confirmed*/ false,
			/*current direction stable*/ false,
			/*episode seconds*/ 1.0,
			/*angular quiet*/ true));
	const bool bSustainedReversalConfirmed =
		IsMotionDirectionCandidateConfirmed(
			/*currently coherent*/ true,
			/*coherent seconds*/ 0.25,
			/*integrated progress*/ LowDirectionProgressFloor,
			LowDirectionProgressFloor);
	TestTrue(TEXT("A sustained reversal exiting the envelope confirms"),
		bSustainedReversalConfirmed);
	if (bSustainedReversalConfirmed)
	{
		SemanticVelocity = ClearDownwardReversal;
	}
	TestEqual(TEXT("A clear reversal updates the active direction"),
		MotionStateValue(true, SemanticVelocity, false,
			FTransform::Identity, FVector::ZeroVector), TEXT("Downward"));
	TestFalse(TEXT("Another member's clear motion cannot authorize a weak reversal"),
		HasCoherentClearLinearDirectionEvidence(
			/*tiny merged reversal*/ FVector(0.0f, 0.0f, -6.0f),
			/*unrelated clear member*/ FVector(20.0f, 0.0f, 0.0f),
			/*clear members*/ 1, /*active members*/ 2));
	TestTrue(TEXT("Aligned clear member evidence authorizes the merged reversal"),
		HasCoherentClearLinearDirectionEvidence(
			FVector(0.0f, 0.0f, -12.0f),
			FVector(0.0f, 0.0f, -20.0f),
			/*clear members*/ 1, /*active members*/ 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMotionTrendTest,
	"Convai.DynamicContext.Spatial.MotionTrend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMotionTrendTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiSpatial;
	const FQuat Identity = FQuat::Identity;

	const FMotionTrend TooShort = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector(1.0f, 0.0f, 0.0f), Identity, 0.25,
		/*MaxLinearExcursion*/ 1.0f, /*MaxAngularExcursion*/ 0.0f);
	TestFalse(TEXT("A partial trend window cannot confirm a stop"), TooShort.bReady);
	TestEqual(TEXT("A partial trend still measures coherent progress"),
		TooShort.LinearVelocity, FVector(4.0f, 0.0f, 0.0f));

	// At a fast custom poll cadence, alternating 0 / 0.6 cm samples each look
	// like 6 cm/s instantaneously. Their partial net trend alternates between
	// progress and no progress, so they can never confirm a false start.
	const FMotionTrend WarmupProgress = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector(0.6f, 0.0f, 0.0f), Identity, 0.10,
		/*MaxLinearExcursion*/ 0.6f, /*MaxAngularExcursion*/ 0.0f);
	const FMotionTrend WarmupReturn = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector::ZeroVector, Identity, 0.20,
		/*MaxLinearExcursion*/ 0.6f, /*MaxAngularExcursion*/ 0.0f);
	TestTrue(TEXT("Coherent warm-up progress can begin start confirmation"),
		IsMotionStartEvidence(/*raw above start*/ true, WarmupProgress));
	TestFalse(TEXT("Warm-up bounce back cancels raw start evidence"),
		IsMotionStartEvidence(/*raw above start*/ true, WarmupReturn));
	bool bWarmupMoving = false;
	double WarmupCandidateSince = -1.0;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const bool bProgressPoll = Index % 2 == 0;
		const FMotionTrend& WarmupTrend = bProgressPoll
			? WarmupProgress : WarmupReturn;
		TestEqual(TEXT("Bounded warm-up jitter emits no edge"),
			UpdateMotionState(
				IsMotionStartEvidence(/*raw above start*/ true, WarmupTrend),
				/*below stop*/ false, 0.10 * (Index + 1),
				bWarmupMoving, WarmupCandidateSince,
				MotionStartConfirmationSeconds, /*stop confirmation*/ 0.0),
			EMotionTransition::None);
	}
	TestFalse(TEXT("Bounded warm-up jitter never starts"), bWarmupMoving);

	// Exercise the exact runtime accumulator: every raw delta is 6 cm/s, but the
	// platform only bounces within 1.5 cm and returns to its window origin.
	struct FTestTrendSample
	{
		FVector Location;
		FQuat Rotation;
		double TimeSeconds;
	};
	TArray<FTestTrendSample> RollingSamples;
	RollingSamples.Add({FVector::ZeroVector, Identity, 0.0});
	RollingSamples.Add({FVector(1.5f, 0.0f, 0.0f), Identity, 0.25});
	const FMotionTrend PartialRollingTrend =
		ComputeRollingMotionTrend(RollingSamples);
	TestFalse(TEXT("Rolling accumulator waits for its full window"),
		PartialRollingTrend.bReady);
	RollingSamples.Add({FVector::ZeroVector, Identity, 0.50});
	const FMotionTrend SettlingJitter =
		ComputeRollingMotionTrend(RollingSamples);
	TestTrue(TEXT("Bounded collision jitter settles"),
		IsMotionTrendQuiet(SettlingJitter));
	TestTrue(TEXT("Settling jitter supplies no semantic direction"),
		SettlingJitter.LinearVelocity.Size() <= MotionStopLinearSpeed);

	bool bMoving = true;
	double CandidateSince = -1.0;
	TestEqual(TEXT("The trend window itself confirms the stop without another delay"),
		UpdateMotionState(/*above start*/ false,
			/*below stop*/ IsMotionTrendQuiet(SettlingJitter),
			0.50, bMoving, CandidateSince,
			MotionStartConfirmationSeconds, /*stop confirmation*/ 0.0),
		EMotionTransition::Stopped);
	TestFalse(TEXT("Stopped after the settling window"), bMoving);

	// A bounded bounce can exceed the raw 5 cm/s start threshold every poll. A
	// ready quiet trend must veto those spikes or the state flaps forever.
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const double PollTime = 0.75 + 0.25 * Index;
		const FVector JitterLocation = Index % 2 == 0
			? FVector(1.5f, 0.0f, 0.0f) : FVector::ZeroVector;
		RollingSamples.Add({JitterLocation, Identity, PollTime});
		const FMotionTrend PostStopTrend =
			ComputeRollingMotionTrend(RollingSamples);
		TestTrue(TEXT("Every post-stop rolling window remains quiet"),
			IsMotionTrendQuiet(PostStopTrend));
		if (Index == 0)
		{
			TestEqual(TEXT("Rolling window retains the boundary sample"),
				RollingSamples[0].TimeSeconds, 0.25);
		}
		TestFalse(TEXT("Quiet trend vetoes a raw collision-spike start"),
			IsMotionStartEvidence(/*raw above start*/ true, PostStopTrend));
		TestEqual(TEXT("Post-stop jitter emits no new edge"),
			UpdateMotionState(
				IsMotionStartEvidence(/*raw above start*/ true, PostStopTrend),
				/*below stop*/ true, PollTime, bMoving, CandidateSince,
				MotionStartConfirmationSeconds, /*stop confirmation*/ 0.0),
			EMotionTransition::None);
		TestFalse(TEXT("Post-stop jitter remains stopped"), bMoving);
	}

	const FMotionTrend SlowCoherentTravel = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector(0.0f, 0.0f, 1.5f), Identity, 0.50,
		/*MaxLinearExcursion*/ 1.5f, /*MaxAngularExcursion*/ 0.0f);
	TestFalse(TEXT("A coherent 3 cm/s drift remains moving"),
		IsMotionTrendQuiet(SlowCoherentTravel));
	TestEqual(TEXT("Slow coherent direction remains measurable"),
		SlowCoherentTravel.LinearVelocity,
		FVector(0.0f, 0.0f, 3.0f));

	const FMotionTrend GenuineRestart = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector(5.0f, 0.0f, 0.0f), Identity, 0.50,
		/*MaxLinearExcursion*/ 5.0f, /*MaxAngularExcursion*/ 0.0f);
	TestTrue(TEXT("A non-quiet trend permits raw start evidence"),
		IsMotionStartEvidence(/*raw above start*/ true, GenuineRestart));
	TestEqual(TEXT("Genuine restart begins confirmation"),
		UpdateMotionState(
			IsMotionStartEvidence(true, GenuineRestart), /*below stop*/ false,
			1.50, bMoving, CandidateSince,
			MotionStartConfirmationSeconds, /*stop confirmation*/ 0.0),
		EMotionTransition::None);
	TestEqual(TEXT("Genuine restart confirms on the normal cadence"),
		UpdateMotionState(
			IsMotionStartEvidence(true, GenuineRestart), /*below stop*/ false,
			1.75, bMoving, CandidateSince,
			MotionStartConfirmationSeconds, /*stop confirmation*/ 0.0),
		EMotionTransition::Started);
	TestTrue(TEXT("Moving after a genuine restart"), bMoving);

	const FMotionTrend LinearOscillation = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector::ZeroVector, Identity, 0.50,
		/*MaxLinearExcursion*/ 3.0f, /*MaxAngularExcursion*/ 0.0f);
	TestFalse(TEXT("A meaningful oscillation is not mistaken for rest"),
		IsMotionTrendQuiet(LinearOscillation));
	TestEqual(TEXT("Directionless linear oscillation becomes generic Moving"),
		MotionStateValue(true, LinearOscillation.LinearVelocity,
			/*rotation only*/ false, FTransform::Identity, FVector::ZeroVector),
		TEXT("Moving"));

	const FMotionTrend AngularJitter = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector::ZeroVector, FRotator(0.0f, 0.5f, 0.0f).Quaternion(), 0.50,
		/*MaxLinearExcursion*/ 0.0f, /*MaxAngularExcursion*/ 1.0f);
	TestTrue(TEXT("Bounded angular jitter settles"),
		IsMotionTrendQuiet(AngularJitter));

	const FMotionTrend AngularOscillation = ComputeMotionTrend(
		FVector::ZeroVector, Identity, 0.0,
		FVector::ZeroVector, Identity, 0.50,
		/*MaxLinearExcursion*/ 0.0f, /*MaxAngularExcursion*/ 3.0f);
	TestFalse(TEXT("A meaningful rotational oscillation remains moving"),
		IsMotionTrendQuiet(AngularOscillation));
	TestEqual(TEXT("Directionless angular oscillation remains Rotating"),
		MotionStateValue(true, AngularOscillation.LinearVelocity,
			/*rotation only*/ true, FTransform::Identity, FVector::ZeroVector),
		TEXT("Rotating"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMotionPhraseTest,
	"Convai.DynamicContext.Spatial.MotionPhrase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMotionPhraseTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiSpatial;

	const FTransform Observer(FRotator::ZeroRotator, FVector::ZeroVector);
	const FVector Subject(1000.0f, 0.0f, 0.0f);
	TestEqual(TEXT("Toward observer"),
		MotionPhrase(FVector(-100.0f, 0.0f, 0.0f), false, Observer, Subject),
		TEXT("moving toward you"));
	TestEqual(TEXT("Away from observer"),
		MotionPhrase(FVector(100.0f, 0.0f, 0.0f), false, Observer, Subject),
		TEXT("moving away from you"));
	TestEqual(TEXT("Observer-relative right"),
		MotionPhrase(FVector(0.0f, 100.0f, 0.0f), false, Observer, Subject),
		TEXT("moving to your right"));
	TestEqual(TEXT("Slow vertical motion"),
		MotionPhrase(FVector(0.0f, 0.0f, 50.0f), false, Observer, Subject),
		TEXT("moving upward slowly"));
	TestEqual(TEXT("Fast vertical motion"),
		MotionPhrase(FVector(0.0f, 0.0f, -600.0f), false, Observer, Subject),
		TEXT("moving downward quickly"));
	TestEqual(TEXT("Rotation is not given a fake direction"),
		MotionPhrase(FVector::ZeroVector, true, Observer, Subject), TEXT("rotating"));
	TestEqual(TEXT("Incoherent merged motion stays generic"),
		MotionPhrase(FVector::ZeroVector, false, Observer, Subject), TEXT("moving"));
	TestEqual(TEXT("Ordinary stationary objects stay terse"),
		MotionStatusPhrase(false, false, FVector::ZeroVector, false, Observer, Subject),
		TEXT(""));
	TestEqual(TEXT("Opted-in stationary objects state the confirmed stop"),
		MotionStatusPhrase(false, true, FVector::ZeroVector, false, Observer, Subject),
		TEXT("stopped"));
	TestEqual(TEXT("Active motion stays automatic without state tracking"),
		MotionStatusPhrase(true, false, FVector(0.0f, 0.0f, 50.0f), false,
			Observer, Subject),
		TEXT("moving upward slowly"));
	TestEqual(TEXT("Movement state keeps upward direction but omits speed"),
		MotionStateValue(true, FVector(0.0f, 0.0f, 50.0f), false,
			Observer, Subject), TEXT("Upward"));
	TestEqual(TEXT("Movement state keeps downward direction but omits speed"),
		MotionStateValue(true, FVector(0.0f, 0.0f, -600.0f), false,
			Observer, Subject), TEXT("Downward"));
	TestEqual(TEXT("Movement state can approach the observer"),
		MotionStateValue(true, FVector(-100.0f, 0.0f, 0.0f), false,
			Observer, Subject), TEXT("Toward You"));
	TestEqual(TEXT("Movement state can move away from the observer"),
		MotionStateValue(true, FVector(100.0f, 0.0f, 0.0f), false,
			Observer, Subject), TEXT("Away"));
	TestEqual(TEXT("Movement state can move to the observer's right"),
		MotionStateValue(true, FVector(0.0f, 100.0f, 0.0f), false,
			Observer, Subject), TEXT("Right"));
	TestEqual(TEXT("Incoherent movement remains generic"),
		MotionStateValue(true, FVector::ZeroVector, false,
			Observer, Subject), TEXT("Moving"));
	TestEqual(TEXT("Rotation remains explicit in the movement state"),
		MotionStateValue(true, FVector::ZeroVector, true,
			Observer, Subject), TEXT("Rotating"));
	TestEqual(TEXT("Stopped state ignores retained direction"),
		MotionStateValue(false, FVector(0.0f, 0.0f, -600.0f), false,
			Observer, Subject), TEXT("Stopped"));
	TestFalse(TEXT("Speed-only change does not churn the movement direction"),
		HasMeaningfulMotionDirectionChange(
			FVector(0.0f, 0.0f, 100.0f), false,
			FVector(0.0f, 0.0f, 600.0f), false));
	TestTrue(TEXT("A quick reversal refreshes direction before the next cadence"),
		HasMeaningfulMotionDirectionChange(
			FVector(0.0f, 0.0f, 100.0f), false,
			FVector(0.0f, 0.0f, -100.0f), false));
	TestFalse(TEXT("A small turn waits and accumulates against the semantic anchor"),
		HasMeaningfulMotionDirectionChange(
			FVector(100.0f, 0.0f, 0.0f), false,
			FRotator(0.0f, 5.0f, 0.0f).RotateVector(
				FVector(100.0f, 0.0f, 0.0f)), false));
	TestTrue(TEXT("Accumulated curvature eventually refreshes direction"),
		HasMeaningfulMotionDirectionChange(
			FVector(100.0f, 0.0f, 0.0f), false,
			FRotator(0.0f, 15.0f, 0.0f).RotateVector(
				FVector(100.0f, 0.0f, 0.0f)), false));
	TestTrue(TEXT("Rotation-to-translation mode change refreshes immediately"),
		HasMeaningfulMotionDirectionChange(
			FVector::ZeroVector, true, FVector(100.0f, 0.0f, 0.0f), false));
	TestTrue(TEXT("Normal confirmed stop remains a gameplay edge"),
		IsGameplayMotionTransition(EMotionTransition::Stopped,
			/*bReinitializedThisPoll*/ false,
			/*bAwaitingPostReinitMotion*/ false));
	TestFalse(TEXT("Reinit poll cannot publish a gameplay edge"),
		IsGameplayMotionTransition(EMotionTransition::Started,
			/*bReinitializedThisPoll*/ true,
			/*bAwaitingPostReinitMotion*/ false));
	TestFalse(TEXT("Stationary target after moving reinit is a baseline stop"),
		IsGameplayMotionTransition(EMotionTransition::Stopped,
			/*bReinitializedThisPoll*/ false,
			/*bAwaitingPostReinitMotion*/ true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiSpatialFactWordingTest,
	"Convai.DynamicContext.Spatial.FactWording",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiSpatialFactWordingTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiSpatial;
	const FString Direction = TEXT("to your left");
	TestEqual(TEXT("Nearby states positive walk reachability"),
		BandPhrase(EProximityBand::Nearby, Direction),
		TEXT("close by, to your left, reachable by walking"));
	TestEqual(TEXT("Moderate states positive walk reachability"),
		BandPhrase(EProximityBand::Moderate, Direction),
		TEXT("some distance away, to your left, reachable by walking"));
	TestEqual(TEXT("Far states positive walk reachability"),
		BandPhrase(EProximityBand::Far, Direction),
		TEXT("far away, to your left, reachable by walking"));
	TestEqual(TEXT("Unreachable keeps the explicit negative verdict"),
		BandPhrase(EProximityBand::Unreachable, Direction),
		TEXT("to your left, with no walking path there"));
	TestEqual(TEXT("Support reachability does not repeat the subject"),
		ComposeSupportClause(TEXT("underneath you"), TEXT(""), true, true),
		TEXT("underneath you and is reachable by walking"));
	TestEqual(TEXT("Support stop and reachability form one predicate list"),
		ComposeSupportClause(TEXT("underneath you"), TEXT("stopped"), true, true),
		TEXT("underneath you, stopped, and is reachable by walking"));
	TestEqual(TEXT("Support motion and reachability form one predicate list"),
		ComposeSupportClause(TEXT("underneath you"), TEXT("moving upward"), true, true),
		TEXT("underneath you, moving upward, and is reachable by walking"));
	TestEqual(TEXT("Blocked support keeps the established path wording"),
		ComposeSupportClause(TEXT("underneath you"), TEXT("stopped"), true, false),
		TEXT("underneath you and stopped, but there is no walking path there"));
	TestEqual(TEXT("Grouped anchors remain separate from reachability"),
		ComposeSupportClause(TEXT("underneath you and Player"), TEXT(""), true, true),
		TEXT("underneath you and Player, and is reachable by walking"));
	TestEqual(TEXT("Arrival omits walking verdict but keeps the stop"),
		ComposeSupportClause(TEXT("underneath you"), TEXT("stopped"), false, false),
		TEXT("underneath you and stopped"));

	FDirection ThreeAxis;
	ThreeAxis.Forward = EForwardDir::Front;
	ThreeAxis.Lateral = ELateralDir::Right;
	ThreeAxis.Vertical = EVerticalDir::Above;
	TestEqual(TEXT("Three-axis direction uses unambiguous punctuation"),
		DirectionToYou(ThreeAxis),
		TEXT("in front of you, to your right, and above you"));
	TestEqual(TEXT("Reached object uses arrival rather than path wording"),
		AlreadyThereSentence(TEXT("the Crate"), /*bIsPerson*/ false),
		TEXT("You are already at the Crate."));
	TestEqual(TEXT("Reached person uses with wording"),
		AlreadyThereSentence(TEXT("Alice"), /*bIsPerson*/ true),
		TEXT("You are already with Alice."));
	TestEqual(TEXT("Reached object with context names the destination once"),
		AlreadyThereFact(TEXT("the Moving Platform"), /*bIsPerson*/ false,
			TEXT("underneath you and stopped")),
		TEXT("You are already at the Moving Platform, which is underneath you and stopped."));
	TestEqual(TEXT("Reached person with context uses who"),
		AlreadyThereFact(TEXT("Alice"), /*bIsPerson*/ true, TEXT("facing you")),
		TEXT("You are already with Alice, who is facing you."));
	TestTrue(TEXT("Already-there outranks a failed path verdict"),
		HasUsableDestination(/*bReachable*/ false, /*bAlreadyThere*/ true));
	TestFalse(TEXT("A failed path with no arrival is unusable"),
		HasUsableDestination(/*bReachable*/ false, /*bAlreadyThere*/ false));
	TestTrue(TEXT("A nearby person on a short valid path is already with the observer"),
		IsPersonAlreadyThere(/*bReachable*/ true, /*DirectDistance2DUU*/ 100.0f,
			/*VerticalDistanceUU*/ 20.0f, /*PathLengthUU*/ 130.0));
	TestFalse(TEXT("Raw proximity cannot put an unreachable person with the observer"),
		IsPersonAlreadyThere(/*bReachable*/ false, /*DirectDistance2DUU*/ 50.0f,
			/*VerticalDistanceUU*/ 0.0f, /*PathLengthUU*/ -1.0));
	TestFalse(TEXT("A wall detour prevents the already-with verdict"),
		IsPersonAlreadyThere(/*bReachable*/ true, /*DirectDistance2DUU*/ 50.0f,
			/*VerticalDistanceUU*/ 0.0f, /*PathLengthUU*/ 250.0));
	TestFalse(TEXT("A person beyond the direct arrival radius is not already with the observer"),
		IsPersonAlreadyThere(/*bReachable*/ true, /*DirectDistance2DUU*/ 151.0f,
			/*VerticalDistanceUU*/ 0.0f, /*PathLengthUU*/ 151.0));
	TestFalse(TEXT("A person on another floor is not already with the observer"),
		IsPersonAlreadyThere(/*bReachable*/ true, /*DirectDistance2DUU*/ 50.0f,
			/*VerticalDistanceUU*/ 201.0f, /*PathLengthUU*/ 100.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiMotionDeltaStyleTest,
	"Convai.DynamicContext.Spatial.MotionDeltaStyle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiMotionDeltaStyleTest::RunTest(const FString& Parameters)
{
	const FString Upward = TEXT("Upward");
	const FString Downward = TEXT("Downward");
	const FString Stopped = TEXT("Stopped");
	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Platform.Movement"), Stopped);
	FConvaiPendingContextBatch Batch;
	Batch.StageState(Tracker, TEXT("Platform.Movement"), Upward,
		EC_RunLLMOption::Never, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	TestFalse(TEXT("Started movement includes its prior stopped state"),
		Batch.OmitPreviousValueKeys.Contains(TEXT("Platform.Movement")));
	TestEqual(TEXT("Started edge records Stopped as its prior state"),
		Batch.OldValues[TEXT("Platform.Movement")], Stopped);

	Batch.ClearStaged();
	Batch.StageState(Tracker, TEXT("Platform.Movement"), Stopped,
		EC_RunLLMOption::Auto, /*bOmitPreviousValue*/ false, &Downward,
		/*bPreserveTransition*/ true);
	TestFalse(TEXT("Stopped movement includes its prior moving state"),
		Batch.OmitPreviousValueKeys.Contains(TEXT("Platform.Movement")));
	TestEqual(TEXT("Stopped edge records its prior moving direction"),
		Batch.OldValues[TEXT("Platform.Movement")], Downward);

	// A complete trip can fit inside one debounce window. The final detector
	// edge must replace the pre-batch snapshot so the prompt never says the
	// platform stopped without first moving.
	Tracker.SetState(TEXT("Platform.Movement"), Stopped);
	FConvaiPendingContextBatch RoundTrip;
	RoundTrip.StageState(Tracker, TEXT("Platform.Movement"), Upward,
		EC_RunLLMOption::Auto, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	// The confirmed final edge supplies its real prior direction; preservation
	// stays sticky so the complete trip cannot collapse back to its baseline.
	RoundTrip.StageState(Tracker, TEXT("Platform.Movement"), Stopped,
		EC_RunLLMOption::Never, /*bOmitPreviousValue*/ false, &Upward,
		/*bPreserveTransition*/ true);
	TestEqual(TEXT("Coalesced stop keeps the immediately preceding moving direction"),
		RoundTrip.OldValues[TEXT("Platform.Movement")], Upward);
	TestEqual(TEXT("Coalesced movement keeps its immutable batch baseline"),
		RoundTrip.InitialValues[TEXT("Platform.Movement")], Stopped);
	TestTrue(TEXT("Transition preservation remains sticky until flush"),
		RoundTrip.PreserveTransitionKeys.Contains(TEXT("Platform.Movement")));
	FString FinalValue;
	TestTrue(TEXT("Coalesced final state remains available"),
		Tracker.GetStateValue(TEXT("Platform.Movement"), FinalValue));
	TestEqual(TEXT("Coalesced final value is Stopped"),
		FinalValue, Stopped);
	TestEqual(TEXT("Responsive start remains explained by the final transition"),
		RoundTrip.AggregateRunLLM, EC_RunLLMOption::Auto);

	FConvaiHeldContextLane Lane;
	Lane.HoldState(TEXT("Platform.Movement"), Upward,
		EC_RunLLMOption::Auto, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	Lane.HoldState(TEXT("Platform.Movement"), Stopped,
		EC_RunLLMOption::Never, /*bOmitPreviousValue*/ false, &Upward,
		/*bPreserveTransition*/ true);
	TestEqual(TEXT("Idle-held round trip keeps the final value"),
		Lane.StateValues[TEXT("Platform.Movement")], Stopped);
	TestEqual(TEXT("Idle-held round trip keeps the final edge history"),
		Lane.StatePreviousValueOverrides[TEXT("Platform.Movement")], Upward);
	TestEqual(TEXT("Idle-held round trip retains the responsive start rank"),
		Lane.StateRespond[TEXT("Platform.Movement")], EC_RunLLMOption::Auto);
	TestTrue(TEXT("Idle-held movement keeps the transition-preservation hint"),
		Lane.StatePreserveTransitions.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Idle delivery preserves both transition states"),
		Lane.StateOmitPreviousValue.Contains(TEXT("Platform.Movement")));
	TestTrue(TEXT("Held transition history can satisfy a bare watch on release"),
		ConvaiContextFormat::WouldWatchMatch(TEXT(""),
			Lane.StatePreviousValueOverrides[TEXT("Platform.Movement")],
			Lane.StateValues[TEXT("Platform.Movement")], false));
	Lane.DropStateKey(TEXT("Platform.Movement"));
	TestFalse(TEXT("Held drop removes transition history"),
		Lane.StatePreviousValueOverrides.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Held drop removes transition preservation"),
		Lane.StatePreserveTransitions.Contains(TEXT("Platform.Movement")));

	// Ordinary state churn has no meaningful excursion semantics. If it returns
	// to its pre-batch value, cancel the staged key and its response request.
	FConvaiDynamicContextTracker OrdinaryTracker;
	OrdinaryTracker.SetState(TEXT("Door.State"), TEXT("Closed"));
	FConvaiPendingContextBatch OrdinaryRoundTrip;
	OrdinaryRoundTrip.StageState(OrdinaryTracker, TEXT("Door.State"), TEXT("Open"),
		EC_RunLLMOption::Always);
	OrdinaryRoundTrip.StageState(OrdinaryTracker, TEXT("Door.State"), TEXT("Closed"),
		EC_RunLLMOption::Never);
	TestTrue(TEXT("Ordinary same-batch round trip collapses"),
		OrdinaryRoundTrip.IsEmpty());
	TestEqual(TEXT("Collapsed round trip removes its stale response request"),
		OrdinaryRoundTrip.AggregateRunLLM, EC_RunLLMOption::Never);
	TestFalse(TEXT("Collapsed round trip removes its prior-value snapshot"),
		OrdinaryRoundTrip.OldValues.Contains(TEXT("Door.State")));
	TestFalse(TEXT("Collapsed round trip removes its immutable baseline"),
		OrdinaryRoundTrip.InitialValues.Contains(TEXT("Door.State")));
	TestFalse(TEXT("Collapsed round trip leaves no preservation hint"),
		OrdinaryRoundTrip.PreserveTransitionKeys.Contains(TEXT("Door.State")));

	// Direction refreshes while the object remains moving are ordinary state
	// updates. A same-batch reversal therefore disappears when it has no net
	// effect, unlike the transition-significant Stopped -> Moving -> Stopped trip.
	FConvaiDynamicContextTracker DirectionTracker;
	DirectionTracker.SetState(TEXT("Platform.Movement"), TEXT("Left"));
	FConvaiPendingContextBatch DirectionRoundTrip;
	DirectionRoundTrip.StageState(DirectionTracker, TEXT("Platform.Movement"),
		Upward, EC_RunLLMOption::Never);
	TestEqual(TEXT("Direction shift records the immediately preceding direction"),
		DirectionRoundTrip.OldValues[TEXT("Platform.Movement")], TEXT("Left"));
	DirectionRoundTrip.StageState(DirectionTracker, TEXT("Platform.Movement"),
		TEXT("Left"), EC_RunLLMOption::Never);
	TestTrue(TEXT("Direction-only round trip collapses as an ordinary update"),
		DirectionRoundTrip.IsEmpty());

	FConvaiDynamicContextTracker StartThenDirectionTracker;
	StartThenDirectionTracker.SetState(TEXT("Platform.Movement"), Stopped);
	FConvaiPendingContextBatch StartThenDirection;
	StartThenDirection.StageState(StartThenDirectionTracker,
		TEXT("Platform.Movement"), Upward, EC_RunLLMOption::Auto,
		/*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	StartThenDirection.StageState(StartThenDirectionTracker,
		TEXT("Platform.Movement"), TEXT("Left"), EC_RunLLMOption::Never,
		/*bOmitPreviousValue*/ false, &Stopped);
	TestEqual(TEXT("Silent direction update keeps the confirmed start origin"),
		StartThenDirection.OldValues[TEXT("Platform.Movement")], Stopped);
	TestEqual(TEXT("Silent direction update keeps the start response rank"),
		StartThenDirection.AggregateRunLLM, EC_RunLLMOption::Auto);

	FConvaiHeldContextLane HeldDirectionChange;
	HeldDirectionChange.HoldState(TEXT("Platform.Movement"), Upward,
		EC_RunLLMOption::Auto, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	HeldDirectionChange.HoldState(TEXT("Platform.Movement"), TEXT("Left"),
		EC_RunLLMOption::Never, /*bOmitPreviousValue*/ false, &Stopped);
	TestEqual(TEXT("Held direction shift retains the responsive start"),
		HeldDirectionChange.StateRespond[TEXT("Platform.Movement")],
		EC_RunLLMOption::Auto);
	TestEqual(TEXT("Held direction shift keeps the confirmed start origin"),
		HeldDirectionChange.StatePreviousValueOverrides[TEXT("Platform.Movement")],
		Stopped);
	TestTrue(TEXT("Held direction shift remains transition-significant"),
		HeldDirectionChange.StatePreserveTransitions.Contains(
			TEXT("Platform.Movement")));

	FConvaiDynamicContextTracker ReversalTracker;
	ReversalTracker.SetState(TEXT("Platform.Movement"), Stopped);
	FConvaiPendingContextBatch ReversalThenStop;
	ReversalThenStop.StageState(ReversalTracker, TEXT("Platform.Movement"), Upward,
		EC_RunLLMOption::Auto, false, &Stopped, true);
	ReversalThenStop.StageState(ReversalTracker, TEXT("Platform.Movement"), Downward,
		EC_RunLLMOption::Never);
	ReversalThenStop.StageState(ReversalTracker, TEXT("Platform.Movement"), Stopped,
		EC_RunLLMOption::Never, false, &Downward, true);
	TestEqual(TEXT("A quick reversal reports its final active direction on stop"),
		ReversalThenStop.OldValues[TEXT("Platform.Movement")], Downward);

	FConvaiDynamicContextTracker NewTracker;
	FConvaiPendingContextBatch FirstAppearance;
	FirstAppearance.StageState(NewTracker, TEXT("Synthetic.State"), Upward,
		EC_RunLLMOption::Auto, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	TestFalse(TEXT("Display history does not fabricate an initial canonical key"),
		FirstAppearance.InitialValues.Contains(TEXT("Synthetic.State")));
	TestEqual(TEXT("First appearance may still carry observed transition history"),
		FirstAppearance.OldValues[TEXT("Synthetic.State")], Stopped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiCancelledMotionBatchTest,
	"Convai.DynamicContext.Spatial.CancelledMotionBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiCancelledMotionBatchTest::RunTest(const FString& Parameters)
{
	FConvaiDynamicContextTracker Tracker;
	const FString Stopped = TEXT("Stopped");
	const FString Upward = TEXT("Upward");
	FConvaiPendingContextBatch Batch;
	Batch.StageState(Tracker, TEXT("Platform.Movement"), TEXT("Upward"),
		EC_RunLLMOption::Always, /*bOmitPreviousValue*/ false, &Stopped,
		/*bPreserveTransition*/ true);
	Batch.StageDeclarative(Tracker, TEXT("Object:platform"),
		TEXT("The Platform is moving upward."), EC_RunLLMOption::Always);
	TestEqual(TEXT("Responsive transition raises the aggregate"),
		Batch.AggregateRunLLM, EC_RunLLMOption::Always);

	// Destroying or disabling the object before debounce expires cancels both
	// movement payloads. Their old response request must not survive the drop.
	Batch.DropStateKey(TEXT("Platform.Movement"));
	Batch.DropDeclarativeKey(TEXT("Object:platform"));
	TestEqual(TEXT("Cancelled transition cannot trigger an empty/stale response"),
		Batch.AggregateRunLLM, EC_RunLLMOption::Never);
	TestFalse(TEXT("Cancellation removes movement baseline metadata"),
		Batch.InitialValues.Contains(TEXT("Platform.Movement")));
	TestFalse(TEXT("Cancellation removes transition-preservation metadata"),
		Batch.PreserveTransitionKeys.Contains(TEXT("Platform.Movement")));
	Batch.StageState(Tracker, TEXT("Platform.Movement"), TEXT("Stopped"),
		EC_RunLLMOption::Never, /*bOmitPreviousValue*/ true);
	TestEqual(TEXT("A fresh-session movement baseline stays silent"),
		Batch.AggregateRunLLM, EC_RunLLMOption::Never);

	// Structural reinitialization must undo an in-flight movement edge before
	// silently seeding the new observation baseline. This mirrors the subsystem's
	// reset sequence and protects both the canonical tracker and response rank.
	FConvaiDynamicContextTracker ReinitTracker;
	ReinitTracker.SetState(TEXT("MovingPlatform.Movement"), Stopped);
	FConvaiPendingContextBatch ReinitBatch;
	ReinitBatch.StageState(ReinitTracker, TEXT("MovingPlatform.Movement"),
		TEXT("Upward"), EC_RunLLMOption::Always, false, &Stopped, true);
	const FString RestoredBaseline =
		ReinitBatch.InitialValues[TEXT("MovingPlatform.Movement")];
	ReinitTracker.SetState(TEXT("MovingPlatform.Movement"), RestoredBaseline);
	ReinitBatch.DropStateKey(TEXT("MovingPlatform.Movement"));
	ReinitBatch.StageState(ReinitTracker, TEXT("MovingPlatform.Movement"),
		TEXT("Moving"), EC_RunLLMOption::Never, /*bOmitPreviousValue*/ true);
	TestEqual(TEXT("Reinit replaces a stale edge with a silent baseline"),
		ReinitBatch.AggregateRunLLM, EC_RunLLMOption::Never);
	FString ReinitializedValue;
	TestTrue(TEXT("Reinit tracker retains the movement state"),
		ReinitTracker.GetStateValue(
			TEXT("MovingPlatform.Movement"), ReinitializedValue));
	TestEqual(TEXT("Reinit tracker contains only the new baseline"),
		ReinitializedValue, TEXT("Moving"));

	FConvaiHeldContextLane ReinitHeld;
	ReinitHeld.HoldState(TEXT("MovingPlatform.Movement"), TEXT("Upward"),
		EC_RunLLMOption::Always, false, &Stopped, true);
	ReinitHeld.DropStateKey(TEXT("MovingPlatform.Movement"));
	TestFalse(TEXT("Reinit cancels a held movement response"),
		ReinitHeld.HasWork());

	FConvaiDynamicContextTracker ReinitFactTracker;
	FConvaiPendingContextBatch ReinitFactBatch;
	const FString SpatialKey = TEXT("Object:moving platform");
	ReinitFactBatch.StageDeclarative(ReinitFactTracker, SpatialKey,
		TEXT("The Moving Platform is moving upward."),
		EC_RunLLMOption::Always);
	ReinitFactBatch.StageDeclarative(ReinitFactTracker, SpatialKey,
		TEXT("The Moving Platform is moving."), EC_RunLLMOption::Never);
	TestEqual(TEXT("Ordinary overwrite alone retains the stale fact response"),
		ReinitFactBatch.AggregateRunLLM, EC_RunLLMOption::Always);
	ReinitFactBatch.DropDeclarativeKey(SpatialKey);
	ReinitFactBatch.StageDeclarative(ReinitFactTracker, SpatialKey,
		TEXT("The Moving Platform is moving."), EC_RunLLMOption::Never);
	TestEqual(TEXT("Reinit neutralization makes the current fact silent"),
		ReinitFactBatch.AggregateRunLLM, EC_RunLLMOption::Never);
	FString ReinitializedFact;
	TestTrue(TEXT("Reinit keeps the current spatial fact"),
		ReinitFactTracker.GetDeclarativeValue(SpatialKey, ReinitializedFact));
	TestEqual(TEXT("Reinit fact reflects the new observation baseline"),
		ReinitializedFact, TEXT("The Moving Platform is moving."));

	FConvaiHeldContextLane ReinitHeldFact;
	ReinitHeldFact.HoldDeclarative(SpatialKey,
		TEXT("The Moving Platform is moving upward."),
		EC_RunLLMOption::Always);
	ReinitHeldFact.DropDeclarativeKey(SpatialKey);
	ReinitHeldFact.HoldDeclarative(SpatialKey,
		TEXT("The Moving Platform is moving."), EC_RunLLMOption::Never);
	TestEqual(TEXT("Reinit clears a held spatial-fact wake-up"),
		ReinitHeldFact.DeclarativeRespond[SpatialKey],
		EC_RunLLMOption::Never);

	// Each confirmed movement edge is authoritative. A newer silent start must
	// supersede an older responsive stop instead of inheriting Auto or allowing
	// the stopped payload to flush first.
	FConvaiDynamicContextTracker EdgeTracker;
	const FString EdgeStateKey = TEXT("EdgePlatform.Movement");
	EdgeTracker.SetState(EdgeStateKey, TEXT("Upward"));
	FConvaiPendingContextBatch PendingStateEdge;
	PendingStateEdge.StageState(EdgeTracker, EdgeStateKey, Stopped,
		EC_RunLLMOption::Auto, false, &Upward, true);
	TestTrue(TEXT("Pending stop is responsive before supersession"),
		PendingStateEdge.StateRequestsResponse(EdgeStateKey));
	TestTrue(TEXT("Authoritative start withdraws the pending stop"),
		PendingStateEdge.WithdrawStateForHold(EdgeTracker, EdgeStateKey));
	PendingStateEdge.StageState(EdgeTracker, EdgeStateKey, Upward,
		EC_RunLLMOption::Never, false, &Stopped, true);
	TestEqual(TEXT("Pending stop Auto does not leak onto the newer start"),
		PendingStateEdge.AggregateRunLLM, EC_RunLLMOption::Never);
	TestEqual(TEXT("New pending state edge is the silent start"),
		PendingStateEdge.StateRespond[EdgeStateKey], EC_RunLLMOption::Never);

	FConvaiDynamicContextTracker WatchedEdgeTracker;
	WatchedEdgeTracker.SetState(EdgeStateKey, Upward);
	FConvaiPendingContextBatch WatchedPendingStop;
	WatchedPendingStop.StageState(WatchedEdgeTracker, EdgeStateKey, Stopped,
		EC_RunLLMOption::Always, false, &Upward, true);
	WatchedPendingStop.WatchPromotedStateKeys.Add(EdgeStateKey);
	bool bInheritedWatchPromotion = false;
	TestTrue(TEXT("Authoritative edge withdraws a watch-promoted stop"),
		WatchedPendingStop.WithdrawStateForHold(
			WatchedEdgeTracker, EdgeStateKey, &bInheritedWatchPromotion));
	TestTrue(TEXT("Consumed watch promotion is returned to the replacement edge"),
		bInheritedWatchPromotion);
	WatchedPendingStop.StageState(WatchedEdgeTracker, EdgeStateKey, Upward,
		bInheritedWatchPromotion ? EC_RunLLMOption::Always : EC_RunLLMOption::Never,
		false, &Stopped, true);
	if (bInheritedWatchPromotion)
	{
		WatchedPendingStop.WatchPromotedStateKeys.Add(EdgeStateKey);
	}
	TestEqual(TEXT("The fresher edge fulfils the inherited watch promise"),
		WatchedPendingStop.StateRespond[EdgeStateKey],
		EC_RunLLMOption::Always);
	bInheritedWatchPromotion = false;
	TestTrue(TEXT("A third edge withdraws the watch-promoted replacement"),
		WatchedPendingStop.WithdrawStateForHold(
			WatchedEdgeTracker, EdgeStateKey, &bInheritedWatchPromotion));
	TestTrue(TEXT("Watch promotion remains sticky across A to B to C"),
		bInheritedWatchPromotion);
	WatchedPendingStop.StageState(
		WatchedEdgeTracker, EdgeStateKey, TEXT("Downward"),
		bInheritedWatchPromotion ? EC_RunLLMOption::Always : EC_RunLLMOption::Never,
		false, &Upward, true);
	if (bInheritedWatchPromotion)
	{
		WatchedPendingStop.WatchPromotedStateKeys.Add(EdgeStateKey);
	}
	TestEqual(TEXT("Third pending edge still fulfils the consumed watch"),
		WatchedPendingStop.StateRespond[EdgeStateKey],
		EC_RunLLMOption::Always);
	TestTrue(TEXT("Third pending edge retains explicit watch ownership"),
		WatchedPendingStop.WatchPromotedStateKeys.Contains(EdgeStateKey));

	FConvaiHeldContextLane HeldStateEdge;
	HeldStateEdge.HoldState(EdgeStateKey, Stopped,
		EC_RunLLMOption::Auto, false, &Upward, true);
	HeldStateEdge.DropStateKey(EdgeStateKey);
	HeldStateEdge.HoldState(EdgeStateKey, Upward,
		EC_RunLLMOption::Never, false, &Stopped, true);
	TestEqual(TEXT("Held stop Auto does not leak onto the newer start"),
		HeldStateEdge.StateRespond[EdgeStateKey], EC_RunLLMOption::Never);
	TestEqual(TEXT("Held state contains only the authoritative start"),
		HeldStateEdge.StateValues[EdgeStateKey], Upward);

	FConvaiHeldContextLane WatchedHeldEdges;
	WatchedHeldEdges.HoldState(EdgeStateKey, Stopped,
		EC_RunLLMOption::Always, false, &Upward, true,
		/*bWatchPromoted*/ true);
	bool bHeldWatchPromotion = false;
	WatchedHeldEdges.DropStateKey(EdgeStateKey, &bHeldWatchPromotion);
	TestTrue(TEXT("Held replacement inherits the consumed watch"),
		bHeldWatchPromotion);
	WatchedHeldEdges.HoldState(EdgeStateKey, Upward,
		bHeldWatchPromotion ? EC_RunLLMOption::Always : EC_RunLLMOption::Never,
		false, &Stopped, true, bHeldWatchPromotion);
	bHeldWatchPromotion = false;
	WatchedHeldEdges.DropStateKey(EdgeStateKey, &bHeldWatchPromotion);
	TestTrue(TEXT("Held watch remains sticky across A to B to C"),
		bHeldWatchPromotion);
	WatchedHeldEdges.HoldState(EdgeStateKey, TEXT("Downward"),
		bHeldWatchPromotion ? EC_RunLLMOption::Always : EC_RunLLMOption::Never,
		false, &Upward, true, bHeldWatchPromotion);
	TestEqual(TEXT("Third held edge still fulfils the consumed watch"),
		WatchedHeldEdges.StateRespond[EdgeStateKey],
		EC_RunLLMOption::Always);
	TestTrue(TEXT("Third held edge retains explicit watch ownership"),
		WatchedHeldEdges.WatchPromotedStateKeys.Contains(EdgeStateKey));

	FConvaiDynamicContextTracker EdgeFactTracker;
	const FString EdgeFactKey = TEXT("Object:edge platform");
	const FString MovingFact = TEXT("The Edge Platform is moving upward.");
	const FString StoppedFact = TEXT("The Edge Platform is stopped.");
	EdgeFactTracker.SetDeclarative(EdgeFactKey, MovingFact);
	FConvaiPendingContextBatch PendingFactEdge;
	PendingFactEdge.StageDeclarative(
		EdgeFactTracker, EdgeFactKey, StoppedFact, EC_RunLLMOption::Auto);
	TestTrue(TEXT("Authoritative fact start withdraws the pending stop"),
		PendingFactEdge.WithdrawDeclarativeForHold(
			EdgeFactTracker, EdgeFactKey));
	PendingFactEdge.StageDeclarative(
		EdgeFactTracker, EdgeFactKey, MovingFact, EC_RunLLMOption::Never);
	TestEqual(TEXT("Pending stopped fact Auto does not leak onto the newer fact"),
		PendingFactEdge.AggregateRunLLM, EC_RunLLMOption::Never);
	TestEqual(TEXT("New pending fact edge is silent"),
		PendingFactEdge.DeclarativeRespond[EdgeFactKey],
		EC_RunLLMOption::Never);

	FConvaiHeldContextLane HeldFactEdge;
	HeldFactEdge.HoldDeclarative(
		EdgeFactKey, StoppedFact, EC_RunLLMOption::Auto);
	HeldFactEdge.DropDeclarativeKey(EdgeFactKey);
	HeldFactEdge.HoldDeclarative(
		EdgeFactKey, MovingFact, EC_RunLLMOption::Never);
	TestEqual(TEXT("Held stopped fact Auto does not leak onto the newer fact"),
		HeldFactEdge.DeclarativeRespond[EdgeFactKey],
		EC_RunLLMOption::Never);
	TestEqual(TEXT("Held fact contains only the authoritative start"),
		HeldFactEdge.DeclarativeSentences[EdgeFactKey], MovingFact);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConvaiTargetPropertyWatchTest,
	"Convai.DynamicContext.Spatial.TargetPropertyWatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConvaiTargetPropertyWatchTest::RunTest(const FString& Parameters)
{
	using namespace ConvaiContextFormat;
	TestTrue(TEXT("Bare watch matches a genuine change"),
		WouldWatchMatch(TEXT(""), TEXT("Closed"), TEXT("Open"), false));
	TestFalse(TEXT("Non-target change leaves a targeted watch armed"),
		WouldWatchMatch(TEXT("Stopped"), TEXT("Stopped"), TEXT("Moving"), false));
	TestTrue(TEXT("Target matches after an ordinary canonical change"),
		WouldWatchMatch(TEXT("Stopped"), TEXT("Moving"), TEXT("Stopped"), false));
	TestFalse(TEXT("Target equal to canonical does not fire immediately"),
		WouldWatchMatch(TEXT("Stopped"), TEXT("Stopped"), TEXT("Stopped"), false));
	TestTrue(TEXT("Idle-held departure lets a return to canonical satisfy target"),
		WouldWatchMatch(TEXT("Stopped"), TEXT("Stopped"), TEXT("Stopped"), true));
	TestTrue(TEXT("A quoted prompt literal matches its raw multi-word value"),
		WouldWatchMatch(TEXT("\"Waiting For Player\""), TEXT("Idle"),
			TEXT("Waiting For Player"), false));
	TestTrue(TEXT("Non-target edge is recorded as a departure"),
		IsTargetWatchDeparture(TEXT("Stopped"), TEXT("Stopped"), TEXT("Moving")));

	// Generic preserved transitions remain last-edge based. A watched change can
	// reverse inside the debounce window and must still report the watched value,
	// rather than a contradictory Closed (was Closed) delta.
	FConvaiDynamicContextTracker WatchedRoundTripTracker;
	WatchedRoundTripTracker.SetState(TEXT("Door.State"), TEXT("Closed"));
	FConvaiPendingContextBatch WatchedRoundTrip;
	const FString Closed = TEXT("Closed");
	WatchedRoundTrip.StageState(WatchedRoundTripTracker, TEXT("Door.State"),
		TEXT("Open"), EC_RunLLMOption::Always, false, &Closed, true);
	WatchedRoundTrip.StageState(WatchedRoundTripTracker, TEXT("Door.State"),
		TEXT("Closed"), EC_RunLLMOption::Never);
	TestEqual(TEXT("Watched round trip keeps the immediately prior open edge"),
		WatchedRoundTrip.OldValues[TEXT("Door.State")], TEXT("Open"));
	TestEqual(TEXT("Watched round trip retains its response"),
		WatchedRoundTrip.AggregateRunLLM, EC_RunLLMOption::Always);

	FConvaiDynamicContextTracker Tracker;
	Tracker.SetState(TEXT("Door.State"), TEXT("Closed"));
	FConvaiPendingContextBatch Batch;
	Batch.StageState(Tracker, TEXT("Door.State"), TEXT("Closed"),
		EC_RunLLMOption::Always, /*bOmitPreviousValue*/ true,
		/*PreviousValueOverride*/ nullptr,
		/*bPreserveTransition*/ true);
	TestFalse(TEXT("Target return remains staged despite matching canonical"),
		Batch.IsEmpty());
	TestTrue(TEXT("Return-to-canonical target suppresses '(was same value)'"),
		Batch.OmitPreviousValueKeys.Contains(TEXT("Door.State")));

	FConvaiHeldContextLane Lane;
	Lane.HoldDeclarative(TEXT("Object:platform"),
		TEXT("The Platform is moving upward."), EC_RunLLMOption::Always);
	Lane.DropDeclarativeKey(TEXT("Object:platform"));
	Lane.HoldDeclarative(TEXT("Object:platform"),
		TEXT("The Platform is close by."), EC_RunLLMOption::Never);
	TestEqual(TEXT("A reversed edge cannot retain a stale held-fact wake-up"),
		Lane.DeclarativeRespond[TEXT("Object:platform")], EC_RunLLMOption::Never);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // WITH_TESTS
