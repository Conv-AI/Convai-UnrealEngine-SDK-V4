// Copyright 2022 Convai Inc. All Rights Reserved.

#include "Debug/ConvaiDebugOverlay.h"
#include "Debug/ConvaiDebugSubsystem.h"
#include "ConvaiSubsystem.h"
#include "ConvaiContextSubsystem.h"
#include "ConvaiChatbotComponent.h"
#include "ConvaiObjectComponent.h"
#include "Utility/ConvaiContextFormat.h"
#include "ConvaiUtils.h"
#include "../Convai.h"

#include "Engine/World.h"
// FHitResult moved from Engine/EngineTypes.h into its own header in UE 5.1;
// older engines don't pull it in transitively here (same pattern as ConvaiSpatial.cpp).
#if __has_include("Engine/HitResult.h")
#include "Engine/HitResult.h"
#else
#include "Engine/EngineTypes.h"
#endif
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Actor.h"
// Complete APawn definition: the runtime (non-editor) target only
// forward-declares it, and GetPawn()'s result (TObjectPtr<APawn> on 5.8)
// must upcast to const AActor* for FCollisionQueryParams::AddIgnoredActor.
#include "GameFramework/Pawn.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Text/STextBlock.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace ConvaiDebugStyle
{
	// One place for every color/size — dark translucent cards, Convai-green
	// accents; each color pairs with a glyph or word (colorblind rule).
	const FLinearColor CardBg        (0.02f, 0.02f, 0.03f, 0.85f);
	const FLinearColor TextMain      (0.92f, 0.94f, 0.95f);
	const FLinearColor TextDim       (0.55f, 0.58f, 0.60f);
	const FLinearColor Green         (0.00f, 0.90f, 0.45f);
	const FLinearColor Amber         (1.00f, 0.72f, 0.10f);
	const FLinearColor Red           (1.00f, 0.25f, 0.22f);
	const FLinearColor Cyan          (0.20f, 0.85f, 1.00f);
	constexpr float OccludedOpacity  = 0.45f;
	constexpr int32 MaxMarkers       = 12;
	constexpr float PulseSeconds     = 2.0f;
	constexpr float PulseRetriggerFloor = 1.0f;

	FSlateFontInfo Font(int32 Size, bool bBold = false)
	{
		FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
		// Baked outline so glyphs/labels stay readable over bright scenes.
		Info.OutlineSettings.OutlineSize = 1;
		Info.OutlineSettings.OutlineColor = FLinearColor(0, 0, 0, 0.8f);
		return Info;
	}

	const FSlateBrush* CardBrush()
	{
		static FSlateRoundedBoxBrush Brush(CardBg, 8.0f);
		return &Brush;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Input processor
// ─────────────────────────────────────────────────────────────────────────────

bool FConvaiDebugInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& KeyEvent)
{
	UConvaiDebugSubsystem* Sub = Owner.Get();
	if (!Sub)
	{
		return false;
	}

	// AltGr guard: a focused text field means the user is typing — never steal.
	TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget();
	if (Focused.IsValid() && Focused->GetTypeAsString().Contains(TEXT("EditableText")))
	{
		return false;
	}

	const UConvaiSettings* Settings = Convai::Get().GetConvaiSettings();
	const FKey ToggleKey = (Settings && Settings->DebugOverlayKey.IsValid()) ? Settings->DebugOverlayKey : EKeys::K;

	if (KeyEvent.IsControlDown() && KeyEvent.IsAltDown() && KeyEvent.GetKey() == ToggleKey)
	{
		Sub->ToggleOverlay();
		return true;
	}

	// Selection cycling only while the overlay is up.
	if (Sub->IsFeedActive()
		&& (KeyEvent.GetKey() == EKeys::PageUp || KeyEvent.GetKey() == EKeys::PageDown))
	{
		if (TSharedPtr<SConvaiDebugOverlay> OverlayWidget = Sub->GetOverlay())
		{
			const int32 Dir = (KeyEvent.GetKey() == EKeys::PageDown) ? 1 : -1;
			if (KeyEvent.IsShiftDown())
			{
				OverlayWidget->CycleCharacter(Dir);
			}
			else
			{
				OverlayWidget->CycleObject(Dir);
			}
			return true;
		}
	}
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Overlay
// ─────────────────────────────────────────────────────────────────────────────

void SConvaiDebugOverlay::Construct(const FArguments& InArgs, UConvaiDebugSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
	SetCanTick(true);

	ChildSlot
	[
		SNew(SOverlay)

		// World-anchored layer: markers + the over-head action queue.
		+ SOverlay::Slot()
		[
			SAssignNew(Canvas, SConstraintCanvas)
		]

		// Right-docked character panel.
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(0, 40, 16, 40)
		[
			SNew(SBox)
			.WidthOverride(360)
			[
				SNew(SBorder)
				.BorderImage(ConvaiDebugStyle::CardBrush())
				.Padding(12)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
						[
							SAssignNew(PanelHeaderText, STextBlock)
							.Font(ConvaiDebugStyle::Font(14, true))
							.ColorAndOpacity(ConvaiDebugStyle::Cyan)
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
						[
							SNew(STextBlock)
							.Text(INVTEXT("CONTEXT STATES"))
							.Font(ConvaiDebugStyle::Font(9, true))
							.ColorAndOpacity(ConvaiDebugStyle::TextDim)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(PanelStatesBox, SVerticalBox)
						]

#if ConvaiDebugMode
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(FactsSection, SVerticalBox)
							.Visibility(EVisibility::Collapsed)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 4)
							[
								SNew(STextBlock)
								.Text(INVTEXT("FACTS"))
								.Font(ConvaiDebugStyle::Font(9, true))
								.ColorAndOpacity(ConvaiDebugStyle::TextDim)
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(PanelFactsBox, SVerticalBox)
							]
						]
#endif

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 4)
						[
							SNew(STextBlock)
							.Text(INVTEXT("SURROUNDINGS"))
							.Font(ConvaiDebugStyle::Font(9, true))
							.ColorAndOpacity(ConvaiDebugStyle::TextDim)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(PanelProximityBox, SVerticalBox)
						]

#if ConvaiDebugMode
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(EventsSection, SVerticalBox)
							.Visibility(EVisibility::Collapsed)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 4)
							[
								SNew(STextBlock)
								.Text(INVTEXT("EVENTS"))
								.Font(ConvaiDebugStyle::Font(9, true))
								.ColorAndOpacity(ConvaiDebugStyle::TextDim)
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(PanelEventsBox, SVerticalBox)
							]
						]
#endif

					]
				]
			]
		]
	];

	// Over-head queue slot lives on the canvas; content rebuilt on refresh.
	// The completion chip above it animates in Tick and is hidden otherwise.
	Canvas->AddSlot()
		.Expose(QueueSlot)
		.AutoSize(true)
		.Alignment(FVector2D(0.5f, 1.0f))
		[
			SAssignNew(QueueRoot, SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 0, 0, 2)
			[
				SAssignNew(CompletionChip, SBorder)
				.BorderImage(ConvaiDebugStyle::CardBrush())
				.Padding(FMargin(8, 3))
				.Visibility(EVisibility::Collapsed)
				.RenderTransformPivot(FVector2D(0.5f, 0.5f))
				[
					SAssignNew(CompletionText, STextBlock)
					.Font(ConvaiDebugStyle::Font(11, true))
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(QueueBox, SVerticalBox)
			]
		];

	if (UConvaiDebugSubsystem* Sub = Subsystem.Get())
	{
		Sub->OnStateDebug.AddSP(this, &SConvaiDebugOverlay::OnStateDebug);
		Sub->OnActionDebug.AddSP(this, &SConvaiDebugOverlay::OnActionDebug);
	}
}

