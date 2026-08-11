// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ConvaiDefinitions.h"
#include "ConvaiDebugSubsystem.generated.h"

class UConvaiChatbotComponent;

/** Lifecycle phases the debug overlay animates on. */
enum class EConvaiActionDebugPhase : uint8
{
	Received,   // appended to the queue
	Started,    // became the current action
	Succeeded,
	Failed,
	Aborted,    // queue cleared / sequence aborted
};

/**
 * Per-GameInstance home for the Convai debug overlay's event feed. The
 * overlay is snapshot-first (it rebuilds its panels from the live registries
 * and public component state when opened); these delegates exist ONLY to
 * drive moment-of-change animations — action chips and state pulse dots.
 *
 * Call sites go through GetActive(), which returns null unless an overlay is
 * actually listening, so the taps are a pointer test when the feature is off.
 * Per-instance (not static) so multiple PIE clients never cross-talk.
 */
UCLASS()
class CONVAI_API UConvaiDebugSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The subsystem for this world's game instance, or null when no overlay
	 *  is listening — the one-line guard for every tap call site. */
	static UConvaiDebugSubsystem* GetActive(const UObject* WorldContext);

	DECLARE_MULTICAST_DELEGATE_FourParams(FOnConvaiActionDebug,
		UConvaiChatbotComponent* /*Chatbot*/, const FString& /*Action*/,
		EConvaiActionDebugPhase, const FString& /*Note*/);
	DECLARE_MULTICAST_DELEGATE_FiveParams(FOnConvaiStateDebug,
		UConvaiChatbotComponent* /*Chatbot*/, const FString& /*Key*/,
		const FString& /*Value*/, EC_RunLLMOption, bool /*bRemoved*/);

	/** Action lifecycle pulses (queue chips, results ribbon). */
	FOnConvaiActionDebug OnActionDebug;

	/** Context-state change pulses (panel rows). Spatial facts intentionally
	 *  do NOT feed this — they churn every poll and would strobe the UI. */
	FOnConvaiStateDebug OnStateDebug;

	/** Overlay lifetime: flips the feed on/off. */
	void SetFeedActive(bool bActive) { bFeedActive = bActive; }
	bool IsFeedActive() const { return bFeedActive; }

	//~ UGameInstanceSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Shows/hides the overlay. Refuses in Shipping/Test unless the Convai
	 *  settings opt-in is on. Bound to Ctrl+Alt+<DebugOverlayKey> and the
	 *  `Convai.DebugOverlay` console command. */
	void ToggleOverlay();

	TSharedPtr<class SConvaiDebugOverlay> GetOverlay() const { return Overlay; }

private:
	bool bFeedActive = false;

	/** Alive only while the overlay is on screen. */
	TSharedPtr<class SConvaiDebugOverlay> Overlay;

	/** Alive for the subsystem's lifetime; only acts on the exact chord. */
	TSharedPtr<class FConvaiDebugInputProcessor> InputProcessor;
};
