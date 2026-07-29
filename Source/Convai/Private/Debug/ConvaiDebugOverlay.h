// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Framework/Application/IInputProcessor.h"
#include "ConvaiDefinitions.h"

class UConvaiDebugSubsystem;
class UConvaiChatbotComponent;
class UConvaiObjectComponent;
class SVerticalBox;
enum class EConvaiActionDebugPhase : uint8;

/**
 * Toggles the overlay on Ctrl+Alt+<UConvaiSettings::DebugOverlayKey> and, while
 * the overlay is open, cycles selection on PgUp/PgDn (+Shift for characters).
 * Rules (from design review): never act while a text field has keyboard focus
 * (Ctrl+Alt is AltGr on many layouts). The preprocessor is app-wide: in
 * single-process multi-client PIE the first-registered client handles the
 * chord — per-client toggling goes through the Convai.DebugOverlay console
 * command instead.
 */
class FConvaiDebugInputProcessor : public IInputProcessor
{
public:
	explicit FConvaiDebugInputProcessor(UConvaiDebugSubsystem* InOwner) : Owner(InOwner) {}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& KeyEvent) override;

private:
	TWeakObjectPtr<UConvaiDebugSubsystem> Owner;
};

/**
 * The Convai debug overlay: screen-space markers over every registered Convai
 * object, an action queue over the selected character's head, and a right-side
 * panel with the character's context states (the selected object's rows are
 * highlighted) and proximity rows. Snapshot-first: all panel data is re-pulled
 * from the live registries at the object-poll cadence; the debug feed only
 * drives moment-of-change pulses.
 */
class SConvaiDebugOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SConvaiDebugOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UConvaiDebugSubsystem* InSubsystem);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	void CycleCharacter(int32 Direction);
	void CycleObject(int32 Direction);

private:
	// Snapshot refresh (poll cadence) — rebuilds panel rows + marker set.
	void RefreshData();
	// Per-frame — reprojects marker/queue positions (DPI-corrected) + occlusion.
	void UpdateScreenPositions();

	UWorld* GetWorld() const;
	UConvaiChatbotComponent* GetSelectedChatbot() const;
	UConvaiObjectComponent* GetSelectedObject() const;

	TWeakObjectPtr<UConvaiDebugSubsystem> Subsystem;

	// Selection (weak — components die on level travel).
	TArray<TWeakObjectPtr<UConvaiChatbotComponent>> Chatbots;
	TArray<TWeakObjectPtr<UConvaiObjectComponent>>  Objects;
	int32 SelectedChatbotIdx = 0;
	int32 SelectedObjectIdx  = 0;

	// One marker per visible object, keyed by component — plus one per NAMED
	// movement point (PointIndex set), so the addressable name floats over the
	// exact spot it targets.
	struct FMarker
	{
		TWeakObjectPtr<UConvaiObjectComponent> Object;
		int32 PointIndex = INDEX_NONE;
		SConstraintCanvas::FSlot* Slot = nullptr;
		TSharedPtr<SWidget> Widget;
		TSharedPtr<class STextBlock> Glyph;
		TSharedPtr<class STextBlock> Label;
	};
	TArray<FMarker> Markers;

	// Over-head action queue (selected character) + the completion chip that
	// flashes above it (green fade on success; red, growing fade on fail/abort).
	SConstraintCanvas::FSlot* QueueSlot = nullptr;
	TSharedPtr<SWidget> QueueRoot;
	TSharedPtr<SVerticalBox> QueueBox;
	TSharedPtr<class SBorder> CompletionChip;
	TSharedPtr<class STextBlock> CompletionText;
	float CompletionRemaining = 0.0f;
	float CompletionDuration = 1.0f;
	bool bCompletionFailed = false;

	TSharedPtr<SConstraintCanvas> Canvas;
	TSharedPtr<SVerticalBox> PanelStatesBox;
	TSharedPtr<SVerticalBox> PanelFactsBox;
	TSharedPtr<SVerticalBox> PanelProximityBox;
	TSharedPtr<SVerticalBox> PanelEventsBox;
	// FACTS/EVENTS collapse entirely when empty — dead headers are noise.
	TSharedPtr<SWidget> FactsSection;
	TSharedPtr<SWidget> EventsSection;
	TSharedPtr<class STextBlock> PanelHeaderText;

	// Persistent panel rows, updated in place — rebuilding rows every refresh
	// caused visible flicker (old + new rows painted within one frame).
	struct FPanelRow
	{
		TSharedPtr<SWidget> Row;
		TSharedPtr<class STextBlock> Text;
		TSharedPtr<class STextBlock> Dot; // states only
	};
	TMap<FString, FPanelRow> StateRows;
	TMap<FString, FPanelRow> FactRows;
	TMap<FString, FPanelRow> ProximityRows;
	// Events are positional (strings repeat), so rows pair by index, not key.
	TArray<TSharedPtr<class STextBlock>> EventRows;

	/** Signature of the queue content last built — rebuild only on change. */
	FString QueueSignature;

	float RefreshAccumulator = 1.0f; // refresh immediately on first tick

	// key -> pulse remaining + tint (decays in Tick; floor re-trigger 1 s). The
	// tint encodes the state's ShouldRespond: grey Never, amber Auto, red Always.
	struct FPulse
	{
		float Remaining = 0.0f;
		FLinearColor Color = FLinearColor(0.55f, 0.58f, 0.60f); // = style TextDim
	};
	TMap<FString, FPulse> StatePulses;
	void OnStateDebug(UConvaiChatbotComponent* Chatbot, const FString& Key,
		const FString& Value, EC_RunLLMOption ShouldRespond, bool bRemoved);
	void OnActionDebug(UConvaiChatbotComponent* Chatbot, const FString& Action,
		EConvaiActionDebugPhase Phase, const FString& Note);
};