UWorld* SConvaiDebugOverlay::GetWorld() const
{
	UConvaiDebugSubsystem* Sub = Subsystem.Get();
	UGameInstance* GI = Sub ? Sub->GetGameInstance() : nullptr;
	return GI ? GI->GetWorld() : nullptr;
}

UConvaiChatbotComponent* SConvaiDebugOverlay::GetSelectedChatbot() const
{
	return Chatbots.IsValidIndex(SelectedChatbotIdx) ? Chatbots[SelectedChatbotIdx].Get() : nullptr;
}

UConvaiObjectComponent* SConvaiDebugOverlay::GetSelectedObject() const
{
	return Objects.IsValidIndex(SelectedObjectIdx) ? Objects[SelectedObjectIdx].Get() : nullptr;
}

void SConvaiDebugOverlay::CycleCharacter(int32 Direction)
{
	if (Chatbots.Num() > 0)
	{
		SelectedChatbotIdx = (SelectedChatbotIdx + Direction + Chatbots.Num()) % Chatbots.Num();
		// States and the completion chip belong to the selected character — drop
		// them so the next refresh builds the new character's set cleanly.
		StateRows.Reset();
		PanelStatesBox->ClearChildren();
		StatePulses.Reset();
#if ConvaiDebugMode
		FactRows.Reset();
		PanelFactsBox->ClearChildren();
		EventRows.Reset();
		PanelEventsBox->ClearChildren();
#endif
		CompletionRemaining = 0.0f;
		CompletionChip->SetVisibility(EVisibility::Collapsed);
		RefreshAccumulator = 1.0f; // rebuild next tick
	}
}

void SConvaiDebugOverlay::CycleObject(int32 Direction)
{
	if (Objects.Num() > 0)
	{
		SelectedObjectIdx = (SelectedObjectIdx + Direction + Objects.Num()) % Objects.Num();
		RefreshAccumulator = 1.0f;
	}
}

void SConvaiDebugOverlay::OnStateDebug(UConvaiChatbotComponent* Chatbot, const FString& Key,
	const FString& /*Value*/, EC_RunLLMOption ShouldRespond, bool /*bRemoved*/)
{
	if (Chatbot != GetSelectedChatbot())
	{
		return;
	}
	// Re-trigger floor: a rapidly-mutating key must not strobe its dot. The
	// tint says how the character reacts: grey Never, amber Auto, red Always.
	FPulse& Pulse = StatePulses.FindOrAdd(Key);
	Pulse.Color = ShouldRespond == EC_RunLLMOption::Always ? ConvaiDebugStyle::Red
		: (ShouldRespond == EC_RunLLMOption::Never ? ConvaiDebugStyle::TextDim : ConvaiDebugStyle::Amber);
	if (Pulse.Remaining < ConvaiDebugStyle::PulseSeconds - ConvaiDebugStyle::PulseRetriggerFloor)
	{
		Pulse.Remaining = ConvaiDebugStyle::PulseSeconds;
	}
	RefreshAccumulator = 1.0f; // pick up the new value promptly
}

void SConvaiDebugOverlay::OnActionDebug(UConvaiChatbotComponent* Chatbot, const FString& Action,
	EConvaiActionDebugPhase Phase, const FString& Note)
{
	if (Chatbot != GetSelectedChatbot()
		|| (Phase != EConvaiActionDebugPhase::Succeeded && Phase != EConvaiActionDebugPhase::Failed
			&& Phase != EConvaiActionDebugPhase::Aborted))
	{
		return;
	}
	bCompletionFailed = Phase != EConvaiActionDebugPhase::Succeeded;
	CompletionDuration = bCompletionFailed ? 2.0f : 1.2f; // failures linger
	CompletionRemaining = CompletionDuration;
	const TCHAR* Verdict = Phase == EConvaiActionDebugPhase::Succeeded ? TEXT("done")
		: (Phase == EConvaiActionDebugPhase::Failed ? TEXT("failed") : TEXT("aborted"));
	CompletionText->SetText(FText::FromString(Note.IsEmpty()
		? FString::Printf(TEXT("%s — %s"), *Action, Verdict)
		: FString::Printf(TEXT("%s — %s (%s)"), *Action, Verdict, *Note)));
	RefreshAccumulator = 1.0f; // the queue shrinks — rebuild it promptly
}

void SConvaiDebugOverlay::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	for (auto It = StatePulses.CreateIterator(); It; ++It)
	{
		It.Value().Remaining -= InDeltaTime;
		if (It.Value().Remaining <= 0.0f)
		{
			It.RemoveCurrent();
		}
	}

	// Pulse dots decay smoothly every frame on the persistent rows.
	for (const TPair<FString, FPanelRow>& Pair : StateRows)
	{
		if (Pair.Value.Dot.IsValid())
		{
			const FPulse Pulse = StatePulses.FindRef(Pair.Key);
			const float Alpha = FMath::Clamp(Pulse.Remaining / ConvaiDebugStyle::PulseSeconds, 0.0f, 1.0f);
			FLinearColor DotColor = Pulse.Color;
			DotColor.A = FMath::Max(Alpha, 0.15f);
			Pair.Value.Dot->SetColorAndOpacity(DotColor);
		}
	}

	// Completion chip: fade out; failures also grow so they can't be missed.
	if (CompletionRemaining > 0.0f)
	{
		CompletionRemaining = FMath::Max(CompletionRemaining - InDeltaTime, 0.0f);
		const float Alpha = CompletionRemaining / CompletionDuration;
		FLinearColor ChipColor = bCompletionFailed ? ConvaiDebugStyle::Red : ConvaiDebugStyle::Green;
		ChipColor.A = Alpha;
		CompletionText->SetColorAndOpacity(ChipColor);
		CompletionChip->SetBorderBackgroundColor(FLinearColor(1, 1, 1, Alpha));
		const float ChipScale = bCompletionFailed ? 1.0f + 0.35f * (1.0f - Alpha) : 1.0f;
		CompletionChip->SetRenderTransform(FSlateRenderTransform(FScale2D(ChipScale)));
		CompletionChip->SetVisibility(
			CompletionRemaining > 0.0f ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
	}

	RefreshAccumulator += InDeltaTime;
	const float Interval = UConvaiUtils::GetObjectPollIntervalSeconds();
	if (RefreshAccumulator >= FMath::Max(Interval, 0.1f))
	{
		RefreshAccumulator = 0.0f;
		RefreshData();
	}

	UpdateScreenPositions();
}

void SConvaiDebugOverlay::RefreshData()
{
	UWorld* World = GetWorld();
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	UConvaiSubsystem* ConvaiSub = GI ? GI->GetSubsystem<UConvaiSubsystem>() : nullptr;

	// ── Registries → weak lists (stable order for cycling) ──
	Chatbots.Reset();
	Objects.Reset();
	if (ConvaiSub)
	{
		for (UConvaiChatbotComponent* Bot : ConvaiSub->GetAllChatbotComponents())
		{
			if (IsValid(Bot)) { Chatbots.Add(Bot); }
		}
		for (UConvaiObjectComponent* Obj : ConvaiSub->GetAllObjectComponents())
		{
			if (IsValid(Obj)) { Objects.Add(Obj); }
		}
	}
	SelectedChatbotIdx = Chatbots.Num() > 0 ? SelectedChatbotIdx % Chatbots.Num() : 0;
	SelectedObjectIdx  = Objects.Num()  > 0 ? SelectedObjectIdx  % Objects.Num()  : 0;

	UConvaiChatbotComponent* Bot = GetSelectedChatbot();

	// ── Header ──
	FString Header = TEXT("Convai Debug — no character");
	if (Bot)
	{
		const FString Attention = Bot->EnvironmentData.CurrentAttentionObject.Name;
		Header = FString::Printf(TEXT("%s%s%s"),
			*Bot->CharacterName,
			Bot->GetIsTalking() ? TEXT("  · TALKING") : TEXT("  · idle"),
			Attention.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  · looking at %s"), *Attention));
	}
	PanelHeaderText->SetText(FText::FromString(Header));

	// ── Context states — persistent rows updated in place (no flicker) ──
	{
		TArray<TPair<FString, FString>> States;
		if (Bot)
		{
			Bot->GetContextStatesSnapshot(States);
		}
		TSet<FString> LiveKeys;
		bool bRowSetChanged = false;
		for (const TPair<FString, FString>& Row : States)
		{
			LiveKeys.Add(Row.Key);
			// The selected object's own states highlight cyan — this list IS
			// its tracked properties as the AI sees them (no separate section).
			const UConvaiObjectComponent* SelObj = GetSelectedObject();
			const bool bSelectedObjectRow = SelObj && Row.Key.StartsWith(
				ConvaiContextFormat::SanitizeKey(SelObj->ObjectEntry.Name) + TEXT("."));
			FPanelRow* Existing = StateRows.Find(Row.Key);
			if (!Existing)
			{
				FPanelRow NewRow;
				PanelStatesBox->AddSlot().AutoHeight().Padding(0, 1)
				[
					SAssignNew(NewRow.Row, SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
					[
						SAssignNew(NewRow.Dot, STextBlock)
						.Text(INVTEXT("●"))
						.Font(ConvaiDebugStyle::Font(8))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SAssignNew(NewRow.Text, STextBlock)
						.Font(ConvaiDebugStyle::Font(10))
						.ColorAndOpacity(ConvaiDebugStyle::TextMain)
						.WrapTextAt(300)
					]
				];
				Existing = &StateRows.Add(Row.Key, NewRow);
				bRowSetChanged = true;
			}
			Existing->Text->SetText(FText::FromString(FString::Printf(TEXT("%s : %s%s"),
				*Row.Key, *Row.Value,
				Bot->IsContextKeyPendingFlush(Row.Key) ? TEXT(" (pending flush)") : TEXT(""))));
			Existing->Text->SetColorAndOpacity(bSelectedObjectRow
				? ConvaiDebugStyle::Cyan : ConvaiDebugStyle::TextMain);
		}
		for (auto It = StateRows.CreateIterator(); It; ++It)
		{
			if (!LiveKeys.Contains(It.Key()))
			{
				PanelStatesBox->RemoveSlot(It.Value().Row.ToSharedRef());
				It.RemoveCurrent();
			}
		}
		// Dot colors animate in Tick (pulse decay), not here.
		(void)bRowSetChanged;
	}

	// ── Surroundings rows — every published spatial fact (objects, characters,
	//    players), VERBATIM. No fact = no row: the AI is told nothing. Label
	//    carries the reachability signal so the sentence can stay dim. ──
	{
		TSet<FString> LiveKeys;
		UConvaiContextSubsystem* Ctx = GI ? GI->GetSubsystem<UConvaiContextSubsystem>() : nullptr;
		TArray<UConvaiContextSubsystem::FConvaiDebugSpatialFact> Facts;
		if (Bot && Ctx)
		{
			Ctx->GetDebugSpatialFacts(Bot, Facts);
		}
		for (const UConvaiContextSubsystem::FConvaiDebugSpatialFact& F : Facts)
		{
			LiveKeys.Add(F.Key);
			FPanelRow* Existing = ProximityRows.Find(F.Key);
			if (!Existing)
			{
				FPanelRow NewRow;
				TSharedPtr<SVerticalBox> RowBox;
				PanelProximityBox->AddSlot().AutoHeight().Padding(0, 2)
				[
					SAssignNew(RowBox, SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(NewRow.Dot, STextBlock)
						.Font(ConvaiDebugStyle::Font(10, true))
					]
#if ConvaiDebugMode
					// Verbatim sentences are Convai-dev detail; released builds
					// show only the name + exceptional-state chips.
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(NewRow.Text, STextBlock)
						.Font(ConvaiDebugStyle::Font(9))
						.ColorAndOpacity(ConvaiDebugStyle::TextDim)
						.WrapTextAt(300)
					]
#endif
				];
				NewRow.Row = RowBox;
				Existing = &ProximityRows.Add(F.Key, NewRow);
			}
			// Name alone = reachable; only exceptional states get a chip
			// (cyan reached matches the nav debug-draw's already-there color).
			Existing->Dot->SetText(FText::FromString(
				F.bReached ? F.Name + TEXT(" — reached")
					: (F.bReachable ? F.Name : F.Name + TEXT(" — no path"))));
			Existing->Dot->SetColorAndOpacity(
				F.bReached ? ConvaiDebugStyle::Cyan
					: (F.bReachable ? ConvaiDebugStyle::TextMain : ConvaiDebugStyle::Red));
#if ConvaiDebugMode
			// "(pending flush)" = staged in the debounce batch, not yet online
			// in the chatbot's context backend.
			Existing->Text->SetText(FText::FromString(F.bPendingFlush
				? F.Sentence + TEXT(" (pending flush)") : F.Sentence));
#endif
		}
		for (auto It = ProximityRows.CreateIterator(); It; ++It)
		{
			if (!LiveKeys.Contains(It.Key()))
			{
				PanelProximityBox->RemoveSlot(It.Value().Row.ToSharedRef());
				It.RemoveCurrent();
			}
		}

#if ConvaiDebugMode
		// ── Facts rows — user-authored declaratives, verbatim. The spatial
		//    pass's keys are excluded: those render above with reachability. ──
		{
			TSet<FString> SpatialKeys;
			for (const UConvaiContextSubsystem::FConvaiDebugSpatialFact& F : Facts)
			{
				SpatialKeys.Add(F.Key);
			}
			TSet<FString> LiveFactKeys;
			TArray<TPair<FString, FString>> Declaratives;
			if (Bot)
			{
				Bot->GetContextDeclarativesSnapshot(Declaratives);
			}
			for (const TPair<FString, FString>& Fact : Declaratives)
			{
				if (SpatialKeys.Contains(Fact.Key)) { continue; }
				LiveFactKeys.Add(Fact.Key);
				FPanelRow* Existing = FactRows.Find(Fact.Key);
				if (!Existing)
				{
					FPanelRow NewRow;
					PanelFactsBox->AddSlot().AutoHeight().Padding(0, 1)
					[
						SAssignNew(NewRow.Text, STextBlock)
						.Font(ConvaiDebugStyle::Font(10))
						.ColorAndOpacity(ConvaiDebugStyle::TextMain)
						.WrapTextAt(300)
					];
					NewRow.Row = NewRow.Text;
					Existing = &FactRows.Add(Fact.Key, NewRow);
				}
				Existing->Text->SetText(FText::FromString(Bot->IsContextKeyPendingFlush(Fact.Key)
					? Fact.Value + TEXT(" (pending flush)") : Fact.Value));
			}
			for (auto It = FactRows.CreateIterator(); It; ++It)
			{
				if (!LiveFactKeys.Contains(It.Key()))
				{
					PanelFactsBox->RemoveSlot(It.Value().Row.ToSharedRef());
					It.RemoveCurrent();
				}
			}
			FactsSection->SetVisibility(FactRows.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed);
		}

		// ── Events rows — last 6 committed (chronological), then what the next
		//    flush will add: staged "(pending flush)", ephemeral "(one-shot)"
		//    (heard exactly once, never committed), pending attention text. ──
		{
			constexpr int32 MaxEvents = 6;
			TArray<FString> Lines;
			if (Bot)
			{
				TArray<FString> Committed, Staged, OneShot;
				FString AttentionText;
				Bot->GetContextEventsSnapshot(Committed);
				Bot->GetPendingContextDebugSnapshot(Staged, OneShot, AttentionText);
				if (Committed.Num() > MaxEvents)
				{
					Lines.Add(FString::Printf(TEXT("(+%d earlier)"), Committed.Num() - MaxEvents));
				}
				for (int32 i = FMath::Max(0, Committed.Num() - MaxEvents); i < Committed.Num(); ++i)
				{
					Lines.Add(Committed[i]);
				}
				for (const FString& E : Staged)  { Lines.Add(E + TEXT(" (pending flush)")); }
				for (const FString& E : OneShot) { Lines.Add(E + TEXT(" (one-shot)")); }
				if (!AttentionText.IsEmpty())
				{
					Lines.Add(AttentionText + TEXT(" (pending flush)"));
				}
			}
			while (EventRows.Num() > Lines.Num())
			{
				PanelEventsBox->RemoveSlot(EventRows.Pop().ToSharedRef());
			}
			for (int32 i = 0; i < Lines.Num(); ++i)
			{
				if (!EventRows.IsValidIndex(i))
				{
					TSharedPtr<STextBlock> Row;
					PanelEventsBox->AddSlot().AutoHeight().Padding(0, 1)
					[
						SAssignNew(Row, STextBlock)
						.Font(ConvaiDebugStyle::Font(10))
						.WrapTextAt(300)
					];
					EventRows.Add(Row);
				}
				EventRows[i]->SetText(FText::FromString(Lines[i]));
				// The truncation marker reads quieter than the events themselves.
				EventRows[i]->SetColorAndOpacity(Lines[i].StartsWith(TEXT("(+"))
					? ConvaiDebugStyle::TextDim : ConvaiDebugStyle::TextMain);
			}
			EventsSection->SetVisibility(EventRows.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed);
		}
#endif // ConvaiDebugMode
	}

	// (The selected object's tracked properties are the cyan-highlighted rows
	// in CONTEXT STATES above — a separate section duplicated them.)

	// ── Marker set: (re)build when the object list or named points changed ──
	// One marker per object plus one per NAMED movement point — addressable
	// sub-objects and label-only points alike: the name floats over the exact
	// spot so a designer can see what each point is.
	struct FWantedMarker
	{
		TWeakObjectPtr<UConvaiObjectComponent> Object;
		int32 PointIndex = INDEX_NONE;
		FString Label;
	};
	TArray<FWantedMarker> Wanted;
	for (int32 i = 0; i < Objects.Num() && i < ConvaiDebugStyle::MaxMarkers; ++i)
	{
		if (UConvaiObjectComponent* Obj = Objects[i].Get())
		{
			Wanted.Add({ Objects[i], INDEX_NONE, Obj->ObjectEntry.Name });
		}
	}
	for (int32 i = 0; i < Objects.Num() && Wanted.Num() < ConvaiDebugStyle::MaxMarkers * 2; ++i)
	{
		UConvaiObjectComponent* Obj = Objects[i].Get();
		if (!Obj) { continue; }
		const TArray<FConvaiMovementPoint>& Points = Obj->ObjectEntry.MovementPoints;
		for (int32 p = 0; p < Points.Num() && Wanted.Num() < ConvaiDebugStyle::MaxMarkers * 2; ++p)
		{
			const FString PointName = FConvaiObjectEntry::EffectiveMovementPointName(Points[p]);
			if (Points[p].bEnabled && !PointName.IsEmpty())
			{
				Wanted.Add({ Objects[i], p, PointName });
			}
		}
	}

	bool bRebuildMarkers = Markers.Num() != Wanted.Num();
	for (int32 i = 0; !bRebuildMarkers && i < Markers.Num(); ++i)
	{
		bRebuildMarkers |= Markers[i].Object != Wanted[i].Object
			|| Markers[i].PointIndex != Wanted[i].PointIndex;
	}
	if (bRebuildMarkers)
	{
		for (FMarker& M : Markers)
		{
			if (M.Widget.IsValid()) { Canvas->RemoveSlot(M.Widget.ToSharedRef()); }
		}
		Markers.Reset();
		for (const FWantedMarker& W : Wanted)
		{
			const bool bPointMarker = W.PointIndex != INDEX_NONE;
			FMarker M;
			M.Object = W.Object;
			M.PointIndex = W.PointIndex;
			TSharedPtr<SVerticalBox> Box;
			Canvas->AddSlot()
				.Expose(M.Slot)
				.AutoSize(true)
				.Alignment(FVector2D(0.5f, 1.0f))
				[
					SAssignNew(Box, SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SAssignNew(M.Label, STextBlock)
						.Font(ConvaiDebugStyle::Font(bPointMarker ? 9 : 10, true))
						.ColorAndOpacity(ConvaiDebugStyle::TextMain)
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SAssignNew(M.Glyph, STextBlock)
						.Font(ConvaiDebugStyle::Font(bPointMarker ? 12 : 16, true))
						.Text(bPointMarker ? INVTEXT("◇") : INVTEXT("◆"))
					]
				];
			M.Widget = Box;
			M.Label->SetText(FText::FromString(W.Label));
			Markers.Add(MoveTemp(M));
		}
	}
	else
	{
		// Same marker set — refresh labels in place so a live point rename
		// can't go stale (SetText early-outs on identical text).
		for (int32 i = 0; i < Markers.Num(); ++i)
		{
			Markers[i].Label->SetText(FText::FromString(Wanted[i].Label));
		}
	}
}

void SConvaiDebugOverlay::UpdateScreenPositions()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (!PC || !Viewport)
	{
		return;
	}

	// APlayerController::ProjectWorldLocationToScreen already returns
	// DPI-corrected, viewport-local coordinates — verified empirically on a
	// 200% display: an extra /DPIScale here halves every marker position.
	FVector CamLoc; FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);

	UConvaiChatbotComponent* SelectedBot = GetSelectedChatbot();

	for (FMarker& M : Markers)
	{
		UConvaiObjectComponent* Obj = M.Object.Get();
		AActor* Owner = Obj ? Obj->GetOwner() : nullptr;
		if (!Owner || !M.Slot || !M.Widget.IsValid())
		{
			continue;
		}
		FVector Anchor;
		if (M.PointIndex != INDEX_NONE)
		{
			// Named movement point: the label sits right on its spot.
			if (!Obj->ObjectEntry.MovementPoints.IsValidIndex(M.PointIndex))
			{
				M.Widget->SetVisibility(EVisibility::Collapsed);
				continue;
			}
			Anchor = Obj->ObjectEntry.MovementPoints[M.PointIndex]
				.ResolveWorldLocation(Owner, Obj->GetResolvedComponent()) + FVector(0, 0, 30);
		}
		else
		{
			USceneComponent* Resolved = Obj->GetResolvedComponent();
			Anchor = (Resolved ? Resolved->GetComponentLocation() : Owner->GetActorLocation())
				+ FVector(0, 0, 40);
		}

		// Widget-space projection: handles the viewport's DPI-curve scale in
		// every window state (embedded viewport, maximized, fullscreen) —
		// raw ProjectWorldLocationToScreen only agreed at UI scale 1.0.
		FVector2D WidgetPos;
		const bool bOnScreen = UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(
			PC, Anchor, WidgetPos, /*bPlayerViewportRelative*/ false);
		M.Widget->SetVisibility(bOnScreen ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
		if (!bOnScreen)
		{
			continue;
		}
		M.Slot->SetOffset(FMargin(WidgetPos.X, WidgetPos.Y, 0, 0));

		// Occlusion: dim + desaturate when a wall is between camera and anchor.
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ConvaiDebugOcclusion), false);
		Params.AddIgnoredActor(Owner);
		if (PC->GetPawn()) { Params.AddIgnoredActor(PC->GetPawn()); }
		const bool bOccluded = World->LineTraceSingleByChannel(Hit, CamLoc, Anchor, ECC_Visibility, Params);
		const float Opacity = bOccluded ? ConvaiDebugStyle::OccludedOpacity : 1.0f;

		const bool bSelected = (Obj == GetSelectedObject());
		const bool bDisabled = !Obj->bConvaiEnabled;
		FLinearColor GlyphColor = bDisabled ? ConvaiDebugStyle::TextDim
			: (bSelected ? ConvaiDebugStyle::Cyan : ConvaiDebugStyle::Green);
		GlyphColor.A = Opacity;
		M.Glyph->SetColorAndOpacity(GlyphColor);
		FLinearColor LabelColor = ConvaiDebugStyle::TextMain;
		LabelColor.A = Opacity;
		M.Label->SetColorAndOpacity(LabelColor);
	}

	// Over-head action queue follows the selected character.
	if (QueueSlot && QueueBox.IsValid())
	{
		AActor* BotOwner = SelectedBot ? SelectedBot->GetOwner() : nullptr;
		FVector2D WidgetPos = FVector2D::ZeroVector;
		const bool bShow = BotOwner && UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(
			PC, BotOwner->GetActorLocation() + FVector(0, 0, 120), WidgetPos, false);
		QueueRoot->SetVisibility(bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
		if (bShow)
		{
			QueueSlot->SetOffset(FMargin(WidgetPos.X, WidgetPos.Y, 0, 0));

			// Content rebuilt only when the queue actually changed — rebuilding
			// per frame flickered.
			FString Signature;
			for (int32 i = 0; i < SelectedBot->ActionsQueue.Num() && i < 5; ++i)
			{
				const FConvaiResultAction& A = SelectedBot->ActionsQueue[i];
				Signature += (A.ActionString.IsEmpty() ? A.Action : A.ActionString) + TEXT("|");
			}
			if (Signature != QueueSignature)
			{
				QueueSignature = Signature;
				QueueBox->ClearChildren();
				for (int32 i = 0; i < SelectedBot->ActionsQueue.Num() && i < 5; ++i)
				{
					const FConvaiResultAction& A = SelectedBot->ActionsQueue[i];
					const bool bCurrent = (i == 0);
					QueueBox->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0, 1)
					[
						SNew(SBorder)
						.BorderImage(ConvaiDebugStyle::CardBrush())
						.Padding(FMargin(8, 3))
						[
							SNew(STextBlock)
							.Text(FText::FromString((bCurrent ? TEXT("▶ ") : TEXT("· "))
								+ (A.ActionString.IsEmpty() ? A.Action : A.ActionString)))
							.Font(ConvaiDebugStyle::Font(bCurrent ? 11 : 9, bCurrent))
							.ColorAndOpacity(bCurrent ? ConvaiDebugStyle::TextMain : ConvaiDebugStyle::TextDim)
						]
					];
				}
			}
		}
	}
}
