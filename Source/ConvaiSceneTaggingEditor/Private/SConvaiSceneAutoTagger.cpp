// Copyright Convai. All Rights Reserved.

#include "SConvaiSceneAutoTagger.h"

#include "ConvaiSceneTaggingEditor.h"
#include "ConvaiSceneAutoTaggerSettings.h"
#include "Convai/Convai.h"
#include "ConvaiUtils.h"
#include "ConvaiVisionService.h"
#include "SceneAutoTaggerNativeCoreAdapter.h"
#include "SceneAutoTaggerCapture.h"
#include "SceneAutoTaggerCharacterCatalog.h"
#include "SceneAutoTaggerController.h"
#include "SceneAutoTaggerSetupModel.h"
#include "SceneAutoTaggerViewSelection.h"
#include "SceneAutoTaggerVisionProtocol.h"

#include "Editor.h"
#include "Async/Async.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/PrimitiveComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Selection.h"
#include "Engine/Font.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "ISettingsModule.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpResponse.h"
#include "UEFreeImage.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "RenderCommandFence.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Rendering/SlateResourceHandle.h"
#include "Slate/DeferredCleanupSlateBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/ConvaiStyle.h"
#include "UI/Widgets/SRoundedBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Colors/SSimpleGradient.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "SConvaiSceneAutoTagger"

DEFINE_LOG_CATEGORY_STATIC(LogConvaiSceneAutoTaggerUi, Log, All);

namespace
{
using FCandidatePtr = TSharedPtr<FSceneAutoTaggerCandidate>;
DECLARE_DELEGATE_TwoParams(FOnCandidateBatchSelectionChanged, FCandidatePtr, ECheckBoxState);
DECLARE_DELEGATE_RetVal_TwoParams(FReply, FOnCandidateRowPressed, FCandidatePtr, const FPointerEvent&);
DECLARE_DELEGATE_OneParam(FOnViewDragCommitted, FVector2D);
DECLARE_DELEGATE_OneParam(FOnViewZoom, float);
constexpr TCHAR PrivacyConsentKey[] = TEXT("scene-auto-tagger-convai-vision-v3");

constexpr float SetupWideLayoutThreshold = 1240.0f;
constexpr float SetupPanelGap = 14.0f;
constexpr float SetupHorizontalChrome = 70.0f;
constexpr float CharacterPanelWideFraction = 0.58f;
constexpr float GuidancePanelWideFraction = 0.42f;
constexpr float CharacterGridInnerChrome = 36.0f;
constexpr int32 CharacterGridColumnCount = 3;
constexpr float CharacterCardMinWidth = 120.0f;
constexpr float CharacterCardCaptionHeight = 44.0f;

float CalculateSetupPanelWidth(float SetupWidth, float MinimumContentWidth, float WideFraction)
{
	const float AvailableContentWidth = FMath::Max(MinimumContentWidth, SetupWidth - SetupHorizontalChrome);
	return (AvailableContentWidth - SetupPanelGap)
		* (SetupWidth >= SetupWideLayoutThreshold ? WideFraction : 1.0f);
}

struct FCharacterGridLayout
{
	float Width = 0.0f;
	int32 ColumnCount = CharacterGridColumnCount;
};

FCharacterGridLayout CalculateCharacterGridLayout(float SetupWidth)
{
	FCharacterGridLayout Layout;
	Layout.Width = CalculateSetupPanelWidth(SetupWidth, 460.0f, CharacterPanelWideFraction)
		- CharacterGridInnerChrome;
	return Layout;
}

/**
 * Direct render-target image used only while Edit View is active.
 * FinalColorLDR guarantees useful RGB but not opaque alpha, so ordinary SImage
 * blending can turn a valid live capture gray or translucent. The render target
 * is sRGB; Slate performs its normal texture/display conversion while this leaf
 * ignores only the undefined alpha and keeps the target on the GPU.
 */
class SSceneAutoTaggerLiveCaptureImage final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSceneAutoTaggerLiveCaptureImage)
		: _Image(nullptr)
	{}
		SLATE_ATTRIBUTE(const FSlateBrush*, Image)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Image = InArgs._Image;
		SetCanTick(false);
		bCanSupportFocus = false;
	}

private:
	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		const int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		const bool bParentEnabled) const override
	{
		const FSlateBrush* Brush = Image.Get();
		if (Brush && Brush->DrawAs != ESlateBrushDrawType::NoDrawType)
		{
			ESlateDrawEffect DrawEffects = ESlateDrawEffect::IgnoreTextureAlpha;
			if (!ShouldBeEnabled(bParentEnabled))
			{
				DrawEffects |= ESlateDrawEffect::DisabledEffect;
			}
			const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint()
				* Brush->GetTint(InWidgetStyle);
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(),
				Brush,
				DrawEffects,
				Tint);
		}
		return LayerId;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		const FSlateBrush* Brush = Image.Get();
		return Brush ? FVector2D(Brush->ImageSize.X, Brush->ImageSize.Y) : FVector2D::ZeroVector;
	}

	TAttribute<const FSlateBrush*> Image;
};

/**
 * Smooth two-dimensional shade behind character names. Vertex interpolation
 * keeps the upper edge feathered while concentrating contrast at bottom-left.
 */
class SCharacterNameShade final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCharacterNameShade) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetCanTick(false);
		bCanSupportFocus = false;
	}

private:
	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		const int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		const bool bParentEnabled) const override
	{
		const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
		if (LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
		{
			return LayerId;
		}

		const FSlateBrush* WhiteBrush = FAppStyle::GetBrush("WhiteBrush");
		const FSlateResourceHandle ResourceHandle =
			FSlateApplication::Get().GetRenderer()->GetResourceHandle(*WhiteBrush);
		const FSlateShaderResourceProxy* ResourceProxy = ResourceHandle.GetResourceProxy();
		const FVector2f Uv = ResourceProxy ? FVector2f(ResourceProxy->StartUV) : FVector2f::ZeroVector;
		const FSlateRenderTransform& RenderTransform = AllottedGeometry.GetAccumulatedRenderTransform();

		constexpr int32 ColumnCount = 5;
		constexpr int32 RowCount = 5;
		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;
		Vertices.Reserve(ColumnCount * RowCount);
		Indices.Reserve((ColumnCount - 1) * (RowCount - 1) * 6);

		for (int32 Row = 0; Row < RowCount; ++Row)
		{
			const float Y = static_cast<float>(Row) / static_cast<float>(RowCount - 1);
			for (int32 Column = 0; Column < ColumnCount; ++Column)
			{
				const float X = static_cast<float>(Column) / static_cast<float>(ColumnCount - 1);
				const float Alpha = 0.90f * FMath::Square(Y) * FMath::Square(1.0f - X);
				const FColor Color = FLinearColor(0.01f, 0.07f, 0.11f, Alpha).ToFColor(true);
				Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(
					RenderTransform,
					FVector2f(X * LocalSize.X, Y * LocalSize.Y),
					Uv,
					Color));
			}
		}

		for (int32 Row = 0; Row < RowCount - 1; ++Row)
		{
			for (int32 Column = 0; Column < ColumnCount - 1; ++Column)
			{
				const SlateIndex TopLeft = static_cast<SlateIndex>(Row * ColumnCount + Column);
				const SlateIndex TopRight = static_cast<SlateIndex>(TopLeft + 1);
				const SlateIndex BottomLeft = static_cast<SlateIndex>(TopLeft + ColumnCount);
				const SlateIndex BottomRight = static_cast<SlateIndex>(BottomLeft + 1);
				Indices.Append({TopLeft, TopRight, BottomLeft, TopRight, BottomRight, BottomLeft});
			}
		}

		FSlateDrawElement::MakeCustomVerts(
			OutDrawElements,
			LayerId,
			ResourceHandle,
			Vertices,
			Indices,
			nullptr,
			0,
			0,
			ESlateDrawEffect::None);
		return LayerId;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D(1.0f, 44.0f);
	}
};

namespace SceneAutoTaggerVisual
{
FLinearColor FromHex(const TCHAR* Hex)
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
}

const FLinearColor& Window()
{
	static const FLinearColor Value = FromHex(TEXT("141A20"));
	return Value;
}

const FLinearColor& Header()
{
	static const FLinearColor Value = FromHex(TEXT("11181E"));
	return Value;
}

const FLinearColor& Toolbar()
{
	static const FLinearColor Value = FromHex(TEXT("1B242C"));
	return Value;
}

const FLinearColor& Panel()
{
	static const FLinearColor Value = FromHex(TEXT("202A32"));
	return Value;
}

const FLinearColor& Row()
{
	static const FLinearColor Value = FromHex(TEXT("1B242B"));
	return Value;
}

const FLinearColor& RowHover()
{
	static const FLinearColor Value = FromHex(TEXT("25323B"));
	return Value;
}

const FLinearColor& SelectedRow()
{
	static const FLinearColor Value = FromHex(TEXT("21444E"));
	return Value;
}

const FLinearColor& Preview()
{
	static const FLinearColor Value = FromHex(TEXT("10171C"));
	return Value;
}

const FLinearColor& Border()
{
	static const FLinearColor Value = FromHex(TEXT("33414A"));
	return Value;
}

const FLinearColor& PrimaryText()
{
	static const FLinearColor Value = FromHex(TEXT("EEF2F4"));
	return Value;
}

const FLinearColor& SecondaryText()
{
	static const FLinearColor Value = FromHex(TEXT("AAB5BC"));
	return Value;
}

const FLinearColor& MutedText()
{
	static const FLinearColor Value = FromHex(TEXT("8D999F"));
	return Value;
}

const FLinearColor& Accent()
{
	static const FLinearColor Value = FromHex(TEXT("19B8AA"));
	return Value;
}

const FLinearColor& PrimaryAction()
{
	// A darker Convai-adjacent teal keeps the brand family while preserving
	// readable white labels. The Editor theme's brighter green is reserved for
	// small accents because white text on it is too low-contrast for dense tools.
	static const FLinearColor Value = FromHex(TEXT("0C735F"));
	return Value;
}

const FLinearColor& PrimaryActionHover()
{
	static const FLinearColor Value = FromHex(TEXT("0F826C"));
	return Value;
}

const FLinearColor& PrimaryActionPressed()
{
	static const FLinearColor Value = FromHex(TEXT("095F50"));
	return Value;
}

const FLinearColor& DisabledAction()
{
	static const FLinearColor Value = FromHex(TEXT("2A343B"));
	return Value;
}

const FLinearColor& DisabledText()
{
	static const FLinearColor Value = FromHex(TEXT("89949B"));
	return Value;
}

const FButtonStyle& PrimaryButtonStyle()
{
	static const FButtonStyle Style = FButtonStyle()
		.SetNormal(FSlateRoundedBoxBrush(PrimaryAction(), 4.0f))
		.SetHovered(FSlateRoundedBoxBrush(PrimaryActionHover(), 4.0f))
		.SetPressed(FSlateRoundedBoxBrush(PrimaryActionPressed(), 4.0f))
		.SetDisabled(FSlateRoundedBoxBrush(DisabledAction(), 4.0f, Border(), 1.0f))
		.SetNormalForeground(FSlateColor(PrimaryText()))
		.SetHoveredForeground(FSlateColor(PrimaryText()))
		.SetPressedForeground(FSlateColor(PrimaryText()))
		.SetDisabledForeground(FSlateColor(DisabledText()))
		.SetNormalPadding(FMargin(0.0f))
		.SetPressedPadding(FMargin(0.0f));
	return Style;
}

const FButtonStyle& SecondaryButtonStyle()
{
	static const FButtonStyle Style = FButtonStyle()
		.SetNormal(FSlateRoundedBoxBrush(Row(), 4.0f, Border(), 1.0f))
		.SetHovered(FSlateRoundedBoxBrush(RowHover(), 4.0f, Accent(), 1.0f))
		.SetPressed(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Accent(), 1.0f))
		.SetDisabled(FSlateRoundedBoxBrush(DisabledAction(), 4.0f, Border(), 1.0f))
		.SetNormalForeground(FSlateColor(PrimaryText()))
		.SetHoveredForeground(FSlateColor(PrimaryText()))
		.SetPressedForeground(FSlateColor(PrimaryText()))
		.SetDisabledForeground(FSlateColor(DisabledText()))
		.SetNormalPadding(FMargin(0.0f))
		.SetPressedPadding(FMargin(0.0f));
	return Style;
}

const FCheckBoxStyle& ScopeToggleStyle()
{
	static const FCheckBoxStyle Style = FCheckBoxStyle()
		.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
		.SetUncheckedImage(FSlateRoundedBoxBrush(Row(), 4.0f, Border(), 1.0f))
		.SetUncheckedHoveredImage(FSlateRoundedBoxBrush(RowHover(), 4.0f, Border(), 1.0f))
		.SetUncheckedPressedImage(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Border(), 1.0f))
		.SetCheckedImage(FSlateRoundedBoxBrush(PrimaryAction(), 4.0f))
		.SetCheckedHoveredImage(FSlateRoundedBoxBrush(PrimaryActionHover(), 4.0f))
		.SetCheckedPressedImage(FSlateRoundedBoxBrush(PrimaryActionPressed(), 4.0f))
		.SetPadding(FMargin(0.0f))
		.SetForegroundColor(FSlateColor(SecondaryText()))
		.SetHoveredForegroundColor(FSlateColor(PrimaryText()))
		.SetPressedForegroundColor(FSlateColor(PrimaryText()))
		.SetCheckedForegroundColor(FSlateColor(PrimaryText()))
		.SetCheckedHoveredForegroundColor(FSlateColor(PrimaryText()))
		.SetCheckedPressedForegroundColor(FSlateColor(PrimaryText()));
	return Style;
}

FSlateFontInfo RegularFont(const int32 Size)
{
	FSlateFontInfo Font = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountValue"));
	Font.Size = Size;
	return Font;
}

FSlateFontInfo MediumFont(const int32 Size)
{
	FSlateFontInfo Font = FConvaiStyle::Get().GetFontStyle(TEXT("Convai.Font.accountLabel"));
	Font.Size = Size;
	return Font;
}

FSlateFontInfo CharacterNameFont(const int32 Size)
{
	// This composite font ships with the Convai content and supplies Arabic,
	// Hindi, and CJK fallbacks. Keep the editor font as a safe source fallback.
	static TStrongObjectPtr<UFont> ConvaiCompositeFont(
		LoadObject<UFont>(nullptr, TEXT("/ConvAI/Widgets/Fonts/ConvaiFont.ConvaiFont")));
	return ConvaiCompositeFont.IsValid()
		? FSlateFontInfo(ConvaiCompositeFont.Get(), Size)
		: MediumFont(Size);
}

const FTableRowStyle& CandidateRowStyle()
{
	static const FTableRowStyle Style = FTableRowStyle()
		.SetEvenRowBackgroundBrush(FSlateRoundedBoxBrush(Row(), 4.0f, Border(), 1.0f))
		.SetOddRowBackgroundBrush(FSlateRoundedBoxBrush(Row(), 4.0f, Border(), 1.0f))
		.SetEvenRowBackgroundHoveredBrush(FSlateRoundedBoxBrush(RowHover(), 4.0f, Border(), 1.0f))
		.SetOddRowBackgroundHoveredBrush(FSlateRoundedBoxBrush(RowHover(), 4.0f, Border(), 1.0f))
		.SetActiveBrush(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Accent(), 1.0f))
		.SetActiveHoveredBrush(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Accent(), 1.0f))
		.SetInactiveBrush(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Accent(), 1.0f))
		.SetInactiveHoveredBrush(FSlateRoundedBoxBrush(SelectedRow(), 4.0f, Accent(), 1.0f))
		.SetSelectorFocusedBrush(FSlateNoResource())
		.SetTextColor(FSlateColor(PrimaryText()))
		.SetSelectedTextColor(FSlateColor(PrimaryText()));
	return Style;
}
}

class SSceneAutoTaggerViewInput final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSceneAutoTaggerViewInput) {}
		SLATE_EVENT(FOnViewDragCommitted, OnOrbit)
		SLATE_EVENT(FOnViewDragCommitted, OnPan)
		SLATE_EVENT(FOnViewZoom, OnZoom)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnOrbit = InArgs._OnOrbit;
		OnPan = InArgs._OnPan;
		OnZoom = InArgs._OnZoom;
		bCanSupportFocus = true;
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			.Padding(0.0f)
			.ToolTipText(LOCTEXT(
				"EditViewInputTooltip",
				"Drag to orbit. Shift-drag or right-drag to pan. Use the mouse wheel to zoom. The isolated preview follows the camera while you adjust it."))
		];
	}

	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() != EKeys::LeftMouseButton
			&& Event.GetEffectingButton() != EKeys::RightMouseButton)
		{
			return FReply::Unhandled();
		}
		bPanning = Event.GetEffectingButton() == EKeys::RightMouseButton || Event.IsShiftDown();
		// Slate normally throttles expensive editor work for a handled mouse-down.
		// Edit View's persistent scene capture advances on that editor tick, so
		// throttling here freezes the preview for the entire orbit/pan gesture.
		return FReply::Handled()
			.PreventThrottling()
			.SetUserFocus(SharedThis(this), EFocusCause::Mouse)
			.CaptureMouse(SharedThis(this));
	}

	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override
	{
		if (!HasMouseCapture())
		{
			return FReply::Unhandled();
		}
		const FVector2D DragDelta = Event.GetCursorDelta();
		if (!DragDelta.IsNearlyZero())
		{
			if (bPanning)
			{
				OnPan.ExecuteIfBound(DragDelta);
			}
			else
			{
				OnOrbit.ExecuteIfBound(DragDelta);
			}
		}
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override
	{
		if (!HasMouseCapture())
		{
			return FReply::Unhandled();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}

	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override
	{
		bPanning = false;
		SCompoundWidget::OnMouseCaptureLost(CaptureLostEvent);
	}

	virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& Event) override
	{
		OnZoom.ExecuteIfBound(Event.GetWheelDelta());
		return FReply::Handled();
	}

private:
	FOnViewDragCommitted OnOrbit;
	FOnViewDragCommitted OnPan;
	FOnViewZoom OnZoom;
	bool bPanning = false;
};

TSharedPtr<FSlateDynamicImageBrush> CreateEvidenceBrush(
	const TArray<FColor>& Pixels,
	const int32 Resolution)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Scene Auto Tagger preview textures must be created on the game thread.")))
	{
		return nullptr;
	}
	if (Resolution <= 0 || Pixels.Num() != Resolution * Resolution)
	{
		return nullptr;
	}

	// The evidence is already BGRA8-sRGB. Register it directly with Slate instead
	// of routing it through a transient UTexture2D whose resource can be cached by
	// SImage before the asynchronous texture upload has completed. Captures are
	// intentionally opaque: scene evidence is a finished image, not UI alpha.
	TArray<uint8> OpaqueBGRA;
	OpaqueBGRA.SetNumUninitialized(Pixels.Num() * sizeof(FColor));
	for (int32 PixelIndex = 0; PixelIndex < Pixels.Num(); ++PixelIndex)
	{
		const int32 ByteIndex = PixelIndex * sizeof(FColor);
		OpaqueBGRA[ByteIndex] = Pixels[PixelIndex].B;
		OpaqueBGRA[ByteIndex + 1] = Pixels[PixelIndex].G;
		OpaqueBGRA[ByteIndex + 2] = Pixels[PixelIndex].R;
		OpaqueBGRA[ByteIndex + 3] = 255;
	}

	const FName ResourceName(*FString::Printf(
		TEXT("ConvaiSceneAutoTaggerEvidence_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	return FSlateDynamicImageBrush::CreateWithImageData(
		ResourceName,
		FVector2D(Resolution, Resolution),
		OpaqueBGRA);
}

void ReleaseEvidenceBrush(TSharedPtr<FSlateDynamicImageBrush>& Brush)
{
	if (!Brush.IsValid())
	{
		return;
	}
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
	{
		// Slate draw buffers can outlive the game-thread widget update that replaced
		// this brush. Let the renderer retire the dynamic resource after those
		// buffers are no longer in flight.
		FSlateApplication::Get().GetRenderer()->RemoveDynamicBrushResource(MoveTemp(Brush));
		return;
	}
	Brush.Reset();
}

FText DecisionText(ESceneAutoTaggerDecision Decision)
{
	switch (Decision)
	{
	case ESceneAutoTaggerDecision::Accepted:
		return LOCTEXT("AcceptedDecision", "Included");
	case ESceneAutoTaggerDecision::Rejected:
		return LOCTEXT("RejectedDecision", "Excluded");
	default:
		return LOCTEXT("PendingDecision", "Review");
	}
}

FSlateColor DecisionColor(ESceneAutoTaggerDecision Decision)
{
	switch (Decision)
	{
	case ESceneAutoTaggerDecision::Accepted:
		return FSlateColor(FLinearColor(0.22f, 0.78f, 0.42f));
	case ESceneAutoTaggerDecision::Rejected:
		return FSlateColor(FLinearColor(0.93f, 0.32f, 0.28f));
	default:
		return FSlateColor(FLinearColor(0.95f, 0.72f, 0.22f));
	}
}

FText LowInformationReasonText(const ESceneAutoTaggerLowInformationReason Reason)
{
	switch (Reason)
	{
	case ESceneAutoTaggerLowInformationReason::ObjectNotClearlyVisible:
		return LOCTEXT("LowInformationObjectNotVisible", "Object not clearly visible in capture");
	case ESceneAutoTaggerLowInformationReason::NoUsefulVisualDetail:
		return LOCTEXT("LowInformationNoUsefulDetail", "Capture has little useful visual detail");
	default:
		return LOCTEXT("LowInformationGenericReason", "Capture has too little usable visual detail");
	}
}

FText CandidateStatusText(const FSceneAutoTaggerCandidate& Candidate)
{
	if (!Candidate.Actor.IsValid())
	{
		return LOCTEXT("MissingActorStatus", "Actor missing");
	}
	if (Candidate.bApplied)
	{
		return LOCTEXT("AppliedStatus", "Applied");
	}
	if (!Candidate.Error.IsEmpty())
	{
		return LOCTEXT("AttentionStatus", "Attention");
	}
	if (!ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
		Candidate.RefinementNote).IsEmpty())
	{
		return LOCTEXT("RefinementNeedsAnalysisStatus", "Needs analysis");
	}
	if (Candidate.bUserCapturePendingAnalysis)
	{
		return LOCTEXT("NeedsAnalysisStatus", "Needs analysis");
	}
	if (Candidate.bTagComplete)
	{
		return DecisionText(Candidate.Decision);
	}
	if (Candidate.bAnalysisCancelled)
	{
		return LOCTEXT("NotAnalyzedStatus", "Not analyzed");
	}
	return LOCTEXT("ProcessingStatus", "Processing");
}

FText CandidateToolTip(const FSceneAutoTaggerCandidate& Candidate)
{
	FString Details = Candidate.ActorPath;
	if (!Candidate.AssetSummary.IsEmpty())
	{
		Details += FString::Printf(TEXT("\nAsset: %s"), *Candidate.AssetSummary);
	}
	if (!Candidate.SignificanceReason.IsEmpty())
	{
		Details += FString::Printf(TEXT("\nLocal relevance: %s"), *Candidate.SignificanceReason);
	}
	if (!Candidate.Error.IsEmpty())
	{
		Details += FString::Printf(TEXT("\nAttention: %s"), *Candidate.Error);
	}
	else if (Candidate.bAnalysisCancelled)
	{
		Details += TEXT("\nNot analyzed: exploration stopped before this object was analyzed. Check the row, then use More to analyze or recapture it.");
	}
	const FString RefinementNote =
		ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(Candidate.RefinementNote);
	if (!RefinementNote.IsEmpty())
	{
		Details += FString::Printf(TEXT("\nSaved refinement note: %s"), *RefinementNote);
	}
	return FText::FromString(Details);
}

FText CandidateAccessibleText(const FSceneAutoTaggerCandidate& Candidate)
{
	const FText BaseText = FText::Format(
		LOCTEXT("CandidateAccessibleFormat", "{0}. Source {1}. Confidence {2}. Review state {3}. Status {4}."),
		FText::FromString(Candidate.ActorLabel),
		FText::FromString(LexToString(Candidate.Source)),
		Candidate.bTagComplete && !Candidate.bUserCapturePendingAnalysis
			? FText::AsPercent(Candidate.Confidence)
			: LOCTEXT("UnknownConfidence", "not available"),
		DecisionText(Candidate.Decision),
		CandidateStatusText(Candidate));
	return ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
		Candidate.RefinementNote).IsEmpty()
		? BaseText
		: FText::Format(
			LOCTEXT("CandidateAccessibleWithNoteFormat", "{0} One-time refinement note saved. Analyze this row to use it."),
			BaseText);
}

bool HasValidEvidence(const FSceneAutoTaggerCandidate* Candidate)
{
	if (!Candidate)
	{
		return false;
	}
	const int32 Resolution = Candidate->PreviewSize.X;
	return Resolution > 0
		&& Candidate->PreviewSize.Y == Resolution
		&& static_cast<int64>(Candidate->PreviewBGRA.Num())
			== static_cast<int64>(Resolution) * Resolution * static_cast<int64>(sizeof(FColor));
}

class SSceneAutoTaggerCandidateRow final : public SMultiColumnTableRow<FCandidatePtr>
{
public:
	SLATE_BEGIN_ARGS(SSceneAutoTaggerCandidateRow) {}
		SLATE_ARGUMENT(FCandidatePtr, Candidate)
		SLATE_EVENT(FOnCandidateBatchSelectionChanged, OnBatchSelectionChanged)
		SLATE_EVENT(FOnCandidateRowPressed, OnRowPressed)
		SLATE_ATTRIBUTE(ECheckBoxState, BatchSelectionState)
		SLATE_ATTRIBUTE(bool, CanBatchSelect)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Candidate = InArgs._Candidate;
		OnBatchSelectionChanged = InArgs._OnBatchSelectionChanged;
		OnRowPressed = InArgs._OnRowPressed;
		BatchSelectionState = InArgs._BatchSelectionState;
		CanBatchSelect = InArgs._CanBatchSelect;
		check(Candidate.IsValid());
		SMultiColumnTableRow<FCandidatePtr>::Construct(
			FSuperRowType::FArguments()
			.Padding(FMargin(8.0f, 6.0f))
			.Style(&SceneAutoTaggerVisual::CandidateRowStyle())
			.ToolTipText_Lambda([this]() { return CandidateToolTip(*Candidate); })
			.AccessibleText_Lambda([this]() { return CandidateAccessibleText(*Candidate); }),
			OwnerTable);
	}

	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& PointerEvent) override
	{
		if ((PointerEvent.GetEffectingButton() == EKeys::LeftMouseButton
			|| PointerEvent.GetEffectingButton() == EKeys::RightMouseButton)
			&& OnRowPressed.IsBound())
		{
			FReply SelectionReply = OnRowPressed.Execute(Candidate, PointerEvent);
			if (SelectionReply.IsEventHandled())
			{
				return SelectionReply;
			}
		}
		return SMultiColumnTableRow<FCandidatePtr>::OnMouseButtonDown(Geometry, PointerEvent);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (ColumnName == TEXT("Select"))
		{
			return SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked(BatchSelectionState)
					.IsEnabled(CanBatchSelect)
					.OnCheckStateChanged_Lambda([this](const ECheckBoxState NewState)
					{
						OnBatchSelectionChanged.ExecuteIfBound(Candidate, NewState);
					})
					.ToolTipText(LOCTEXT("BatchSelectRowTooltip", "Check this object for batch actions. Selecting the row opens its details."))
				];
		}
		if (ColumnName == TEXT("Actor"))
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Candidate->ActorLabel))
					.Font(SceneAutoTaggerVisual::MediumFont(11))
					.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Clipping(EWidgetClipping::ClipToBounds)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SBorder)
						.Visibility_Lambda([this]()
						{
							return ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
								Candidate->RefinementNote).IsEmpty()
								? EVisibility::Collapsed
								: EVisibility::Visible;
						})
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FLinearColor(0.02f, 0.42f, 0.39f, 0.55f))
						.Padding(FMargin(4.0f, 0.0f))
						.ToolTipText_Lambda([this]()
						{
							const FString Note =
								ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
									Candidate->RefinementNote);
							return FText::Format(
								LOCTEXT(
									"SavedRefinementNoteTooltip",
									"Saved refinement note: {0}\nAnalyze this row to use it."),
								FText::FromString(Note));
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("SavedRefinementNotePill", "Note"))
							.Font(SceneAutoTaggerVisual::MediumFont(8))
							.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							if (!Candidate->SuggestedName.IsEmpty())
							{
								return FText::FromString(Candidate->SuggestedName);
							}
							if (Candidate->bAnalysisCancelled)
							{
								return LOCTEXT("ExplorationStoppedBeforeAnalysis", "Exploration stopped before analysis");
							}
							return Candidate->Error.IsEmpty()
								? LOCTEXT("WaitingForSuggestion", "Waiting for suggestion")
								: LOCTEXT("SuggestionUnavailable", "Analysis unavailable");
						})
						.Font(SceneAutoTaggerVisual::RegularFont(9))
						.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				];
		}
		if (ColumnName == TEXT("Source"))
		{
			return SNew(STextBlock)
				.Text_Lambda([this]() { return FText::FromString(LexToString(Candidate->Source)); })
				.Font(SceneAutoTaggerVisual::RegularFont(9))
				.Justification(ETextJustify::Center);
		}
		if (ColumnName == TEXT("Confidence"))
		{
			return SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return Candidate->bTagComplete && !Candidate->bUserCapturePendingAnalysis
						? FText::AsPercent(Candidate->Confidence)
						: LOCTEXT("ConfidenceDash", "—");
				})
				.Font(SceneAutoTaggerVisual::MediumFont(10))
				.Justification(ETextJustify::Right);
		}
		if (ColumnName == TEXT("Decision"))
		{
			return SNew(STextBlock)
				.Text_Lambda([this]() { return DecisionText(Candidate->Decision); })
				.ColorAndOpacity_Lambda([this]() { return DecisionColor(Candidate->Decision); })
				.Font(SceneAutoTaggerVisual::MediumFont(9))
				.Justification(ETextJustify::Center);
		}

		return SNew(STextBlock)
			.Text_Lambda([this]() { return CandidateStatusText(*Candidate); })
			.ToolTipText_Lambda([this]()
			{
				if (!Candidate->Error.IsEmpty())
				{
					return FText::Format(
						LOCTEXT("AttentionStatusTooltip", "Needs attention: {0}\n{1}"),
						FText::FromString(Candidate->Error),
						HasValidEvidence(Candidate.Get())
							? LOCTEXT("AttentionAnalyzeRecovery", "Check this row, then choose More > Analyze checked.")
							: LOCTEXT("AttentionRecaptureRecovery", "Check this row, then choose More > Recapture & analyze checked."));
				}
				if (!ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
					Candidate->RefinementNote).IsEmpty())
				{
					return LOCTEXT(
						"RefinementNeedsAnalysisStatusTooltip",
						"A refinement note is saved for this object. Analyze the note before including or applying this suggestion.");
				}
				if (Candidate->bUserCapturePendingAnalysis)
				{
					return LOCTEXT(
						"NeedsAnalysisStatusTooltip",
						"This saved view has not been analyzed. Check the row, then choose More > Analyze checked.");
				}
				if (Candidate->bAnalysisCancelled)
				{
					return HasValidEvidence(Candidate.Get())
						? LOCTEXT("NotAnalyzedStatusTooltip", "Exploration stopped before analysis. Check this row, then choose More > Analyze checked.")
						: LOCTEXT("NotCapturedStatusTooltip", "Exploration stopped before capture. Check this row, then choose More > Recapture & analyze checked.");
				}
				return CandidateStatusText(*Candidate);
			})
			.Justification(ETextJustify::Center)
			.Font(SceneAutoTaggerVisual::RegularFont(9))
			.ColorAndOpacity_Lambda([this]()
			{
				if (!Candidate->Error.IsEmpty())
				{
					return FSlateColor(FLinearColor(0.93f, 0.32f, 0.28f));
				}
				if (Candidate->bUserCapturePendingAnalysis
					|| !ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
						Candidate->RefinementNote).IsEmpty())
				{
					return FSlateColor(FLinearColor(0.95f, 0.72f, 0.22f));
				}
				return Candidate->bAnalysisCancelled
					? FSlateColor(SceneAutoTaggerVisual::MutedText())
					: FSlateColor::UseForeground();
			})
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
	}

private:
	FCandidatePtr Candidate;
	FOnCandidateBatchSelectionChanged OnBatchSelectionChanged;
	FOnCandidateRowPressed OnRowPressed;
	TAttribute<ECheckBoxState> BatchSelectionState;
	TAttribute<bool> CanBatchSelect;
};
}

SConvaiSceneAutoTagger::SConvaiSceneAutoTagger() = default;

void SConvaiSceneAutoTagger::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	CharacterCatalog.Reset(NewObject<USceneAutoTaggerCharacterCatalog>());
	CharacterSetupModel = MakeUnique<FSceneAutoTaggerSetupModel>(12);
	RefreshAgentStatus();

	if (const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>())
	{
		SignificanceThreshold = Settings->SignificanceThreshold;
		bIncludeAlreadyTaggedActors = !Settings->bExcludeAlreadyTaggedActors;
	}
	if (const UConvaiSceneAutoTaggerUserState* UserState = GetDefault<UConvaiSceneAutoTaggerUserState>())
	{
		bPrivacyAcknowledged = UserState->HasAcceptedPrivacyConsent(PrivacyConsentKey);
	}
	SceneContextDraft = GetSceneContextForCurrentMap();
	DescriptionFocusDraft = GetDescriptionFocusForCurrentMap();
	SceneContextEditorMapKey = GetCurrentMapPackageName();
	SceneContextEditorWorld = GetEditorWorld();
	if (Controller.IsValid())
	{
		// The module keeps one controller alive while the tab is closed. Prune stale
		// actors (or clear a review from another map) before rebuilding its Slate view.
		Controller->ValidateRetainedReviewState(SceneContextEditorWorld.Get());
	}

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(SceneAutoTaggerVisual::Window())
			.Padding(0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildCompactSetupPanel()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f)
				[
					BuildErrorBanner()
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(0.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						BuildReviewPanel()
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						BuildEmptyStatePanel()
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f)
				[
					BuildStickyFooter()
				]
			]
		]
		+ SOverlay::Slot()
		[
			BuildCompletionSummaryOverlay()
		]
		+ SOverlay::Slot()
		[
			BuildBusyOverlay()
		]
		+ SOverlay::Slot()
		[
			BuildConsentOverlay()
		]
	];

	if (Controller.IsValid())
	{
		Controller->OnChanged().AddSP(this, &SConvaiSceneAutoTagger::HandleControllerChanged);
	}
	RefreshCharacterCatalog();
	RefreshCandidateList();
	bCompletionSummaryWasVisible =
		GetCompletionSummaryVisibility() == EVisibility::Visible;
	if (bCompletionSummaryWasVisible && CompletionContinueButton.IsValid()
		&& FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			CompletionContinueButton,
			EFocusCause::SetDirectly);
	}
}

SConvaiSceneAutoTagger::~SConvaiSceneAutoTagger()
{
	if (CharacterCatalog)
	{
		CharacterCatalog->Cancel();
	}
	++SceneContextDraftGeneration;
	++PreviewResourceGeneration;
	EndEditView();
	if (Controller.IsValid())
	{
		Controller->OnChanged().RemoveAll(this);
	}
	CommitPendingCandidateEdit();
	if (PreviewImageWidget.IsValid())
	{
		PreviewImageWidget->SetImage(nullptr);
	}
	ReleaseEvidenceBrush(PreviewBrush);
}

void SConvaiSceneAutoTagger::Tick(
	const FGeometry& AllottedGeometry,
	const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	const bool bWideSetupNow = AllottedGeometry.GetLocalSize().X >= SetupWideLayoutThreshold;
	const bool bSetupLayoutChanged = bSetupUsesWideLayout != bWideSetupNow;
	if (SetupScrollBox.IsValid() && bSetupLayoutChanged)
	{
		bSetupUsesWideLayout = bWideSetupNow;
		SetupScrollBox->SetScrollBarVisibility(
			bSetupUsesWideLayout ? EVisibility::Collapsed : EVisibility::Visible);
	}
	if (GuidanceScrollBox.IsValid())
	{
		GuidanceScrollBox->SetScrollBarVisibility(
			bWideSetupNow ? EVisibility::Visible : EVisibility::Collapsed);
		GuidanceScrollBox->SetScrollBarAlwaysVisible(bWideSetupNow);
		GuidanceScrollBox->SetConsumeMouseWheel(
			bWideSetupNow ? EConsumeMouseWheel::WhenScrollingPossible : EConsumeMouseWheel::Never);
		if (!bWideSetupNow)
		{
			if (bSetupLayoutChanged)
			{
				GuidanceScrollBox->EndInertialScrolling();
			}
			GuidanceScrollBox->SetScrollOffset(0.0f);
			GuidanceScrollBox->Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
		}
	}
	SyncSceneContextEditorToCurrentMap();
	if (bEditingView)
	{
		const bool bSessionMatchesSelection = SelectedCandidate.IsValid()
			&& SelectedCandidate->Actor.IsValid()
			&& EditSourceCandidate.Pin() == SelectedCandidate
			&& LiveCaptureSession
			&& LiveCaptureSession->IsValid()
			&& LiveCaptureSession->GetWorld() == GetEditorWorld();
		if (!bSessionMatchesSelection)
		{
			LocalError = TEXT("Edit View stopped because the actor or editor world changed.");
			EndEditView();
		}
		else
		{
			FString CaptureError;
			if (!LiveCaptureSession->RenderFrame(CaptureError))
			{
				LocalError = FString::Printf(
					TEXT("Edit View stopped because its live capture could not render: %s"),
					CaptureError.IsEmpty() ? TEXT("capture session unavailable") : *CaptureError);
				EndEditView();
			}
			else
			{
				if (!bEditViewReadabilityCalibrated
					&& EditViewLastObservedFrame != GFrameCounter)
				{
					EditViewLastObservedFrame = GFrameCounter;
					++EditViewWarmupFrames;
					const double ElapsedSeconds = FPlatformTime::Seconds()
						- EditViewWarmupStartedAtSeconds;
					if (!ConvaiSceneAutoTagger::ShouldContinueAutomaticCaptureWarmup(
						EditViewWarmupFrames,
						ElapsedSeconds,
						0))
					{
						if (!LiveCaptureSession->CalibrateReadability(CaptureError))
						{
							LocalError = FString::Printf(
								TEXT("Edit View stopped because readability calibration failed: %s"),
								CaptureError.IsEmpty() ? TEXT("capture session unavailable") : *CaptureError);
							EndEditView();
							return;
						}
						bEditViewReadabilityCalibrated = true;
					}
				}
				if (LivePreviewImageWidget.IsValid())
				{
					// The brush and layout stay fixed; only repaint the GPU render target.
					LivePreviewImageWidget->Invalidate(EInvalidateWidgetReason::Paint);
				}
			}
		}
	}
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildScopeTogglePair(
	const FName CurrentLevelTag,
	const FName SelectedActorsTag)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.HeightOverride(30.0f)
			[
				SNew(SCheckBox)
				.Style(&SceneAutoTaggerVisual::ScopeToggleStyle())
				.Padding(FMargin(10.0f, 0.0f))
				.HAlign(HAlign_Center)
				.IsChecked(this, &SConvaiSceneAutoTagger::GetScopeCheckState, EExplorationScope::CurrentLevel)
				.OnCheckStateChanged(this, &SConvaiSceneAutoTagger::SetScope, EExplorationScope::CurrentLevel)
				.IsEnabled_Lambda([this]() { return !Controller.IsValid() || !Controller->IsBusy(); })
				.ToolTipText(LOCTEXT("CurrentLevelScopeCompactTooltip", "Explore significant renderable actors in the current editor level."))
				.Tag(CurrentLevelTag)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("CurrentLevelScopeHeader", "Current Level"))
						.Font(SceneAutoTaggerVisual::RegularFont(10))
						.Justification(ETextJustify::Center)
					]
				]
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(1.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.HeightOverride(30.0f)
			[
				SNew(SCheckBox)
				.Style(&SceneAutoTaggerVisual::ScopeToggleStyle())
				.Padding(FMargin(10.0f, 0.0f))
				.HAlign(HAlign_Center)
				.IsChecked(this, &SConvaiSceneAutoTagger::GetScopeCheckState, EExplorationScope::SelectedActors)
				.OnCheckStateChanged(this, &SConvaiSceneAutoTagger::SetScope, EExplorationScope::SelectedActors)
				.IsEnabled_Lambda([this]() { return !Controller.IsValid() || !Controller->IsBusy(); })
				.ToolTipText(LOCTEXT("SelectedActorsScopeCompactTooltip", "Explore renderable actors in the current Level Editor selection, even when they fall below the level relevance threshold."))
				.Tag(SelectedActorsTag)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SelectedActorsScopeHeader", "Selected Actors"))
						.Font(SceneAutoTaggerVisual::RegularFont(10))
						.Justification(ETextJustify::Center)
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildAnalysisQualityTogglePair()
{
	const auto BuildOption = [this](
		const EConvaiSceneAutoTaggerAnalysisDetail Detail,
		const FText& Label,
		const FText& ToolTip,
		const FName WidgetTag)
	{
		return SNew(SBox)
			.HeightOverride(30.0f)
			[
				SNew(SCheckBox)
				.Style(&SceneAutoTaggerVisual::ScopeToggleStyle())
				.Padding(FMargin(12.0f, 0.0f))
				.HAlign(HAlign_Center)
				.IsChecked_Lambda([Detail]()
				{
					const EConvaiSceneAutoTaggerAnalysisDetail ConfiguredDetail =
						GetDefault<UConvaiSceneAutoTaggerSettings>()->AnalysisDetail;
					const EConvaiSceneAutoTaggerAnalysisDetail EffectiveDetail = ConfiguredDetail
						== EConvaiSceneAutoTaggerAnalysisDetail::Detailed
						? EConvaiSceneAutoTaggerAnalysisDetail::Detailed
						: EConvaiSceneAutoTaggerAnalysisDetail::Standard;
					return EffectiveDetail == Detail
						? ECheckBoxState::Checked
						: ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, Detail](const ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked)
					{
						UConvaiSceneAutoTaggerSettings* Settings =
							GetMutableDefault<UConvaiSceneAutoTaggerSettings>();
						Settings->AnalysisDetail = Detail;
						Settings->SaveConfig();
						RefreshAgentStatus();
					}
				})
				.IsEnabled_Lambda([this]() { return !Controller.IsValid() || !Controller->IsBusy(); })
				.ToolTipText(ToolTip)
				.Tag(WidgetTag)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Label)
						.Font(SceneAutoTaggerVisual::RegularFont(10))
						.Justification(ETextJustify::Center)
					]
				]
			];
	};

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			BuildOption(
				EConvaiSceneAutoTaggerAnalysisDetail::Standard,
				LOCTEXT("AnalysisQualityFast", "Fast"),
				LOCTEXT("AnalysisQualityFastTooltip", "Optimized for speed and general object descriptions."),
				TEXT("SceneAutoTagger.AnalysisQuality.Fast"))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(1.0f, 0.0f, 0.0f, 0.0f)
		[
			BuildOption(
				EConvaiSceneAutoTaggerAnalysisDetail::Detailed,
				LOCTEXT("AnalysisQualityAccurate", "Accurate"),
				LOCTEXT("AnalysisQualityAccurateTooltip", "Uses higher-detail images and deeper analysis for artwork, text, and specific subjects. Exact identification is not guaranteed."),
				TEXT("SceneAutoTagger.AnalysisQuality.Accurate"))
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCompactSetupPanel()
{
	const FLinearColor Surface = SceneAutoTaggerVisual::Header();
	const FLinearColor Border = SceneAutoTaggerVisual::Border();
	const FLinearColor PrimaryText = SceneAutoTaggerVisual::PrimaryText();
	const FLinearColor SecondaryText = SceneAutoTaggerVisual::SecondaryText();
	const FLinearColor Accent = SceneAutoTaggerVisual::Accent();

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SRoundedBox)
			.BackgroundColor(Surface)
			.BorderColor(Border)
			.BorderThickness(1.0f)
			.BorderRadius(0.0f)
			.ContentPadding(0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(14.0f, 9.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(SBox)
						.HeightOverride(30.0f)
						.Visibility(this, &SConvaiSceneAutoTagger::GetReviewVisibility)
						[
							SNew(SButton)
							.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
							.ContentPadding(FMargin(9.0f, 0.0f, 10.0f, 0.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleReturnToSetup)
							.IsEnabled(this, &SConvaiSceneAutoTagger::CanClearResults)
							.ToolTipText(LOCTEXT("BackToSetupTooltip", "Return to setup to change scope, guidance, or analysis quality. You will be asked before this review is discarded."))
							.Tag(TEXT("SceneAutoTagger.BackToSetup"))
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 6.0f, 0.0f)
								[
									SNew(SImage)
									.Image(FAppStyle::GetBrush("Icons.ChevronLeft"))
									.ColorAndOpacity(SecondaryText)
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("BackToSetup", "Back to setup"))
									.Font(SceneAutoTaggerVisual::RegularFont(10))
								]
							]
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("SceneExplorerTitle", "Scene Auto Tagger"))
								.Font(SceneAutoTaggerVisual::MediumFont(16))
								.ColorAndOpacity(PrimaryText)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(8.0f, 1.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text_Lambda([this]()
								{
									return Controller.IsValid() && !Controller->GetCandidates().IsEmpty()
										? LOCTEXT("ReviewWorkspaceSubtitle", "Review workspace")
										: LOCTEXT("ExplorationSetupSubtitle", "Exploration setup");
								})
								.Font(SceneAutoTaggerVisual::RegularFont(10))
								.ColorAndOpacity(SecondaryText)
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SBox)
						.HeightOverride(30.0f)
						.WidthOverride(160.0f)
						.Visibility(this, &SConvaiSceneAutoTagger::GetReviewVisibility)
						[
							SAssignNew(SceneContextComboButton, SComboButton)
							.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
							.HasDownArrow(false)
							.ContentPadding(FMargin(10.0f, 0.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.IsEnabled_Lambda([this]()
							{
								return !Controller.IsValid() || !Controller->IsBusy();
							})
							.OnMenuOpenChanged(this, &SConvaiSceneAutoTagger::HandleSceneContextMenuOpenChanged)
							.ToolTipText(LOCTEXT("SceneGuidanceTooltip", "Inspect the context and description-focus snapshots used for this review, or return to setup to change them."))
							.MenuContent()
							[
								BuildSceneContextMenu()
							]
							.ButtonContent()
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 6.0f, 0.0f)
								[
									SNew(SBox)
									.WidthOverride(6.0f)
									.HeightOverride(6.0f)
									.Visibility_Lambda([this]() { return HasGuidanceForCurrentReview() ? EVisibility::Visible : EVisibility::Collapsed; })
									[
										SNew(SRoundedBox)
										.BackgroundColor(Accent)
										.BorderRadius(3.0f)
									]
								]
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(this, &SConvaiSceneAutoTagger::GetSceneContextButtonText)
									.Font(SceneAutoTaggerVisual::RegularFont(10))
									.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(6.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(SImage)
									.Image(FAppStyle::GetBrush("Icons.ChevronDown"))
									.ColorAndOpacity(SecondaryText)
								]
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						.Visibility_Lambda([this]() { return IsAgentReady() ? EVisibility::Visible : EVisibility::Collapsed; })
						+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SBox)
								.WidthOverride(6.0f)
								.HeightOverride(6.0f)
								[
									SNew(SRoundedBox)
									.BackgroundColor(Accent)
									.BorderRadius(3.0f)
								]
							]
						+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(STextBlock)
								.Text(this, &SConvaiSceneAutoTagger::GetReadyAgentText)
								.Font(SceneAutoTaggerVisual::RegularFont(10))
								.ColorAndOpacity(SecondaryText)
								.ToolTipText_Lambda([this]()
								{
									return AgentAnalysisPolicyText.IsEmpty()
										? GetReadyAgentText()
										: FText::Format(
											LOCTEXT("AnalysisPolicyTooltip", "Analysis policy: {0}"),
											AgentAnalysisPolicyText);
								})
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SBox)
						.HeightOverride(30.0f)
						[
							SNew(SComboButton)
							.ContentPadding(FMargin(10.0f, 0.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.ToolTipText(LOCTEXT("SettingsTooltip", "Fine-tune discovery, bulk review, and applied scene objects."))
							.MenuContent()
							[
								BuildSettingsMenu()
							]
							.ButtonContent()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("Settings", "Settings"))
								.Font(SceneAutoTaggerVisual::RegularFont(10))
								.Justification(ETextJustify::Center)
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f)
				[
					SNew(SSeparator)
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.Visibility(this, &SConvaiSceneAutoTagger::GetReviewVisibility)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(SceneAutoTaggerVisual::Header())
			.Padding(FMargin(14.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ReviewSuggestionsLabel", "Review suggestions"))
					.Font(SceneAutoTaggerVisual::MediumFont(11))
					.ColorAndOpacity(PrimaryText)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ReviewSuggestionsHelp", "Select a row to inspect it. Check rows for batch actions."))
					.Font(SceneAutoTaggerVisual::RegularFont(10))
					.ColorAndOpacity(SecondaryText)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SButton)
					.Visibility(this, &SConvaiSceneAutoTagger::GetAttentionChipVisibility)
					.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(8.0f, 2.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleReopenCompletionSummary)
					.IsEnabled_Lambda([this]() { return Controller.IsValid() && !Controller->IsBusy(); })
					.ToolTipText(LOCTEXT("AttentionChipTooltip", "Open the analysis summary and inspect objects that need attention."))
					.Tag(TEXT("SceneAutoTagger.AttentionChip"))
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetAttentionChipText)
						.Font(SceneAutoTaggerVisual::MediumFont(9))
						.ColorAndOpacity(FLinearColor(0.95f, 0.42f, 0.35f))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SButton)
					.Visibility(this, &SConvaiSceneAutoTagger::GetLowInformationChipVisibility)
					.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(8.0f, 2.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleReopenCompletionSummary)
					.IsEnabled_Lambda([this]() { return Controller.IsValid() && !Controller->IsBusy(); })
					.ToolTipText(LOCTEXT("LowInformationChipTooltip", "Review objects that were skipped because their captures contained too little usable visual detail."))
					.Tag(TEXT("SceneAutoTagger.SkippedChip"))
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetLowInformationChipText)
						.Font(SceneAutoTaggerVisual::MediumFont(9))
						.ColorAndOpacity(FLinearColor(0.95f, 0.72f, 0.22f))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetReviewSummaryText)
					.Font(SceneAutoTaggerVisual::RegularFont(10))
					.ColorAndOpacity(SecondaryText)
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.Visibility(this, &SConvaiSceneAutoTagger::GetAgentRecoveryVisibility)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FLinearColor(0.42f, 0.25f, 0.08f, 1.0f))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetAgentStatusText)
					.ColorAndOpacity(this, &SConvaiSceneAutoTagger::GetAgentStatusColor)
					.AutoWrapText(true)
					.Tag(TEXT("SceneAutoTagger.AgentStatus"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(8.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("SetupAgentRecovery", "Open Convai Settings"))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleConfigureAgent)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(8.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("RecheckAgentRecovery", "Recheck"))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleRefreshAgentStatus)
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildSettingsMenu()
{
	return SNew(SBox)
		.WidthOverride(420.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ExploreSettingsTitle", "Settings"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ExploreSettingsSubtitle", "Fine-tune discovery, review automation, and applied scene objects."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ObjectsToAnalyzeTitle", "Discovery filtering"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ThresholdCompact", "Minimum local relevance"))
						.ToolTipText(LOCTEXT("ThresholdCompactTooltip", "Filters objects locally before capture. Higher values analyze fewer objects."))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SBox)
						.WidthOverride(76.0f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.MinSliderValue(0.0f)
							.MaxSliderValue(1.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return SignificanceThreshold; })
							.OnValueChanged_Lambda([this](float Value) { SignificanceThreshold = FMath::Clamp(Value, 0.0f, 1.0f); })
							.Tag(TEXT("SceneAutoTagger.SignificanceThreshold"))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ThresholdCompactHelp", "Applied to Current Level discovery. Explicitly selected actors bypass this relevance threshold; uncapturable or hard-excluded actors remain excluded."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DuplicateBehaviorTitle", "Duplicate objects"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 7.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([]()
					{
						return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveDuplicateGroupingMode()
							== EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([](const ECheckBoxState State)
					{
						if (State == ECheckBoxState::Checked)
						{
							UConvaiSceneAutoTaggerSettings* Settings =
								GetMutableDefault<UConvaiSceneAutoTaggerSettings>();
							Settings->DuplicateGroupingMode =
								EConvaiSceneAutoTaggerDuplicateGroupingMode::KeepSeparate;
							Settings->SaveConfig();
						}
					})
					.ToolTipText(LOCTEXT("KeepVerifiedDuplicatesSeparateTooltip", "Keep every verified copy independently addressable."))
					.Tag(TEXT("SceneAutoTagger.DuplicateGrouping.KeepSeparate"))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("KeepVerifiedDuplicatesSeparate", "Keep separate"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([]()
					{
						return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveDuplicateGroupingMode()
							== EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([](const ECheckBoxState State)
					{
						if (State == ECheckBoxState::Checked)
						{
							UConvaiSceneAutoTaggerSettings* Settings =
								GetMutableDefault<UConvaiSceneAutoTaggerSettings>();
							Settings->DuplicateGroupingMode =
								EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters;
							Settings->SaveConfig();
						}
					})
					.ToolTipText(LOCTEXT("GroupNearbyVerifiedDuplicatesTooltip", "Only proven copies within the configured horizontal and vertical span share one name."))
					.Tag(TEXT("SceneAutoTagger.DuplicateGrouping.Nearby"))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("GroupNearbyVerifiedDuplicates", "Auto (recommended)"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([]()
					{
						return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveDuplicateGroupingMode()
							== EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([](const ECheckBoxState State)
					{
						if (State == ECheckBoxState::Checked)
						{
							UConvaiSceneAutoTaggerSettings* Settings =
								GetMutableDefault<UConvaiSceneAutoTaggerSettings>();
							Settings->DuplicateGroupingMode =
								EConvaiSceneAutoTaggerDuplicateGroupingMode::MergeAllVerified;
							Settings->SaveConfig();
						}
					})
					.ToolTipText(LOCTEXT("GroupAllVerifiedDuplicatesTooltip", "Combine every object in one verified duplicate proof group, even when copies are far apart."))
					.Tag(TEXT("SceneAutoTagger.DuplicateGrouping.All"))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("GroupAllVerifiedDuplicates", "Group all verified"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(22.0f, 7.0f, 0.0f, 0.0f)
				[
					SNew(SVerticalBox)
					.Visibility_Lambda([]()
					{
						return GetDefault<UConvaiSceneAutoTaggerSettings>()->GetEffectiveDuplicateGroupingMode()
							== EConvaiSceneAutoTaggerDuplicateGroupingMode::NearbyClusters
							? EVisibility::Visible
							: EVisibility::Collapsed;
					})
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(LOCTEXT("DuplicateClusterHorizontalSpan", "Maximum horizontal span (cm)"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SBox)
							.WidthOverride(105.0f)
							[
								SNew(SSpinBox<float>)
								.MinValue(1.0f)
								.MaxValue(100000.0f)
								.MinSliderValue(100.0f)
								.MaxSliderValue(5000.0f)
								.Delta(50.0f)
								.Value_Lambda([]() { return GetDefault<UConvaiSceneAutoTaggerSettings>()->DuplicateClusterMaxHorizontalSpanCm; })
								.OnValueChanged_Lambda([](const float Value)
								{
									GetMutableDefault<UConvaiSceneAutoTaggerSettings>()->DuplicateClusterMaxHorizontalSpanCm = FMath::Max(1.0f, Value);
								})
								.OnValueCommitted_Lambda([](const float Value, ETextCommit::Type)
								{
									UConvaiSceneAutoTaggerSettings* Settings = GetMutableDefault<UConvaiSceneAutoTaggerSettings>();
									Settings->DuplicateClusterMaxHorizontalSpanCm = FMath::Max(1.0f, Value);
									Settings->SaveConfig();
								})
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 5.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(LOCTEXT("DuplicateClusterVerticalSpan", "Maximum vertical separation (cm)"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SBox)
							.WidthOverride(105.0f)
							[
								SNew(SSpinBox<float>)
								.MinValue(1.0f)
								.MaxValue(100000.0f)
								.MinSliderValue(100.0f)
								.MaxSliderValue(2000.0f)
								.Delta(50.0f)
								.Value_Lambda([]() { return GetDefault<UConvaiSceneAutoTaggerSettings>()->DuplicateClusterMaxVerticalSeparationCm; })
								.OnValueChanged_Lambda([](const float Value)
								{
									GetMutableDefault<UConvaiSceneAutoTaggerSettings>()->DuplicateClusterMaxVerticalSeparationCm = FMath::Max(1.0f, Value);
								})
								.OnValueCommitted_Lambda([](const float Value, ETextCommit::Type)
								{
									UConvaiSceneAutoTaggerSettings* Settings = GetMutableDefault<UConvaiSceneAutoTaggerSettings>();
									Settings->DuplicateClusterMaxVerticalSeparationCm = FMath::Max(1.0f, Value);
									Settings->SaveConfig();
								})
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 7.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"VerifiedDuplicateGroupingHelp",
						"Only geometry- or image-verified copies can group. Each group keeps one spoken name; navigation and proximity use its nearest concrete instance. After the first Apply, this review keeps its grouping plan."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SVerticalBox)
					.Visibility(this, &SConvaiSceneAutoTagger::GetReviewVisibility)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 12.0f, 0.0f, 8.0f)
					[
						SNew(SSeparator)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("BulkReviewTitle", "Auto-include suggestions"))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("BulkConfidenceCompact", "Minimum AI confidence"))
							.ToolTipText(LOCTEXT("BulkConfidenceWarning", "AI confidence is uncalibrated. Inspect included items before applying them to the scene."))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(72.0f)
							[
								SNew(SSpinBox<float>)
								.MinValue(0.0f)
								.MaxValue(1.0f)
								.MinSliderValue(0.0f)
								.MaxSliderValue(1.0f)
								.Delta(0.01f)
								.Value_Lambda([this]() { return BulkConfidenceThreshold; })
								.OnValueChanged_Lambda([this](float Value) { BulkConfidenceThreshold = FMath::Clamp(Value, 0.0f, 1.0f); })
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.ContentPadding(FMargin(9.0f, 3.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.Text(LOCTEXT("BulkAcceptCompact", "Include matching"))
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleAcceptAtOrAbove)
							.IsEnabled(this, &SConvaiSceneAutoTagger::CanRunBulkActions)
							.ToolTipText(LOCTEXT("BulkAcceptCompactTooltip", "Include every completed suggestion at or above this uncalibrated AI-confidence value."))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 5.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
					.Text(LOCTEXT("BulkConfidenceHelp", "Runs only after analysis; it does not choose which objects are analyzed. Review AI confidence before applying."))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.ContentPadding(FMargin(9.0f, 3.0f))
							.Text(LOCTEXT("ResetDecisionsSettings", "Reset decisions"))
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleResetAll)
							.IsEnabled(this, &SConvaiSceneAutoTagger::CanRunBulkActions)
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 8.0f)
				[
					SNew(SSeparator)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AppliedObjectsTitle", "Applied scene objects"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"AppliedObjectsHelp",
						"Inspect saved Convai objects, generate movement points, or remove managed components."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 7.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(10.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("ManageAppliedObjects", "Open Scene Objects..."))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleManageAppliedObjects)
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildSceneContextMenu()
{
	return SNew(SBox)
		.WidthOverride(440.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SceneGuidanceUsedTitle", "Guidance used"))
					.Font(SceneAutoTaggerVisual::MediumFont(12))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return !HasGuidanceForCurrentReview()
							? LOCTEXT("ImagesOnlyGuidanceSummary", "No optional guidance. This review used object images only.")
							: LOCTEXT("GuidanceSnapshotSummary", "These snapshots guided every suggestion in the current review. Images remained authoritative.");
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([this]()
					{
						return HasSceneContextForCurrentReview() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SceneContextSnapshotLabel", "Scene context"))
						.Font(SceneAutoTaggerVisual::MediumFont(10))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MinDesiredHeight(60.0f)
					.Visibility_Lambda([this]()
					{
						return HasSceneContextForCurrentReview() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(SMultiLineEditableTextBox)
						.Text(this, &SConvaiSceneAutoTagger::GetSceneContextDraftText)
						.IsReadOnly(true)
						.AutoWrapText(true)
						.Tag(TEXT("SceneAutoTagger.SceneContext.ReviewSnapshot"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this]()
					{
						return HasDescriptionFocusForCurrentReview() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("DescriptionFocusSnapshotLabel", "Description focus"))
						.Font(SceneAutoTaggerVisual::MediumFont(10))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MinDesiredHeight(60.0f)
					.Visibility_Lambda([this]()
					{
						return HasDescriptionFocusForCurrentReview() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(SMultiLineEditableTextBox)
						.Text(this, &SConvaiSceneAutoTagger::GetDescriptionFocusDraftText)
						.IsReadOnly(true)
						.AutoWrapText(true)
						.Tag(TEXT("SceneAutoTagger.DescriptionFocus.ReviewSnapshot"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("GuidanceLockedToReview", "Locked to this review so every retry and checked-row analysis uses the same guidance."))
						.Font(SceneAutoTaggerVisual::RegularFont(9))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					.Padding(0.0f, 9.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(10.0f, 4.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleReturnToSetup)
						.IsEnabled(this, &SConvaiSceneAutoTagger::CanClearResults)
						.ToolTipText(LOCTEXT("EditGuidanceInSetupTooltip", "Return to setup after confirmation, then change context, description focus, scope, or analysis quality."))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("EditGuidanceInSetup", "Edit in setup"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildBusyOverlay()
{
	return SNew(SBorder)
		.Visibility(this, &SConvaiSceneAutoTagger::GetBusyOverlayVisibility)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.74f))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(440.0f)
			[
				SNew(SRoundedBox)
				.BackgroundColor(FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.panel")))
				.BorderRadius(10.0f)
				.ContentPadding(18.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetPipelineStateText)
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.Justification(ETextJustify::Center)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					.HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetProgressMessageText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(false)
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.Justification(ETextJustify::Center)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					.HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetProgressCountText)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 12.0f, 0.0f, 0.0f)
					.HAlign(HAlign_Center)
					[
						SNew(SButton)
						.ContentPadding(FMargin(12.0f, 4.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.Text(LOCTEXT("CancelBusy", "Cancel"))
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleCancel)
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCompletionSummaryOverlay()
{
	const FLinearColor PrimaryText = SceneAutoTaggerVisual::PrimaryText();
	const FLinearColor SecondaryText = SceneAutoTaggerVisual::SecondaryText();
	const FLinearColor Amber = FLinearColor(0.95f, 0.72f, 0.22f);

	return SNew(SBorder)
		.Visibility(this, &SConvaiSceneAutoTagger::GetCompletionSummaryVisibility)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.74f))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(18.0f))
		.Tag(TEXT("SceneAutoTagger.CompletionSummary"))
		[
			SNew(SBox)
			.MaxDesiredWidth(640.0f)
			[
				SNew(SRoundedBox)
				.BackgroundColor(SceneAutoTaggerVisual::Panel())
				.BorderColor(SceneAutoTaggerVisual::Border())
				.BorderThickness(1.0f)
				.BorderRadius(10.0f)
				.ContentPadding(FMargin(22.0f, 20.0f))
				[
					SNew(SBox)
					.MaxDesiredHeight(680.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CompletionSummaryTitle", "Analysis complete"))
							.Font(SceneAutoTaggerVisual::MediumFont(16))
							.ColorAndOpacity(PrimaryText)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(10.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SRoundedBox)
							.BackgroundColor(FLinearColor(0.32f, 0.23f, 0.05f, 1.0f))
							.BorderColor(FLinearColor(0.58f, 0.42f, 0.10f, 1.0f))
							.BorderThickness(1.0f)
							.BorderRadius(9.0f)
							.ContentPadding(FMargin(8.0f, 2.0f))
							[
								SNew(STextBlock)
								.Text(this, &SConvaiSceneAutoTagger::GetCompletionSummaryCountText)
								.Font(SceneAutoTaggerVisual::MediumFont(9))
								.ColorAndOpacity(Amber)
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetCompletionSummaryBodyText)
						.Font(SceneAutoTaggerVisual::RegularFont(10))
						.ColorAndOpacity(SecondaryText)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 14.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						.Visibility(this, &SConvaiSceneAutoTagger::GetAttentionSummarySectionVisibility)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CompletionAttentionHeading", "Needs attention"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
							.ColorAndOpacity(FLinearColor(0.95f, 0.42f, 0.35f))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 3.0f, 0.0f, 7.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CompletionAttentionHelp", "These items remain in review. Inspect one to retry its saved image or recapture it."))
							.Font(SceneAutoTaggerVisual::RegularFont(9))
							.ColorAndOpacity(SecondaryText)
							.AutoWrapText(true)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SBox)
							.HeightOverride_Lambda([this]()
							{
								return FOptionalSize(FMath::Clamp(
									static_cast<float>(AttentionCandidates.Num()) * 52.0f,
									64.0f,
									180.0f));
							})
							[
								SAssignNew(AttentionListView, SListView<FCandidatePtr>)
								.ListItemsSource(&AttentionCandidates)
								.SelectionMode(ESelectionMode::None)
								.OnGenerateRow(this, &SConvaiSceneAutoTagger::GenerateAttentionCandidateRow)
								.ToolTipText(LOCTEXT("AttentionSummaryListTooltip", "Objects that did not produce a reliable suggestion and remain available in review."))
								.Tag(TEXT("SceneAutoTagger.AttentionSummaryList"))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 14.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						.Visibility(this, &SConvaiSceneAutoTagger::GetLowInformationSummarySectionVisibility)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("LowInformationSelectionHelp", "Skipped before analysis"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
							.ColorAndOpacity(Amber)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 3.0f, 0.0f, 7.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("LowInformationSelectionCaption", "Check any low-detail capture you still want analyzed."))
							.Font(SceneAutoTaggerVisual::RegularFont(9))
							.ColorAndOpacity(SecondaryText)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SBox)
							.HeightOverride_Lambda([this]()
							{
								return FOptionalSize(FMath::Clamp(
									static_cast<float>(LowInformationCandidates.Num()) * 52.0f,
									64.0f,
									180.0f));
							})
							[
								SAssignNew(LowInformationListView, SListView<FCandidatePtr>)
								.ListItemsSource(&LowInformationCandidates)
								.SelectionMode(ESelectionMode::None)
								.OnGenerateRow(this, &SConvaiSceneAutoTagger::GenerateLowInformationCandidateRow)
								.ToolTipText(LOCTEXT("LowInformationListTooltip", "Objects skipped from review because their saved captures had too little usable visual information."))
								.Tag(TEXT("SceneAutoTagger.LowInformationList"))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					.Padding(0.0f, 16.0f, 0.0f, 0.0f)
					[
						SNew(SWrapBox)
						.UseAllottedSize(true)
						.InnerSlotPadding(FVector2D(8.0f, 5.0f))
						+ SWrapBox::Slot()
						[
							SNew(SButton)
							.Visibility(this, &SConvaiSceneAutoTagger::GetLowInformationSummarySectionVisibility)
							.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
							.ContentPadding(FMargin(12.0f, 4.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleAnalyzeLowInformationSelection)
							.IsEnabled(this, &SConvaiSceneAutoTagger::CanAnalyzeLowInformationSelection)
							.ToolTipText(LOCTEXT("AnalyzeLowInformationTooltip", "Restore the checked objects to review and analyze their retained images."))
							.Tag(TEXT("SceneAutoTagger.AnalyzeSkipped"))
							[
								SNew(STextBlock)
								.Text(this, &SConvaiSceneAutoTagger::GetAnalyzeLowInformationButtonText)
								.Font(SceneAutoTaggerVisual::MediumFont(10))
							]
						]
						+ SWrapBox::Slot()
						[
							SAssignNew(CompletionContinueButton, SButton)
							.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
							.ContentPadding(FMargin(14.0f, 4.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleContinueFromCompletionSummary)
							.IsEnabled_Lambda([this]() { return Controller.IsValid() && !Controller->IsBusy(); })
							.Tag(TEXT("SceneAutoTagger.ContinueFromSummary"))
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ContinueFromLowInformation", "Continue to review"))
								.Font(SceneAutoTaggerVisual::MediumFont(10))
							]
						]
					]
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildConsentOverlay()
{
	return SNew(SBorder)
		.Visibility(this, &SConvaiSceneAutoTagger::GetConsentOverlayVisibility)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.window")))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(560.0f)
			[
				SNew(SRoundedBox)
				.BackgroundColor(FConvaiStyle::RequireColor(TEXT("Convai.Color.surface.panel")))
				.BorderRadius(10.0f)
				.ContentPadding(22.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ConsentTitle", "Allow AI scene analysis?"))
						.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetPrivacyDisclosureText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 16.0f, 0.0f, 0.0f)
					.HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.ContentPadding(FMargin(12.0f, 4.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.Text(LOCTEXT("DeclineConsent", "Decline"))
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleDeclinePrivacy)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
							.ContentPadding(FMargin(14.0f, 4.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.Text(LOCTEXT("AcceptConsent", "Accept"))
							.OnClicked(this, &SConvaiSceneAutoTagger::HandleAcceptPrivacy)
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildReviewPanel()
{
	// Keep the object list and analysis image visible together in a narrow dock.
	// On wide layouts, both inspector columns fill the review height and surplus width
	// favors the evidence image instead of over-stretching editable text.
	const auto GetInspectorColumnHeight = [this]() -> FOptionalSize
	{
		if (!ReviewInspectorViewport.IsValid())
		{
			return FOptionalSize();
		}

		const FVector2D ViewportSize = ReviewInspectorViewport->GetCachedGeometry().GetLocalSize();
		if (ViewportSize.X < 720.0f || ViewportSize.Y <= 0.0f)
		{
			return FOptionalSize();
		}

		return FOptionalSize(FMath::Max(500.0f, ViewportSize.Y));
	};

	return SNew(SSplitter)
		.Visibility(this, &SConvaiSceneAutoTagger::GetReviewVisibility)
		.PhysicalSplitterHandleSize(1.0f)
		+ SSplitter::Slot()
		.Value(0.27f)
		.MinSize(330.0f)
		[
			BuildCandidateListPanel()
		]
		+ SSplitter::Slot()
		.Value(0.73f)
		.MinSize(340.0f)
		[
			SAssignNew(ReviewInspectorViewport, SBox)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					.HAlign(HAlign_Fill)
					.InnerSlotPadding(FVector2D(1.0f, 1.0f))
					+ SWrapBox::Slot()
					.FillLineWhenSizeLessThan(720.0f)
					[
						SNew(SBox)
						.MinDesiredWidth(380.0f)
						.WidthOverride(570.0f)
						.MinDesiredHeight(500.0f)
						.HeightOverride_Lambda(GetInspectorColumnHeight)
						[
							BuildEvidencePanel()
						]
					]
					+ SWrapBox::Slot()
					.FillLineWhenSizeLessThan(720.0f)
					[
						SNew(SBox)
						.MinDesiredWidth(300.0f)
						.WidthOverride(460.0f)
						.MinDesiredHeight(500.0f)
						.HeightOverride_Lambda(GetInspectorColumnHeight)
						[
							BuildCandidateDetailsPanel()
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildEmptyStatePanel()
{
	const auto GetSetupPanelHeight = [this]()
	{
		const FVector2D RootSize = GetCachedGeometry().GetLocalSize();
		const bool bWide = RootSize.X >= SetupWideLayoutThreshold;
		return FOptionalSize(FMath::Clamp(
			RootSize.Y - (bWide ? 220.0f : 180.0f),
			bWide ? 280.0f : 420.0f,
			820.0f));
	};
	const auto HasCredentials = []()
	{
		const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
		return FSceneAutoTaggerSetupModel::HasUsableCredentials(
			AuthHeaderAndKey.Key,
			AuthHeaderAndKey.Value);
	};
	return SNew(SBox)
		.Visibility(this, &SConvaiSceneAutoTagger::GetEmptyStateVisibility)
		.HAlign(HAlign_Fill)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
			SNew(SBox)
			.Visibility_Lambda([HasCredentials]()
			{
				return HasCredentials() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.HAlign(HAlign_Fill)
			[
				SAssignNew(SetupScrollBox, SScrollBox)
				+ SScrollBox::Slot()
				.Padding(FMargin(28.0f, 22.0f, 28.0f, 28.0f))
				[
					SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PrepareSceneAnalysisTitle", "Prepare scene analysis"))
					.Font(SceneAutoTaggerVisual::MediumFont(22))
					.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 18.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PrepareSceneAnalysisBody", "Choose who will analyze the scene, then add any optional guidance."))
					.Font(SceneAutoTaggerVisual::RegularFont(11))
					.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(14.0f, 14.0f))
					+ SWrapBox::Slot()
					.FillLineWhenSizeLessThan(SetupWideLayoutThreshold)
					[
						SNew(SBox)
						.MinDesiredWidth(420.0f)
						.HeightOverride_Lambda(GetSetupPanelHeight)
						.WidthOverride_Lambda([this]()
						{
							return FOptionalSize(CalculateSetupPanelWidth(
								GetCachedGeometry().GetLocalSize().X,
								420.0f,
								CharacterPanelWideFraction));
						})
						[
							BuildCharacterSelectionPanel()
						]
					]
					+ SWrapBox::Slot()
					.FillLineWhenSizeLessThan(SetupWideLayoutThreshold)
					.FillEmptySpace(true)
					[
						SNew(SBox)
						.MinDesiredWidth(460.0f)
						.HeightOverride_Lambda([this, GetSetupPanelHeight]()
						{
							return GetCachedGeometry().GetLocalSize().X >= SetupWideLayoutThreshold
								? GetSetupPanelHeight()
								: FOptionalSize();
						})
						.WidthOverride_Lambda([this]()
						{
							return FOptionalSize(CalculateSetupPanelWidth(
								GetCachedGeometry().GetLocalSize().X,
								460.0f,
								GuidancePanelWideFraction));
						})
						[
							BuildGuidancePanel()
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(14.0f, 10.0f, 14.0f, 0.0f)
				[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNullWidget::NullWidget
						]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
						.ContentPadding(FMargin(20.0f, 7.0f))
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleExplore)
						.IsEnabled(this, &SConvaiSceneAutoTagger::CanExplore)
						.ToolTipText(this, &SConvaiSceneAutoTagger::GetExploreToolTip)
						.Text(this, &SConvaiSceneAutoTagger::GetExploreButtonText)
						.Tag(TEXT("SceneAutoTagger.Explore.EmptyState"))
					]
				]
				]
			]
			]
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility_Lambda([HasCredentials]()
				{
					return HasCredentials() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(32.0f)
				[
					SNew(SBox)
					.MaxDesiredWidth(560.0f)
					[
						SNew(SRoundedBox)
						.BackgroundColor(SceneAutoTaggerVisual::Panel())
						.BorderColor(SceneAutoTaggerVisual::Border())
						.BorderThickness(1.0f)
						.BorderRadius(10.0f)
						.ContentPadding(FMargin(28.0f, 26.0f))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ConnectConvaiTitle", "Connect Convai to continue"))
								.Font(SceneAutoTaggerVisual::MediumFont(18))
								.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
							]
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 8.0f, 0.0f, 20.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT(
									"ConnectConvaiBody",
									"Sign in to Convai or add an API key before using Scene Auto Tagger."))
								.Font(SceneAutoTaggerVisual::RegularFont(11))
								.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
								.Justification(ETextJustify::Center)
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth()
								[
									SNew(SButton)
									.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
									.ContentPadding(FMargin(18.0f, 7.0f))
									.Text(LOCTEXT("ConnectConvaiSettings", "Open Convai Settings"))
									.OnClicked(this, &SConvaiSceneAutoTagger::HandleConfigureAgent)
								]
								+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(SButton)
									.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
									.ContentPadding(FMargin(16.0f, 7.0f))
									.Text(LOCTEXT("ConnectConvaiRecheck", "Recheck"))
									.OnClicked(this, &SConvaiSceneAutoTagger::HandleRefreshAgentStatus)
								]
							]
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCharacterSelectionPanel()
{
	return SNew(SRoundedBox)
		.BackgroundColor(SceneAutoTaggerVisual::Panel())
		.BorderColor(SceneAutoTaggerVisual::Border())
		.BorderThickness(1.0f)
		.BorderRadius(9.0f)
		.ContentPadding(FMargin(18.0f, 16.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ChooseCharacterTitle", "1  Character"))
				.Font(SceneAutoTaggerVisual::MediumFont(15))
				.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SBox)
					.HeightOverride(34.0f)
					[
						SNew(SSearchBox)
						.HintText(LOCTEXT("CharacterSearchHint", "Search your characters"))
						.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleCharacterSearchChanged)
						.Tag(TEXT("SceneAutoTagger.CharacterSearch"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetCharacterCountText)
					.Font(SceneAutoTaggerVisual::RegularFont(9))
					.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 7.0f)
			[
				SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetCharacterCatalogFeedbackText)
					.Font(SceneAutoTaggerVisual::RegularFont(9))
					.Visibility_Lambda([this]()
					{
						return GetCharacterCatalogFeedbackText().IsEmpty()
							? EVisibility::Collapsed : EVisibility::Visible;
					})
					.ColorAndOpacity_Lambda([this]()
					{
						return CharacterCatalogError.IsEmpty()
							? FSlateColor(SceneAutoTaggerVisual::SecondaryText())
							: FSlateColor(FLinearColor(0.93f, 0.32f, 0.28f));
					})
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(CharacterScrollBox, SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(CharacterGridContainer, SBox)
					.MinDesiredHeight(250.0f)
					[
						BuildCharacterGrid()
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SRoundedBox)
				.Visibility_Lambda([this]()
				{
					return CharacterSetupModel && CharacterSetupModel->GetSelectedCharacter()
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.BackgroundColor(SceneAutoTaggerVisual::Toolbar())
				.BorderColor(SceneAutoTaggerVisual::Border())
				.BorderThickness(1.0f)
				.BorderRadius(6.0f)
				.ContentPadding(FMargin(11.0f, 9.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text(this, &SConvaiSceneAutoTagger::GetSelectedCharacterNameText)
							.Font(SceneAutoTaggerVisual::MediumFont(11))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(this, &SConvaiSceneAutoTagger::GetSelectedCharacterDescriptionText)
							.Font(SceneAutoTaggerVisual::RegularFont(9))
							.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
							.AutoWrapText(true)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(0.0f)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleOpenCharacterInConvai)
						.IsEnabled_Lambda([this]()
						{
							return CharacterSetupModel && CharacterSetupModel->GetSelectedCharacter();
						})
						[
							SNew(SRoundedBox)
							.BackgroundColor(SceneAutoTaggerVisual::RowHover())
							.BorderColor(SceneAutoTaggerVisual::Accent())
							.BorderThickness(1.0f)
							.BorderRadius(8.0f)
							.ContentPadding(FMargin(16.0f, 8.0f))
							[
								SNew(STextBlock)
								.Text(LOCTEXT("OpenCharacterInConvai", "Open in Convai"))
								.Font(SceneAutoTaggerVisual::MediumFont(10))
								.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
							]
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCharacterGrid()
{
	TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(3.0f));
	if (bCharacterCatalogLoading)
	{
		Grid->AddSlot(0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SThrobber)]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("LoadingCharacters", "Loading your Convai characters..."))
			]
		];
		return Grid;
	}
	if (!CharacterSetupModel || CharacterSetupModel->GetVisibleCharacters().IsEmpty())
	{
		const bool bHasSearch = CharacterSetupModel && !CharacterSetupModel->GetSearchText().IsEmpty();
		const bool bAccountIsEmpty = CharacterSetupModel
			&& CharacterSetupModel->GetAllCharacters().IsEmpty()
			&& CharacterCatalogError.IsEmpty();
		Grid->AddSlot(0, 0)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(!CharacterCatalogError.IsEmpty()
					? FText::FromString(CharacterCatalogError)
					: bHasSearch
						? LOCTEXT("NoMatchingCharacters", "No characters match this search.")
						: LOCTEXT("NoAccountCharacters", "No Convai characters were found for this account."))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 7.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
				.Text(bHasSearch
					? LOCTEXT("ClearCharacterSearch", "Clear search")
					: bAccountIsEmpty
						? LOCTEXT("OpenConvaiDashboard", "Open Convai")
						: LOCTEXT("RetryCharacterCatalog", "Retry"))
				.OnClicked_Lambda([this, bHasSearch, bAccountIsEmpty]()
				{
					if (bHasSearch && CharacterSetupModel)
					{
						CharacterSetupModel->SetSearchText(FString());
						RefreshCharacterGrid();
					}
					else if (bAccountIsEmpty)
					{
						FPlatformProcess::LaunchURL(TEXT("https://convai.com/dashboard/character"), nullptr, nullptr);
					}
					else
					{
						RefreshCharacterCatalog();
					}
					return FReply::Handled();
				})
			]
		];
		return Grid;
	}
	// Derive card sizing from the stable setup width. The picker intentionally keeps
	// three columns so maximizing or docking the window never changes its structure.
	const FCharacterGridLayout GridLayout = CalculateCharacterGridLayout(GetCachedGeometry().GetLocalSize().X);
	int32 CharacterIndex = 0;
	for (const FSceneAutoTaggerCharacterOption& Character : CharacterSetupModel->GetVisibleCharacters())
	{
		Grid->AddSlot(CharacterIndex % GridLayout.ColumnCount, CharacterIndex / GridLayout.ColumnCount)[BuildCharacterCard(Character)];
		++CharacterIndex;
	}
	// Keep partial rows at the same responsive card width. Without placeholders,
	// a single search result expands across the entire character panel.
	for (int32 PlaceholderIndex = CharacterIndex; PlaceholderIndex % GridLayout.ColumnCount != 0; ++PlaceholderIndex)
	{
		Grid->AddSlot(PlaceholderIndex % GridLayout.ColumnCount, PlaceholderIndex / GridLayout.ColumnCount)
		[
			SNew(SBox)
			.MinDesiredWidth(CharacterCardMinWidth)
			.Visibility(EVisibility::Hidden)
		];
	}
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			Grid
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 14.0f, 0.0f, 8.0f)
		[
			SNew(SButton)
			.Visibility_Lambda([this]()
			{
				return CharacterSetupModel && CharacterSetupModel->CanLoadMore()
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(0.0f)
			.OnClicked(this, &SConvaiSceneAutoTagger::HandleLoadMoreCharacters)
			.Tag(TEXT("SceneAutoTagger.LoadMoreCharacters"))
			[
				SNew(SRoundedBox)
				.BackgroundColor(SceneAutoTaggerVisual::RowHover())
				.BorderColor(SceneAutoTaggerVisual::Border())
				.BorderThickness(1.0f)
				.BorderRadius(9.0f)
				.ContentPadding(FMargin(22.0f, 9.0f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LoadMoreCharacters", "Load more characters"))
					.Font(SceneAutoTaggerVisual::MediumFont(10))
					.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCharacterCard(const FSceneAutoTaggerCharacterOption& Character)
{
	const bool bSelected = CharacterSetupModel && CharacterSetupModel->GetSelectedCharacterID() == Character.ID;
	const FString Initial = Character.Name.IsEmpty() ? TEXT("?") : Character.Name.Left(1).ToUpper();
	const FString AvatarUrl = Character.AvatarUrl;
	const auto GetCardHeight = [this]()
	{
		const FCharacterGridLayout GridLayout = CalculateCharacterGridLayout(GetCachedGeometry().GetLocalSize().X);
		const float CardWidth = FMath::Max(
			CharacterCardMinWidth,
			(GridLayout.Width - GridLayout.ColumnCount * 6.0f) / GridLayout.ColumnCount);
		return FOptionalSize(FMath::Clamp(CardWidth * 0.90f, 190.0f, 320.0f));
	};
	return SNew(SBox)
		.MinDesiredWidth(CharacterCardMinWidth)
		.HeightOverride_Lambda(GetCardHeight)
		.Clipping(EWidgetClipping::ClipToBounds)
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ContentPadding(0.0f)
		.OnClicked_Lambda([this, ID = Character.ID]()
		{
			HandleCharacterSelected(ID);
			return FReply::Handled();
		})
		.Tag(TEXT("SceneAutoTagger.CharacterCard"))
		[
			SNew(SRoundedBox)
			.BackgroundColor(SceneAutoTaggerVisual::Row())
			.BorderColor(bSelected ? SceneAutoTaggerVisual::Accent() : SceneAutoTaggerVisual::Border())
			.BorderThickness(bSelected ? 2.0f : 1.0f)
			.BorderRadius(9.0f)
			.ContentPadding(FMargin(1.0f))
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SSimpleGradient)
					.StartColor(FLinearColor(0.48f, 0.54f, 0.57f))
					.EndColor(FLinearColor(0.32f, 0.38f, 0.42f))
					.Orientation(Orient_Vertical)
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SThrobber)
					.NumPieces(3)
					.Visibility_Lambda([this, AvatarUrl]()
					{
						GetCharacterAvatarBrush(AvatarUrl);
						return !AvatarUrl.IsEmpty() && PendingCharacterAvatarDownloads.Contains(AvatarUrl)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Initial))
					.Font(SceneAutoTaggerVisual::CharacterNameFont(24))
					.Visibility_Lambda([this, AvatarUrl]()
					{
						GetCharacterAvatarBrush(AvatarUrl);
						return !CharacterAvatarCache.Contains(AvatarUrl)
							&& !PendingCharacterAvatarDownloads.Contains(AvatarUrl)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
				]
				+ SOverlay::Slot()
				[
					SNew(SScaleBox)
					// Three-column cards are close enough to square to frame the avatar at
					// full width without the severe clipping caused by the old wide cards.
					// Anchor the image to the lower edge so bodies meet the thumbnail base.
					.Stretch(EStretch::ScaleToFitX)
					.StretchDirection(EStretchDirection::Both)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Bottom)
					.Visibility_Lambda([this, AvatarUrl]()
					{
						GetCharacterAvatarBrush(AvatarUrl);
						return CharacterAvatarCache.Contains(AvatarUrl) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						SNew(SImage)
						.Image_Lambda([this, AvatarUrl]() { return GetCharacterAvatarBrush(AvatarUrl); })
					]
				]
				+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Bottom)
				[
					SNew(SBox)
					.HeightOverride(CharacterCardCaptionHeight)
					[
						SNew(SCharacterNameShade)
					]
				]
				+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Bottom).Padding(13.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Character.Name))
					.ToolTipText(FText::FromString(Character.Name))
					.AccessibleText(FText::FromString(Character.Name))
					.Font(SceneAutoTaggerVisual::CharacterNameFont(12))
					.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
					.AutoWrapText(false)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(10.0f)
				[
					SNew(SRoundedBox)
					.Visibility(bSelected ? EVisibility::Visible : EVisibility::Collapsed)
					.BackgroundColor(SceneAutoTaggerVisual::PrimaryAction())
					.BorderRadius(6.0f)
					.ContentPadding(FMargin(10.0f, 5.0f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SelectedCharacterBadge", "Selected"))
						.Font(SceneAutoTaggerVisual::MediumFont(9))
						.ColorAndOpacity(FLinearColor::White)
					]
				]
			]
		]
	];
}

const FSlateBrush* SConvaiSceneAutoTagger::GetCharacterAvatarBrush(const FString& AvatarUrl) const
{
	if (AvatarUrl.IsEmpty())
	{
		return nullptr;
	}
	if (const FCharacterAvatarCacheEntry* Cached = CharacterAvatarCache.Find(AvatarUrl))
	{
		return Cached->Brush.Get();
	}
	if (PendingCharacterAvatarDownloads.Contains(AvatarUrl) || FailedCharacterAvatarDownloads.Contains(AvatarUrl))
	{
		return nullptr;
	}

	PendingCharacterAvatarDownloads.Add(AvatarUrl);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(AvatarUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("UnrealEngine/ConvaiSceneAutoTagger"));
	TWeakPtr<SConvaiSceneAutoTagger> WeakSelf = SharedThis(const_cast<SConvaiSceneAutoTagger*>(this));
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, AvatarUrl](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->PendingCharacterAvatarDownloads.Remove(AvatarUrl);
			if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				Self->FailedCharacterAvatarDownloads.Add(AvatarUrl);
				UE_LOG(LogConvaiSceneAutoTaggerUi, Warning, TEXT("Character avatar download failed (HTTP %d)."), Response.IsValid() ? Response->GetResponseCode() : 0);
				return;
			}
			const TArray<uint8>& Compressed = Response->GetContent();
			if (Compressed.IsEmpty() || Compressed.Num() > 8 * 1024 * 1024)
			{
				Self->FailedCharacterAvatarDownloads.Add(AvatarUrl);
				return;
			}
			IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			int32 Width = 0;
			int32 Height = 0;
			TArray<uint8> Pixels;
			bool bDecoded = false;
			const EImageFormat Format = ImageModule.DetectImageFormat(Compressed.GetData(), Compressed.Num());
			if (Format != EImageFormat::Invalid)
			{
				TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(Format);
				if (Wrapper.IsValid() && Wrapper->SetCompressed(Compressed.GetData(), Compressed.Num()))
				{
					Width = static_cast<int32>(Wrapper->GetWidth());
					Height = static_cast<int32>(Wrapper->GetHeight());
					bDecoded = Wrapper->GetRaw(ERGBFormat::BGRA, 8, Pixels);
				}
			}
#if WITH_FREEIMAGE_LIB
			if (!bDecoded && Compressed.Num() >= 12
				&& FMemory::Memcmp(Compressed.GetData(), "RIFF", 4) == 0
				&& FMemory::Memcmp(Compressed.GetData() + 8, "WEBP", 4) == 0)
			{
				FUEFreeImageWrapper::FreeImage_Initialise();
				FIMEMORY* Memory = FreeImage_OpenMemory(
					const_cast<BYTE*>(reinterpret_cast<const BYTE*>(Compressed.GetData())), Compressed.Num());
				FIBITMAP* SourceBitmap = Memory ? FreeImage_LoadFromMemory(FIF_WEBP, Memory) : nullptr;
				FIBITMAP* Bitmap = SourceBitmap ? FreeImage_ConvertTo32Bits(SourceBitmap) : nullptr;
				if (Bitmap)
				{
					Width = static_cast<int32>(FreeImage_GetWidth(Bitmap));
					Height = static_cast<int32>(FreeImage_GetHeight(Bitmap));
					if (Width > 0 && Height > 0 && Width <= 4096 && Height <= 4096)
					{
						Pixels.SetNumUninitialized(Width * Height * 4);
						for (int32 Y = 0; Y < Height; ++Y)
						{
							const BYTE* SourceLine = FreeImage_GetScanLine(Bitmap, Height - 1 - Y);
							FMemory::Memcpy(Pixels.GetData() + Y * Width * 4, SourceLine, Width * 4);
						}
						bDecoded = true;
					}
				}
				if (Bitmap)
				{
					FreeImage_Unload(Bitmap);
				}
				if (SourceBitmap)
				{
					FreeImage_Unload(SourceBitmap);
				}
				if (Memory)
				{
					FreeImage_CloseMemory(Memory);
				}
			}
#endif
			if (!bDecoded || Width <= 0 || Height <= 0 || Width > 4096 || Height > 4096)
			{
				Self->FailedCharacterAvatarDownloads.Add(AvatarUrl);
				UE_LOG(LogConvaiSceneAutoTaggerUi, Warning, TEXT("Character avatar image format was not supported or could not be decoded."));
				return;
			}
			auto CreateTexture = [WeakSelf, AvatarUrl, Width, Height, Pixels = MoveTemp(Pixels)]() mutable
			{
				TSharedPtr<SConvaiSceneAutoTagger> Pinned = WeakSelf.Pin();
				if (!Pinned.IsValid())
				{
					return;
				}
				UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
				if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.IsEmpty())
				{
					return;
				}
				FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
				void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
				const int64 ExpectedBytes = static_cast<int64>(Width) * Height * 4;
				if (Pixels.Num() != ExpectedBytes)
				{
					Mip.BulkData.Unlock();
					Pinned->FailedCharacterAvatarDownloads.Add(AvatarUrl);
					return;
				}
				FMemory::Memcpy(Data, Pixels.GetData(), ExpectedBytes);
				Mip.BulkData.Unlock();
				Texture->UpdateResource();
				FCharacterAvatarCacheEntry Entry;
				Entry.Texture = TStrongObjectPtr<UTexture2D>(Texture);
				Entry.Brush = MakeShared<FSlateBrush>();
				Entry.Brush->SetResourceObject(Texture);
				Entry.Brush->ImageSize = FVector2D(static_cast<float>(Width), static_cast<float>(Height));
				Entry.Brush->DrawAs = ESlateBrushDrawType::Image;
				Pinned->CharacterAvatarCache.Add(AvatarUrl, MoveTemp(Entry));
				FSlateApplication::Get().InvalidateAllWidgets(false);
			};
			if (IsInGameThread())
			{
				CreateTexture();
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, MoveTemp(CreateTexture));
			}
		});
	if (!Request->ProcessRequest())
	{
		PendingCharacterAvatarDownloads.Remove(AvatarUrl);
		FailedCharacterAvatarDownloads.Add(AvatarUrl);
	}
	return nullptr;
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildGuidancePanel()
{
	auto BuildLabel = [](const FText& Text)
	{
		return SNew(STextBlock).Text(Text).Font(SceneAutoTaggerVisual::MediumFont(11)).ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText());
	};
	return SNew(SRoundedBox)
		.BackgroundColor(SceneAutoTaggerVisual::Panel()).BorderColor(SceneAutoTaggerVisual::Border())
		.BorderThickness(1.0f).BorderRadius(9.0f).ContentPadding(FMargin(18.0f, 16.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DescribeSceneTitle", "2  Scene guidance"))
				.Font(SceneAutoTaggerVisual::MediumFont(15))
				.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SceneGuidanceHelper", "Optional context to guide how the scene is described."))
				.Font(SceneAutoTaggerVisual::RegularFont(10))
				.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.HeightOverride_Lambda([this]()
				{
					const FVector2D RootSize = GetCachedGeometry().GetLocalSize();
					if (RootSize.X < SetupWideLayoutThreshold)
					{
						return FOptionalSize();
					}
					const float PanelHeight = FMath::Clamp(RootSize.Y - 220.0f, 280.0f, 820.0f);
					return FOptionalSize(FMath::Max(180.0f, PanelHeight - 76.0f));
				})
				[
					SAssignNew(GuidanceScrollBox, SScrollBox)
					.Orientation(Orient_Vertical)
					.ScrollBarVisibility(bSetupUsesWideLayout ? EVisibility::Visible : EVisibility::Collapsed)
					.ScrollBarAlwaysVisible(bSetupUsesWideLayout)
					.ConsumeMouseWheel(bSetupUsesWideLayout
						? EConsumeMouseWheel::WhenScrollingPossible
						: EConsumeMouseWheel::Never)
					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					BuildLabel(LOCTEXT("SceneDescriptionLabel", "Scene description (optional)"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(0.0f)
					.OnClicked_Lambda([this]() { return bSceneContextDraftBusy ? HandleCancelSceneContextDraft() : HandleDraftSceneContextFromViewport(); })
					.IsEnabled_Lambda([this]() { return bSceneContextDraftBusy || CanDraftSceneContext(); })
					[
						SNew(SRoundedBox)
						.BackgroundColor(SceneAutoTaggerVisual::RowHover())
						.BorderColor(SceneAutoTaggerVisual::Border())
						.BorderThickness(1.0f)
						.BorderRadius(8.0f)
						.ContentPadding(FMargin(12.0f, 6.0f))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SThrobber)
								.NumPieces(3)
								.Visibility_Lambda([this]() { return bSceneContextDraftBusy ? EVisibility::Visible : EVisibility::Collapsed; })
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text_Lambda([this]() { return bSceneContextDraftBusy ? LOCTEXT("CancelDraft", "Cancel draft") : LOCTEXT("DraftCurrentView", "Draft from current view"); })
								.Font(SceneAutoTaggerVisual::MediumFont(10))
							]
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).HeightOverride_Lambda([this]()
				{
					const float Height = GetCachedGeometry().GetLocalSize().Y;
					return FOptionalSize(Height >= 900.0f ? 146.0f : (Height >= 800.0f ? 110.0f : 92.0f));
				})
				[
					SNew(SMultiLineEditableTextBox).Text(this, &SConvaiSceneAutoTagger::GetSceneContextDraftText)
					.HintText(LOCTEXT("SceneDescriptionHint", "Describe the environment and its purpose."))
					.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleSceneContextDraftChanged)
					.IsReadOnly_Lambda([this]() { return !CanEditSceneContext(); }).AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return SceneContextDraftError.IsEmpty() ? GetSceneContextDraftFeedbackText() : FText::FromString(SceneContextDraftError); })
				.Font(SceneAutoTaggerVisual::RegularFont(9))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 11.0f, 0.0f, 5.0f)[BuildLabel(LOCTEXT("DescriptionFocusLabel", "Description focus (optional)"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).HeightOverride_Lambda([this]()
				{
					const float Height = GetCachedGeometry().GetLocalSize().Y;
					return FOptionalSize(Height >= 900.0f ? 108.0f : (Height >= 800.0f ? 80.0f : 66.0f));
				})
				[
					SNew(SMultiLineEditableTextBox).Text(this, &SConvaiSceneAutoTagger::GetDescriptionFocusDraftText)
					.HintText(LOCTEXT("DescriptionFocusHint", "For example: prioritize interactive machinery and name visible controls."))
					.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleDescriptionFocusDraftChanged)
					.IsReadOnly_Lambda([this]() { return !CanEditSceneContext(); }).AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 35.0f, 0.0f, 0.0f)
			[
				SNew(SRoundedBox)
				.BackgroundColor(SceneAutoTaggerVisual::Row())
				.BorderColor(SceneAutoTaggerVisual::Border())
				.BorderThickness(1.0f)
				.BorderRadius(8.0f)
				.ContentPadding(FMargin(14.0f, 12.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AnalysisOptionsTitle", "Analysis options"))
						.Font(SceneAutoTaggerVisual::MediumFont(12))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AnalysisOptionsHelper", "Choose what to scan and how thoroughly to analyze it."))
						.Font(SceneAutoTaggerVisual::RegularFont(9))
						.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)[BuildLabel(LOCTEXT("SetupScopeLabel", "Scope"))]
							+ SVerticalBox::Slot().AutoHeight()[BuildScopeTogglePair(FName(TEXT("SceneAutoTagger.Scope.CurrentLevel")), FName(TEXT("SceneAutoTagger.Scope.SelectedActors")))]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)[BuildLabel(LOCTEXT("SetupQualityLabel", "Analysis quality"))]
							+ SVerticalBox::Slot().AutoHeight()[BuildAnalysisQualityTogglePair()]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 11.0f, 0.0f, 0.0f)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bIncludeAlreadyTaggedActors ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bIncludeAlreadyTaggedActors = State == ECheckBoxState::Checked; })
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ReanalyzeExisting", "Re-analyze existing tagged objects"))
							.Font(SceneAutoTaggerVisual::RegularFont(10))
						]
					]
				]
					]
				]
			]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCandidateListPanel()
{
	return SNew(SRoundedBox)
		.BorderRadius(0.0f)
		.BackgroundColor(SceneAutoTaggerVisual::Panel())
		.BorderColor(SceneAutoTaggerVisual::Border())
		.BorderThickness(1.0f)
		.ContentPadding(FMargin(12.0f, 11.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ObjectsReadyLabel", "Objects to review"))
					.Font(SceneAutoTaggerVisual::MediumFont(14))
					.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetVisibleCandidateCountText)
					.Font(SceneAutoTaggerVisual::RegularFont(10))
					.ColorAndOpacity(SceneAutoTaggerVisual::MutedText())
					.Tag(TEXT("SceneAutoTagger.VisibleCount"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SAssignNew(CandidateSearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search objects, descriptions, notes, or errors"))
					.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleSearchChanged)
					.ToolTipText(LOCTEXT("SearchTooltip", "Filter the virtualized review list without discarding any decisions or edits."))
					.AccessibleText(LOCTEXT("SearchAccessible", "Search object suggestions"))
					.Tag(TEXT("SceneAutoTagger.Search"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f)
			[
				SNew(SBox)
				.MinDesiredHeight(56.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(SceneAutoTaggerVisual::Toolbar())
					.Padding(FMargin(7.0f, 5.0f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(this, &SConvaiSceneAutoTagger::GetBatchSelectionCountText)
								.ToolTipText(this, &SConvaiSceneAutoTagger::GetBatchSelectionCountText)
								.AccessibleText(this, &SConvaiSceneAutoTagger::GetBatchSelectionCountText)
								.Font(SceneAutoTaggerVisual::MediumFont(9))
								.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
								.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 5.0f, 0.0f, 0.0f)
						[
							SNew(SWrapBox)
							.UseAllottedSize(true)
							.InnerSlotPadding(FVector2D(6.0f, 4.0f))
							+ SWrapBox::Slot()
							[
								SNew(SBox)
								.WidthOverride(120.0f)
								.HeightOverride(28.0f)
								[
									SNew(SButton)
									.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
									.ContentPadding(FMargin(10.0f, 0.0f))
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.OnClicked(this, &SConvaiSceneAutoTagger::HandleBatchDecision, ESceneAutoTaggerDecision::Accepted)
									.IsEnabled(this, &SConvaiSceneAutoTagger::CanIncludeBatchSelection)
									.ToolTipText(LOCTEXT("BatchAcceptTooltip", "Include eligible checked suggestions in the changes to apply. Selecting a row does not include it."))
									[
										SNew(STextBlock)
										.Text(LOCTEXT("BatchAccept", "Include checked"))
										.Font(SceneAutoTaggerVisual::MediumFont(9))
								]
							]
						]
						+ SWrapBox::Slot()
						[
								SNew(SBox)
								.WidthOverride(108.0f)
								.HeightOverride(28.0f)
								[
									SNew(SButton)
									.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
									.ContentPadding(FMargin(10.0f, 0.0f))
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.OnClicked(this, &SConvaiSceneAutoTagger::HandleBatchDecision, ESceneAutoTaggerDecision::Pending)
									.IsEnabled(this, &SConvaiSceneAutoTagger::CanClearBatchDecision)
									.ToolTipText(LOCTEXT("BatchClearDecisionTooltip", "Remove an existing decision from eligible checked rows. The suggestions remain in review."))
									[
										SNew(STextBlock)
										.Text(LOCTEXT("BatchClearDecision", "Clear decision"))
										.Font(SceneAutoTaggerVisual::MediumFont(9))
								]
							]
						]
						+ SWrapBox::Slot()
							[
								SNew(SBox)
								.WidthOverride(58.0f)
								.HeightOverride(28.0f)
								[
									SNew(SComboButton)
									.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
									.HasDownArrow(false)
									.ContentPadding(FMargin(8.0f, 0.0f))
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.OnGetMenuContent(this, &SConvaiSceneAutoTagger::BuildBatchActionsMenu)
									.IsEnabled(this, &SConvaiSceneAutoTagger::CanRunBatchSelectionActions)
									.ToolTipText(LOCTEXT("BatchMoreTooltip", "Analyze, recapture, or clear the checked rows."))
									.ButtonContent()
									[
										SNew(SBox)
										.HeightOverride(26.0f)
										.HAlign(HAlign_Center)
										.VAlign(VAlign_Center)
										[
											SNew(STextBlock)
											.Text(LOCTEXT("BatchMore", "More"))
											.Font(SceneAutoTaggerVisual::MediumFont(9))
											.Justification(ETextJustify::Center)
										]
									]
								]
							]
							+ SWrapBox::Slot()
							[
								SNew(SBox)
								.WidthOverride(184.0f)
								.HeightOverride(28.0f)
								.HAlign(HAlign_Center)
								.VAlign(VAlign_Center)
								[
									SNew(SCheckBox)
									.IsChecked_Lambda([this]()
									{
										return Controller.IsValid()
											&& Controller->ShouldGenerateMovementPointsOnApply()
											? ECheckBoxState::Checked
											: ECheckBoxState::Unchecked;
									})
									.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
									{
										if (Controller.IsValid())
										{
											Controller->SetGenerateMovementPointsOnApply(
												State == ECheckBoxState::Checked);
										}
									})
									.IsEnabled_Lambda([this]()
									{
										return !bEditingView && Controller.IsValid()
											&& !Controller->IsBusy();
									})
									.ToolTipText(LOCTEXT("GenerateMovementPointsOnApplyTooltip", "After applying included suggestions, generate best-effort navigation points beside those objects. Skipped points do not prevent metadata from being applied."))
									.Tag(TEXT("SceneAutoTagger.GenerateMovementPointsOnApply"))
									[
										SNew(STextBlock)
								.Text(LOCTEXT("GenerateMovementPointsOnApply", "Generate movement points"))
										.Font(SceneAutoTaggerVisual::RegularFont(9))
									]
								]
							]
							+ SWrapBox::Slot()
							[
								SNew(SBox)
								.WidthOverride(72.0f)
								.HeightOverride(28.0f)
								[
									SNew(SButton)
									.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
									.ContentPadding(FMargin(8.0f, 0.0f))
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.OnClicked(this, &SConvaiSceneAutoTagger::HandleApplyAccepted)
									.IsEnabled_Lambda([this]()
									{
										return !bEditingView && Controller.IsValid() && Controller->CanApply();
									})
									.ToolTipText(this, &SConvaiSceneAutoTagger::GetApplyToolTip)
									.AccessibleText(LOCTEXT("ApplyAccessible", "Apply included object suggestions to scene actors"))
									.Tag(TEXT("SceneAutoTagger.ApplyAccepted"))
									[
										SNew(STextBlock)
										.Text(this, &SConvaiSceneAutoTagger::GetApplyButtonText)
										.Font(SceneAutoTaggerVisual::MediumFont(9))
										.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
									]
								]
							]
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(CandidateListView, SListView<FCandidatePtr>)
					.ListItemsSource(&FilteredCandidates)
					.SelectionMode(ESelectionMode::Multi)
					.OnGenerateRow(this, &SConvaiSceneAutoTagger::GenerateCandidateRow)
					.OnMouseButtonClick(this, &SConvaiSceneAutoTagger::HandleCandidateClicked)
					.OnSelectionChanged(this, &SConvaiSceneAutoTagger::HandleCandidateSelectionChanged)
					.OnMouseButtonDoubleClick(this, &SConvaiSceneAutoTagger::HandleCandidateDoubleClicked)
					.OnKeyDownHandler(this, &SConvaiSceneAutoTagger::HandleCandidateListKeyDown)
					.OnContextMenuOpening(this, &SConvaiSceneAutoTagger::BuildCandidateContextMenu)
					.HandleSpacebarSelection(false)
					.ClearSelectionOnClick(false)
					.ToolTipText(LOCTEXT("CandidateListTooltip", "Select a row to inspect it. Check rows for batch actions; right-click selected rows for contextual actions or to add the current Level selection."))
					.AccessibleText(LOCTEXT("CandidateListAccessible", "Scene object suggestion review list"))
					.Tag(TEXT("SceneAutoTagger.CandidateList"))
					.HeaderRow
					(
						SNew(SHeaderRow)
						.ResizeMode(ESplitterResizeMode::Fill)
						.SplitterHandleSize(4.0f)
						+ SHeaderRow::Column(TEXT("Select"))
						.FixedWidth(30.0f)
						.HAlignHeader(HAlign_Center)
						.HAlignCell(HAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked(this, &SConvaiSceneAutoTagger::GetVisibleBatchSelectionState)
							.OnCheckStateChanged(this, &SConvaiSceneAutoTagger::HandleVisibleBatchSelectionChanged)
							.IsEnabled_Lambda([this]()
							{
								return Controller.IsValid() && !Controller->IsBusy()
									&& FilteredCandidates.ContainsByPredicate([this](const FCandidatePtr& Candidate)
									{
										return CanBatchSelectCandidate(Candidate);
									});
							})
							.AccessibleText_Lambda([this]()
							{
								switch (GetVisibleBatchSelectionState())
								{
								case ECheckBoxState::Checked:
									return LOCTEXT("BatchSelectVisibleCheckedAccessible", "All shown rows are checked. Activate to uncheck them.");
								case ECheckBoxState::Undetermined:
									return LOCTEXT("BatchSelectVisibleMixedAccessible", "Some shown rows are checked. Activate to check all shown rows.");
								default:
									return LOCTEXT("BatchSelectVisibleUncheckedAccessible", "No shown rows are checked. Activate to check all shown rows.");
								}
							})
							.ToolTipText(LOCTEXT("BatchSelectVisibleTooltip", "Check or clear all eligible rows currently shown by the search filter."))
							.Tag(TEXT("SceneAutoTagger.SelectVisibleRows"))
						]
						+ SHeaderRow::Column(TEXT("Actor"))
						.DefaultLabel(LOCTEXT("ActorColumn", "Object"))
						.FillWidth(1.0f)
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
						.MinSize(120.0f)
#endif
						+ SHeaderRow::Column(TEXT("Status"))
						.DefaultLabel(LOCTEXT("StatusColumn", "Status"))
						.FillSized(116.0f)
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
						.MinSize(96.0f)
#endif
						+ SHeaderRow::Column(TEXT("Confidence"))
						.DefaultLabel(LOCTEXT("ConfidenceColumn", "Score"))
						.FillSized(54.0f)
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
						.MinSize(48.0f)
#endif
						.HAlignHeader(HAlign_Right)
						.HeaderContentPadding(FMargin(2.0f, 0.0f))
					)
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Visibility(this, &SConvaiSceneAutoTagger::GetEmptyResultsVisibility)
					.Text(this, &SConvaiSceneAutoTagger::GetEmptyResultsText)
					.Font(SceneAutoTaggerVisual::RegularFont(10))
					.ColorAndOpacity(SceneAutoTaggerVisual::MutedText())
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.Tag(TEXT("SceneAutoTagger.EmptyResults"))
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildBatchActionsMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);
	const TWeakPtr<SConvaiSceneAutoTagger> WeakSelfForCallback = SharedThis(this);
	const TSet<FString> RefinementNoteActorPaths =
		GetRefinementNoteActorPaths(BatchSelectedActorPaths);
	MenuBuilder.BeginSection(
		TEXT("CheckedReviewActions"),
		FText::Format(
			LOCTEXT("CheckedReviewActionsHeading", "{0} checked"),
			FText::AsNumber(BatchSelectedActorPaths.Num())));
	MenuBuilder.AddMenuEntry(
		FText::Format(
			LOCTEXT("BatchIncludeCheckedFormat", "Include checked ({0})"),
			FText::AsNumber(GetIncludeEligibleCountForPaths(BatchSelectedActorPaths))),
		LOCTEXT("BatchIncludeCheckedMenuTooltip", "Stage every eligible checked suggestion for Apply."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
				{
					Self->HandleBatchDecision(ESceneAutoTaggerDecision::Accepted);
				}
			}),
			FCanExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
				return Self.IsValid() && Self->CanIncludeBatchSelection();
			})));
	MenuBuilder.AddMenuEntry(
		FText::Format(
			LOCTEXT("BatchClearCheckedFormat", "Clear checked decisions ({0})"),
			FText::AsNumber(GetClearDecisionEligibleCountForPaths(BatchSelectedActorPaths))),
		LOCTEXT("BatchClearCheckedMenuTooltip", "Return decided checked rows to Review."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
				{
					Self->HandleBatchDecision(ESceneAutoTaggerDecision::Pending);
				}
			}),
			FCanExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
				return Self.IsValid() && Self->CanClearBatchDecision();
			})));
	MenuBuilder.AddMenuSeparator();
	if (!RefinementNoteActorPaths.IsEmpty())
	{
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("BatchAnalyzeNotesFormat", "Analyze with notes ({0})"),
				FText::AsNumber(RefinementNoteActorPaths.Num())),
			LOCTEXT(
				"BatchAnalyzeNotesTooltip",
				"Analyze only checked rows with saved notes. Each row uses its own note and saved image; successful notes clear, while failed or canceled notes remain."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Edit")),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSelfForCallback, RefinementNoteActorPaths]()
				{
					if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
					{
						Self->HandleAnalyzeActorPaths(RefinementNoteActorPaths);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakSelfForCallback, RefinementNoteActorPaths]()
				{
					const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
					return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
						&& !Self->Controller->IsBusy() && !RefinementNoteActorPaths.IsEmpty();
				})));
	}
	MenuBuilder.AddMenuEntry(
		FText::Format(
			LOCTEXT("BatchAnalyzeCheckedFormat", "Analyze all checked ({0})"),
			FText::AsNumber(GetBatchAnalyzeEligibleCount())),
		LOCTEXT("BatchAnalyzeCheckedTooltip", "Analyze all eligible checked objects from their saved images. Any saved note on those rows is used. No recapture is performed."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
				{
					Self->HandleAnalyzeBatchSelection();
				}
			}),
			FCanExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
				return Self.IsValid() && Self->CanAnalyzeBatchSelection();
			})));
	MenuBuilder.AddMenuEntry(
		FText::Format(
			LOCTEXT("BatchRecaptureCheckedFormat", "Recapture & analyze checked ({0})"),
			FText::AsNumber(GetBatchRecaptureEligibleCount())),
		LOCTEXT("BatchRecaptureCheckedTooltip", "Capture fresh automatic views for the checked objects, then analyze them in bounded parallel batches. Any saved note on those rows is used."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
				{
					Self->HandleRecaptureAndAnalyzeBatchSelection();
				}
			}),
			FCanExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
				return Self.IsValid() && Self->CanRecaptureBatchSelection();
			})));
	MenuBuilder.AddMenuSeparator();
	MenuBuilder.AddMenuEntry(
		FText::Format(
			LOCTEXT("BatchRemoveCheckedFormat", "Remove checked from this review ({0})"),
			FText::AsNumber(GetRemovableCandidateCountForPaths(BatchSelectedActorPaths))),
		LOCTEXT("BatchRemoveCheckedTooltip", "Hide eligible checked rows from this review without deleting scene actors."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Delete")),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
				{
					Self->HandleRemoveCandidatesFromReview(Self->BatchSelectedActorPaths);
				}
			}),
			FCanExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
				return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
					&& !Self->Controller->IsBusy()
					&& Self->GetRemovableCandidateCountForPaths(Self->BatchSelectedActorPaths) > 0;
			})));
	MenuBuilder.AddMenuSeparator();
	MenuBuilder.AddMenuEntry(
		LOCTEXT("BatchClearSelection", "Uncheck all"),
		LOCTEXT("BatchClearSelectionTooltip", "Remove the checkmarks without changing any suggestion or decision."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([WeakSelfForCallback]()
		{
			if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
			{
				Self->HandleClearBatchSelection();
			}
		})));
	MenuBuilder.EndSection();
	return MenuBuilder.MakeWidget();
}

TSharedPtr<SWidget> SConvaiSceneAutoTagger::BuildCandidateContextMenu()
{
	TArray<FCandidatePtr> HighlightedCandidates;
	if (CandidateListView.IsValid())
	{
		HighlightedCandidates = CandidateListView->GetSelectedItems();
	}
	const TSet<FString> ActorPaths = ResolveContextActionPaths(
		HighlightedCandidates,
		PendingContextMenuCandidate);
	PendingContextMenuCandidate.Reset();
	const TSet<FString> RefinementNoteActorPaths = GetRefinementNoteActorPaths(ActorPaths);

	const TWeakPtr<SConvaiSceneAutoTagger> WeakSelfForCallback = SharedThis(this);
	FMenuBuilder MenuBuilder(true, nullptr);
	if (!ActorPaths.IsEmpty())
	{
		MenuBuilder.BeginSection(
			TEXT("HighlightedReviewActions"),
			FText::Format(
				LOCTEXT("HighlightedReviewActionsHeading", "{0} selected"),
				FText::AsNumber(ActorPaths.Num())));
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("ContextIncludeFormat", "Include selected ({0})"),
				FText::AsNumber(GetIncludeEligibleCountForPaths(ActorPaths))),
			LOCTEXT("ContextIncludeTooltip", "Stage eligible selected suggestions for Apply. Checkbox selection is unchanged."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
					{
						Self->HandleDecisionForActorPaths(
							ActorPaths,
							ESceneAutoTaggerDecision::Accepted,
							false);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
					return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
						&& !Self->Controller->IsBusy()
						&& Self->GetIncludeEligibleCountForPaths(ActorPaths) > 0;
				})));
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("ContextClearFormat", "Clear selected decisions ({0})"),
				FText::AsNumber(GetClearDecisionEligibleCountForPaths(ActorPaths))),
			LOCTEXT("ContextClearTooltip", "Return decided selected rows to Review. Checkbox selection is unchanged."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
					{
						Self->HandleDecisionForActorPaths(
							ActorPaths,
							ESceneAutoTaggerDecision::Pending,
							false);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
					return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
						&& !Self->Controller->IsBusy()
						&& Self->GetClearDecisionEligibleCountForPaths(ActorPaths) > 0;
				})));
		MenuBuilder.AddMenuSeparator();
		if (!RefinementNoteActorPaths.IsEmpty())
		{
			MenuBuilder.AddMenuEntry(
				FText::Format(
					LOCTEXT("ContextAnalyzeNotesFormat", "Analyze selected with notes ({0})"),
					FText::AsNumber(RefinementNoteActorPaths.Num())),
				LOCTEXT(
					"ContextAnalyzeNotesTooltip",
					"Analyze only selected rows with saved notes. Each row uses its own note and saved image; successful notes clear, while failed or canceled notes remain."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Edit")),
				FUIAction(
					FExecuteAction::CreateLambda([WeakSelfForCallback, RefinementNoteActorPaths]()
					{
						if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
						{
							Self->HandleAnalyzeActorPaths(RefinementNoteActorPaths);
						}
					}),
					FCanExecuteAction::CreateLambda([WeakSelfForCallback, RefinementNoteActorPaths]()
					{
						const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
						return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
							&& !Self->Controller->IsBusy() && !RefinementNoteActorPaths.IsEmpty();
					})));
		}
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("ContextAnalyzeFormat", "Analyze all selected ({0})"),
				FText::AsNumber(GetAnalyzeEligibleCountForPaths(ActorPaths))),
			LOCTEXT("ContextAnalyzeTooltip", "Analyze all eligible selected objects from their saved images without recapturing. Any saved note on those rows is used. Checkbox selection is unchanged."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
					{
						Self->HandleAnalyzeActorPaths(ActorPaths);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
					return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
						&& !Self->Controller->IsBusy()
						&& Self->GetAnalyzeEligibleCountForPaths(ActorPaths) > 0;
				})));
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("ContextRecaptureFormat", "Recapture & analyze ({0})"),
				FText::AsNumber(GetRecaptureEligibleCountForPaths(ActorPaths))),
			LOCTEXT("ContextRecaptureTooltip", "Capture fresh views for the selected objects, then analyze them in bounded parallel batches. Any saved note on those rows is used. Checkbox selection is unchanged."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
					{
						Self->HandleRecaptureAndAnalyzeActorPaths(ActorPaths);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
					return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
						&& !Self->Controller->IsBusy()
						&& Self->GetRecaptureEligibleCountForPaths(ActorPaths) > 0;
				})));
		MenuBuilder.AddMenuSeparator();
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("ContextRemoveFormat", "Remove selected from this review ({0})"),
				FText::AsNumber(GetRemovableCandidateCountForPaths(ActorPaths))),
			LOCTEXT("ContextRemoveTooltip", "Hide eligible selected rows from this review without deleting scene actors."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Delete")),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
					{
						Self->HandleRemoveCandidatesFromReview(ActorPaths);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakSelfForCallback, ActorPaths]()
				{
					const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
					return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
						&& !Self->Controller->IsBusy()
						&& Self->GetRemovableCandidateCountForPaths(ActorPaths) > 0;
				})));
		MenuBuilder.EndSection();
	}

	const int32 SelectedLevelActorCount = GetSelectedActorCount();
	MenuBuilder.BeginSection(TEXT("LevelSelectionActions"), LOCTEXT("LevelSelectionHeading", "Level selection"));
	MenuBuilder.AddMenuEntry(
		FText::Format(
			LOCTEXT("AddSelectedLevelActorsFormat", "Add selected Level actors to this review ({0})"),
			FText::AsNumber(SelectedLevelActorCount)),
		SelectedLevelActorCount > 0
			? LOCTEXT("AddSelectedLevelActorsTooltip", "Append and analyze the current Level Editor selection using this review's saved guidance and quality. Existing rows are preserved.")
			: LOCTEXT("AddSelectedLevelActorsDisabledTooltip", "Select one or more actors in the Level Editor first."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Plus")),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				if (const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin())
				{
					Self->HandleAddSelectedActorsToReview();
				}
			}),
			FCanExecuteAction::CreateLambda([WeakSelfForCallback]()
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Self = WeakSelfForCallback.Pin();
				return Self.IsValid() && !Self->bEditingView && Self->Controller.IsValid()
					&& !Self->Controller->IsBusy() && Self->GetSelectedActorCount() > 0;
			})));
	MenuBuilder.EndSection();
	return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildEvidencePanel()
{
	const FLinearColor Surface = SceneAutoTaggerVisual::Panel();
	const FLinearColor Border = SceneAutoTaggerVisual::Border();
	const FLinearColor PrimaryText = SceneAutoTaggerVisual::PrimaryText();
	const FLinearColor SecondaryText = SceneAutoTaggerVisual::SecondaryText();

	return SNew(SRoundedBox)
		.BorderRadius(0.0f)
		.BackgroundColor(Surface)
		.BorderColor(Border)
		.BorderThickness(1.0f)
		.ContentPadding(FMargin(12.0f, 11.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (bEditingView)
					{
						return LOCTEXT("AdjustEvidenceLabel", "Adjust view");
					}
					if (SelectedCandidate.IsValid() && SelectedCandidate->bUserCapturePendingAnalysis)
					{
						return LOCTEXT("AdjustedEvidenceLabel", "Saved view");
					}
					return LOCTEXT("ExactEvidenceLabel", "Analysis image");
				})
				.Font(SceneAutoTaggerVisual::MediumFont(14))
				.ColorAndOpacity(PrimaryText)
				.ToolTipText_Lambda([this]()
				{
					if (bEditingView)
					{
						return LOCTEXT("EditingEvidenceTooltip", "A local preview of the current unsaved camera adjustment.");
					}
					if (SelectedCandidate.IsValid() && SelectedCandidate->bUserCapturePendingAnalysis)
					{
						return LOCTEXT("PendingEvidenceTooltip", "This adjusted view is saved locally but has not been analyzed yet.");
					}
					return LOCTEXT("ExactEvidenceTooltip", "The isolated image used by Convai Vision for this suggestion.");
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.Text(this, &SConvaiSceneAutoTagger::GetSelectedActorTitle)
				.Font(SceneAutoTaggerVisual::RegularFont(10))
				.ColorAndOpacity(SecondaryText)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SRoundedBox)
				.BorderRadius(6.0f)
				.BackgroundColor(SceneAutoTaggerVisual::Preview())
				.BorderColor(Border)
				.BorderThickness(1.0f)
				.ContentPadding(FMargin(5.0f))
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SScaleBox)
						.Stretch(EStretch::ScaleToFit)
						.StretchDirection(EStretchDirection::Both)
						[
							SNew(SOverlay)
							+ SOverlay::Slot()
							[
								SAssignNew(PreviewImageWidget, SImage)
								.Image(nullptr)
								.Visibility(EVisibility::Collapsed)
								.Tag(TEXT("SceneAutoTagger.Evidence.ModelInputView"))
							]
							+ SOverlay::Slot()
							[
								SAssignNew(LivePreviewImageWidget, SSceneAutoTaggerLiveCaptureImage)
								.Image_Lambda([this]()
								{
									return FDeferredCleanupSlateBrush::TrySlateBrush(LivePreviewBrush);
								})
								.Visibility_Lambda([this]()
								{
									return bEditingView && LivePreviewBrush.IsValid()
										? EVisibility::HitTestInvisible
										: EVisibility::Collapsed;
								})
								.ForceVolatile(true)
							]
						]
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SAssignNew(PreviewPlaceholderWidget, STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetPreviewPlaceholderText)
						.Font(SceneAutoTaggerVisual::RegularFont(10))
						.ColorAndOpacity(SecondaryText)
						.Visibility(EVisibility::Visible)
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
					]
					+ SOverlay::Slot()
					[
						SNew(SSceneAutoTaggerViewInput)
						.Visibility(this, &SConvaiSceneAutoTagger::GetEditViewVisibility)
						.OnOrbit(this, &SConvaiSceneAutoTagger::HandleEditViewOrbit)
						.OnPan(this, &SConvaiSceneAutoTagger::HandleEditViewPan)
						.OnZoom(this, &SConvaiSceneAutoTagger::HandleEditViewZoom)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Bottom)
					.Padding(8.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("EditViewInstructions", "Drag: orbit  •  Shift/right-drag: pan  •  Wheel: zoom"))
						.Visibility_Lambda([this]() { return bEditingView ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
						.ColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.94f, 0.96f, 0.9f)))
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
						.WrapTextAt(260.0f)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 7.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SConvaiSceneAutoTagger::GetEvidenceCaptionText)
				.Font(SceneAutoTaggerVisual::RegularFont(10))
				.ColorAndOpacity(SecondaryText)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 9.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 4.0f))
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetStaticViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
						.ContentPadding(FMargin(9.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleSelectActor)
						.IsEnabled_Lambda([this]() { return SelectedCandidate.IsValid() && SelectedCandidate->Actor.IsValid(); })
						.ToolTipText(LOCTEXT("SelectActorTooltip", "Replace the Level Editor selection with this actor."))
						.Tag(TEXT("SceneAutoTagger.SelectActor"))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("SelectActor", "Select in Level"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetStaticViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(9.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleFocusActor)
						.IsEnabled_Lambda([this]() { return SelectedCandidate.IsValid() && SelectedCandidate->Actor.IsValid(); })
						.ToolTipText(LOCTEXT("FocusActorTooltip", "Select this actor and frame it in the active Level Editor viewport."))
						.Tag(TEXT("SceneAutoTagger.FocusActor"))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("FocusActor", "Focus"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetStaticViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(9.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleBeginEditView)
						.IsEnabled_Lambda([this]() { return SelectedCandidate.IsValid() && SelectedCandidate->Actor.IsValid() && Controller.IsValid() && !Controller->IsBusy(); })
						.ToolTipText(LOCTEXT("EditEvidenceViewTooltip", "Adjust the isolated camera without moving the actor or the Level Editor viewport."))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("EditEvidenceView", "Edit View"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetEditViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(9.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleResetEditView)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ResetEvidenceView", "Reset"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetEditViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(9.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleCancelEditView)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CancelEvidenceView", "Cancel"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetEditViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(10.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleUseEditedView)
						.IsEnabled_Lambda([this]() { return bEditableCaptureMatchesCamera && bEditablePreviewResourceReady; })
						.ToolTipText_Lambda([this]()
						{
							return bEditableCaptureMatchesCamera && bEditablePreviewResourceReady
								? LOCTEXT("UseEvidenceViewTooltip", "Save this exact view locally without analyzing it yet. The row is checked so it can be analyzed with other selected objects.")
								: LOCTEXT("UseEvidenceViewWaitingTooltip", "Wait for the live preview to become ready.");
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("UseEvidenceView", "Use view"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
				+ SWrapBox::Slot()
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					.Visibility(this, &SConvaiSceneAutoTagger::GetEditViewVisibility)
					[
						SNew(SButton)
						.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
						.ContentPadding(FMargin(10.0f, 0.0f))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SConvaiSceneAutoTagger::HandleUseAndAnalyzeEditedView)
						.IsEnabled_Lambda([this]() { return bEditableCaptureMatchesCamera && bEditablePreviewResourceReady; })
						.ToolTipText_Lambda([this]()
						{
							return bEditableCaptureMatchesCamera && bEditablePreviewResourceReady
								? LOCTEXT("UseAndAnalyzeEvidenceViewTooltip", "Save this exact view and immediately analyze only this object.")
								: LOCTEXT("UseAndAnalyzeEvidenceViewWaitingTooltip", "Wait for the live preview to become ready.");
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("UseAndAnalyzeEvidenceView", "Use & analyze"))
							.Font(SceneAutoTaggerVisual::MediumFont(10))
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildCandidateDetailsPanel()
{
	const FLinearColor PrimaryText = SceneAutoTaggerVisual::PrimaryText();
	const FLinearColor SecondaryText = SceneAutoTaggerVisual::SecondaryText();
	const FLinearColor MutedText = SceneAutoTaggerVisual::MutedText();

	return SNew(SRoundedBox)
		.BorderRadius(0.0f)
		.BackgroundColor(SceneAutoTaggerVisual::Panel())
		.BorderColor(SceneAutoTaggerVisual::Border())
		.BorderThickness(1.0f)
		.ContentPadding(FMargin(12.0f, 11.0f))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return SelectedCandidate.IsValid()
								&& SelectedCandidate->bUserCapturePendingAnalysis
								? LOCTEXT("PreviousMetadataPanelTitle", "Previous suggestion")
								: LOCTEXT("MetadataPanelTitle", "Editable suggestion");
						})
						.Font(SceneAutoTaggerVisual::MediumFont(14))
						.ColorAndOpacity(PrimaryText)
						.ToolTipText(this, &SConvaiSceneAutoTagger::GetSelectedActorSubtext)
						.Tag(TEXT("SceneAutoTagger.Detail.Actor"))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ActorScopedSuggestion", "Actor-scoped"))
						.Font(SceneAutoTaggerVisual::RegularFont(9))
						.ColorAndOpacity(MutedText)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetSelectedActorTitle)
					.Font(SceneAutoTaggerVisual::RegularFont(10))
					.ColorAndOpacity(SecondaryText)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 3.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NameFieldLabel", "Name"))
					.Font(SceneAutoTaggerVisual::MediumFont(10))
					.ColorAndOpacity(SecondaryText)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SEditableTextBox)
					.Text(this, &SConvaiSceneAutoTagger::GetSelectedNameText)
					.Font(SceneAutoTaggerVisual::RegularFont(11))
					.HintText(LOCTEXT("NameFieldHint", "One- to three-word spoken name"))
					.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleNameChanged)
					.OnTextCommitted(this, &SConvaiSceneAutoTagger::HandleNameCommitted)
					.IsEnabled(this, &SConvaiSceneAutoTagger::CanModifySelected)
					.ToolTipText(LOCTEXT("NameFieldTooltip", "Name stored in the Convai Object Component. Use one to three natural spoken words so action matching stays reliable."))
					.AccessibleText(LOCTEXT("NameFieldAccessible", "Editable Convai object name"))
					.Tag(TEXT("SceneAutoTagger.Detail.Name"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 3.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DescriptionFieldLabel", "Description"))
					.Font(SceneAutoTaggerVisual::MediumFont(10))
					.ColorAndOpacity(SecondaryText)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.HeightOverride(125.0f)
					[
						SNew(SMultiLineEditableTextBox)
						.Text(this, &SConvaiSceneAutoTagger::GetSelectedDescriptionText)
						.Font(SceneAutoTaggerVisual::RegularFont(11))
						.AutoWrapText(true)
						.HintText(LOCTEXT("DescriptionFieldHint", "Useful visible description for conversation"))
						.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleDescriptionChanged)
						.OnTextCommitted(this, &SConvaiSceneAutoTagger::HandleDescriptionCommitted)
						.IsEnabled(this, &SConvaiSceneAutoTagger::CanModifySelected)
						.ToolTipText(LOCTEXT("DescriptionFieldTooltip", "Description stored in the Convai Object Component (320 characters maximum)."))
						.AccessibleText(LOCTEXT("DescriptionFieldAccessible", "Editable Convai object description"))
						.Tag(TEXT("SceneAutoTagger.Detail.Description"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					BuildRefinementPanel()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetSelectedScoreText)
					.Font(SceneAutoTaggerVisual::RegularFont(10))
					.ColorAndOpacity(SecondaryText)
					.ToolTipText(LOCTEXT("ScoreTooltip", "Local relevance controls which objects are analyzed. AI confidence is the uncalibrated value reported after analysis."))
					.AutoWrapText(true)
					.Tag(TEXT("SceneAutoTagger.Detail.Scores"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneAutoTagger::GetSelectedReasonText)
					.Font(SceneAutoTaggerVisual::RegularFont(9))
					.ColorAndOpacity(MutedText)
					.AutoWrapText(true)
					.Tag(TEXT("SceneAutoTagger.Detail.Reason"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SBorder)
					.Visibility(this, &SConvaiSceneAutoTagger::GetSelectedErrorVisibility)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.BorderBackgroundColor(FLinearColor(0.45f, 0.08f, 0.06f, 1.0f))
					.Padding(7.0f)
					[
						SNew(STextBlock)
						.Text(this, &SConvaiSceneAutoTagger::GetSelectedErrorText)
						.ColorAndOpacity(this, &SConvaiSceneAutoTagger::GetSelectedErrorColor)
						.AutoWrapText(true)
						.Tag(TEXT("SceneAutoTagger.Detail.Error"))
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildRefinementPanel()
{
	const FLinearColor PrimaryText = SceneAutoTaggerVisual::PrimaryText();
	const FLinearColor SecondaryText = SceneAutoTaggerVisual::SecondaryText();
	const FLinearColor MutedText = SceneAutoTaggerVisual::MutedText();

	return SNew(SVerticalBox)
		.Visibility(this, &SConvaiSceneAutoTagger::GetRefinementSectionVisibility)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
			.Thickness(1.0f)
			.SeparatorImage(FAppStyle::GetBrush("WhiteBrush"))
			.ColorAndOpacity(SceneAutoTaggerVisual::Border())
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 9.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RefinementTitle", "Refine suggestion"))
				.Font(SceneAutoTaggerVisual::MediumFont(11))
				.ColorAndOpacity(PrimaryText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SConvaiSceneAutoTagger::GetRefinementNoteCharacterCountText)
				.Font(SceneAutoTaggerVisual::RegularFont(9))
				.ColorAndOpacity(MutedText)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 3.0f, 0.0f, 6.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RefinementHelp", "Add one correction or detail for the agent to use when revising this object."))
			.Font(SceneAutoTaggerVisual::RegularFont(9))
			.ColorAndOpacity(SecondaryText)
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.HeightOverride(64.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text(this, &SConvaiSceneAutoTagger::GetSelectedRefinementNoteText)
				.Font(SceneAutoTaggerVisual::RegularFont(10))
				.AutoWrapText(true)
				.HintText(LOCTEXT("RefinementHint", "What should the agent correct or emphasize?"))
				.OnTextChanged(this, &SConvaiSceneAutoTagger::HandleRefinementNoteChanged)
				.IsEnabled(this, &SConvaiSceneAutoTagger::CanEditRefinementNote)
				.ToolTipText(LOCTEXT("RefinementTooltip", "For example: This is Ganesha, the elephant-headed Hindu deity. The note applies only to this object and is not stored in the scene."))
				.AccessibleText(LOCTEXT("RefinementAccessible", "Optional one-time note for refining this suggestion"))
				.Tag(TEXT("SceneAutoTagger.Detail.RefinementNote"))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 7.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.HeightOverride(28.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SceneAutoTaggerVisual::PrimaryButtonStyle())
					.ContentPadding(FMargin(11.0f, 0.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("AnalyzeWithNote", "Analyze with note"))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleAnalyzeWithRefinementNote)
					.IsEnabled(this, &SConvaiSceneAutoTagger::CanAnalyzeWithRefinementNote)
					.ToolTipText(LOCTEXT("AnalyzeWithNoteTooltip", "Analyze this object's saved image using the note and current editable suggestion. A successful update returns the row to Review."))
					.Tag(TEXT("SceneAutoTagger.Detail.AnalyzeWithNote"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(7.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.HeightOverride(28.0f)
				.Visibility(this, &SConvaiSceneAutoTagger::GetClearRefinementNoteVisibility)
				[
					SNew(SButton)
					.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(10.0f, 0.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("ClearRefinementNote", "Clear note"))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleClearRefinementNote)
					.IsEnabled(this, &SConvaiSceneAutoTagger::CanEditRefinementNote)
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(this, &SConvaiSceneAutoTagger::GetRefinementNoteFooterText)
			.Font(SceneAutoTaggerVisual::RegularFont(9))
			.ColorAndOpacity(MutedText)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildErrorBanner()
{
	return SNew(SBorder)
		.Visibility(this, &SConvaiSceneAutoTagger::GetErrorBannerVisibility)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.48f, 0.08f, 0.06f, 1.0f))
		.Padding(FMargin(10.0f, 7.0f))
		[
			SNew(STextBlock)
			.Text(this, &SConvaiSceneAutoTagger::GetErrorBannerText)
			.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.62f, 0.58f)))
			.AutoWrapText(true)
			.Tag(TEXT("SceneAutoTagger.ResultError"))
		];
}

TSharedRef<SWidget> SConvaiSceneAutoTagger::BuildStickyFooter()
{
	return SNew(SBorder)
		.Visibility(this, &SConvaiSceneAutoTagger::GetReviewVisibility)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(SceneAutoTaggerVisual::Header())
		.Padding(FMargin(14.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SConvaiSceneAutoTagger::GetDecisionCountsText)
				.Font(SceneAutoTaggerVisual::MediumFont(10))
				.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
				.ToolTipText(LOCTEXT("DecisionCountsTooltip", "Only included suggestions are written by Apply. Every other row remains unchanged in the scene."))
				.Tag(TEXT("SceneAutoTagger.DecisionCounts"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(14.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]()
					{
						return GetApplySummaryVisibility() == EVisibility::Visible
							? EVisibility::Collapsed
							: EVisibility::HitTestInvisible;
					})
					.Text(LOCTEXT("ApplyReassurance", "Included suggestions are staged until you apply the changes."))
					.Font(SceneAutoTaggerVisual::RegularFont(9))
					.ColorAndOpacity(SceneAutoTaggerVisual::MutedText())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.ToolTipText(LOCTEXT("ApplyReassuranceTooltip", "Included suggestions are not written to the scene until you apply the changes."))
				]
				+ SOverlay::Slot()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Visibility(this, &SConvaiSceneAutoTagger::GetApplySummaryVisibility)
					.Text(this, &SConvaiSceneAutoTagger::GetApplySummaryText)
					.Font(SceneAutoTaggerVisual::RegularFont(9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.22f, 0.78f, 0.42f)))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.ToolTipText(this, &SConvaiSceneAutoTagger::GetApplySummaryText)
					.Tag(TEXT("SceneAutoTagger.ApplySummary"))
				]
			]
		];
}

TSharedRef<ITableRow> SConvaiSceneAutoTagger::GenerateCandidateRow(FCandidatePtr Candidate, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SSceneAutoTaggerCandidateRow, OwnerTable)
		.Candidate(Candidate)
		.OnBatchSelectionChanged(this, &SConvaiSceneAutoTagger::HandleCandidateBatchSelectionChanged)
		.OnRowPressed(this, &SConvaiSceneAutoTagger::HandleCandidateRowPressed)
		.BatchSelectionState_Lambda([this, Candidate]() { return GetCandidateBatchSelectionState(Candidate); })
		.CanBatchSelect_Lambda([this, Candidate]()
		{
			return Controller.IsValid() && !Controller->IsBusy()
				&& CanBatchSelectCandidate(Candidate);
		});
}

TSharedRef<ITableRow> SConvaiSceneAutoTagger::GenerateLowInformationCandidateRow(
	FCandidatePtr Candidate,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FCandidatePtr>, OwnerTable)
		.Padding(FMargin(0.0f, 2.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(SceneAutoTaggerVisual::Row())
			.Padding(FMargin(9.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 9.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this, Candidate]()
					{
						return Candidate.IsValid()
							&& LowInformationSelectedActorPaths.Contains(Candidate->ActorPath)
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this, Candidate](const ECheckBoxState NewState)
					{
						HandleLowInformationSelectionChanged(Candidate, NewState);
					})
					.IsEnabled_Lambda([this, Candidate]()
					{
						return Candidate.IsValid() && Candidate->Actor.IsValid()
							&& Controller.IsValid() && !Controller->IsBusy();
					})
					.ToolTipText(LOCTEXT("LowInformationSelectTooltip", "Check this object to restore and analyze it."))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([Candidate]()
						{
							return Candidate.IsValid()
								? FText::FromString(Candidate->ActorLabel)
								: FText::GetEmpty();
						})
						.Font(SceneAutoTaggerVisual::MediumFont(10))
						.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([Candidate]()
						{
							return Candidate.IsValid()
								? LowInformationReasonText(Candidate->LowInformationReason)
								: FText::GetEmpty();
						})
						.Font(SceneAutoTaggerVisual::RegularFont(9))
						.ColorAndOpacity(SceneAutoTaggerVisual::SecondaryText())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(10.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("FocusLowInformationCandidate", "Focus"))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleFocusLowInformationCandidate, Candidate)
					.IsEnabled_Lambda([Candidate]() { return Candidate.IsValid() && Candidate->Actor.IsValid(); })
					.ToolTipText(LOCTEXT("FocusLowInformationCandidateTooltip", "Select and frame this actor in the Level Editor without closing this summary."))
				]
			]
		];
}

TSharedRef<ITableRow> SConvaiSceneAutoTagger::GenerateAttentionCandidateRow(
	FCandidatePtr Candidate,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FCandidatePtr>, OwnerTable)
		.Padding(FMargin(0.0f, 2.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(SceneAutoTaggerVisual::Row())
			.Padding(FMargin(9.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([Candidate]()
						{
							return Candidate.IsValid()
								? FText::FromString(Candidate->ActorLabel)
								: FText::GetEmpty();
						})
						.Font(SceneAutoTaggerVisual::MediumFont(10))
						.ColorAndOpacity(SceneAutoTaggerVisual::PrimaryText())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([Candidate]()
						{
							if (!Candidate.IsValid())
							{
								return FText::GetEmpty();
							}
							return FText::FromString(Candidate->Actor.IsValid()
								? Candidate->Error
								: TEXT("The actor is no longer available in this level."));
						})
						.Font(SceneAutoTaggerVisual::RegularFont(9))
						.ColorAndOpacity(FLinearColor(0.95f, 0.42f, 0.35f))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.ToolTipText_Lambda([Candidate]()
						{
							return Candidate.IsValid()
								? FText::FromString(Candidate->Error)
								: FText::GetEmpty();
						})
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SceneAutoTaggerVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(10.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("InspectAttentionCandidate", "Inspect"))
					.OnClicked(this, &SConvaiSceneAutoTagger::HandleInspectAttentionCandidate, Candidate)
					.IsEnabled_Lambda([Candidate]() { return Candidate.IsValid(); })
					.ToolTipText(LOCTEXT("InspectAttentionCandidateTooltip", "Close this summary and open the item in review without changing its decision."))
				]
			]
		];
}

FReply SConvaiSceneAutoTagger::HandleCandidateRowPressed(
	FCandidatePtr Candidate,
	const FPointerEvent& PointerEvent)
{
	if (!Candidate.IsValid() || !CandidateListView.IsValid())
	{
		return FReply::Unhandled();
	}
	if (PointerEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		PendingMouseSelectionCandidate.Reset();
		PendingContextMenuCandidate = Candidate;
		if (!CandidateListView->IsItemSelected(Candidate))
		{
			TGuardValue<bool> SuppressSelectionChanged(bSuppressCandidateSelectionChanged, true);
			CandidateListView->ClearSelection();
			CandidateListView->SetItemSelection(Candidate, true, ESelectInfo::OnMouseClick);
		}
		CandidateSelectionAnchor = Candidate;
		HandleCandidateSelectionChanged(Candidate, ESelectInfo::Direct);
		return FReply::Handled().SetUserFocus(CandidateListView.ToSharedRef(), EFocusCause::Mouse);
	}

	PendingContextMenuCandidate.Reset();
	PendingMouseSelectionCandidate = MoveTemp(Candidate);

	const FCandidatePtr PressedCandidate = PendingMouseSelectionCandidate;
	const bool bShiftDown = PointerEvent.IsShiftDown();
	const bool bControlDown = PointerEvent.IsControlDown();
	if (!bShiftDown && !bControlDown)
	{
		CandidateSelectionAnchor = PressedCandidate;
		return FReply::Unhandled();
	}

	PendingMouseSelectionCandidate.Reset();
	if (bShiftDown)
	{
		int32 RangeEnd = FilteredCandidates.IndexOfByKey(PressedCandidate);
		int32 RangeStart = FilteredCandidates.IndexOfByKey(CandidateSelectionAnchor);
		if (RangeStart == INDEX_NONE)
		{
			RangeStart = FilteredCandidates.IndexOfByKey(SelectedCandidate);
		}
		if (RangeStart == INDEX_NONE)
		{
			RangeStart = RangeEnd;
			CandidateSelectionAnchor = PressedCandidate;
		}

		if (RangeStart != INDEX_NONE && RangeEnd != INDEX_NONE)
		{
			TArray<FCandidatePtr> RangeCandidates;
			const int32 FirstIndex = FMath::Min(RangeStart, RangeEnd);
			const int32 LastIndex = FMath::Max(RangeStart, RangeEnd);
			RangeCandidates.Reserve(LastIndex - FirstIndex + 1);
			for (int32 Index = FirstIndex; Index <= LastIndex; ++Index)
			{
				if (FilteredCandidates[Index].IsValid())
				{
					RangeCandidates.Add(FilteredCandidates[Index]);
				}
			}

			TGuardValue<bool> SuppressSelectionChanged(bSuppressCandidateSelectionChanged, true);
			if (!bControlDown)
			{
				CandidateListView->ClearSelection();
			}
			CandidateListView->SetItemSelection(
				RangeCandidates,
				true,
				ESelectInfo::Direct);
			// Keep Slate's keyboard selector on the endpoint without delegating
			// range membership back to Slate's separate, stale range anchor.
			CandidateListView->SetItemSelection(
				PressedCandidate,
				true,
				ESelectInfo::OnMouseClick);
		}
		HandleCandidateSelectionChanged(PressedCandidate, ESelectInfo::Direct);
	}
	else
	{
		const bool bSelectPressed = !CandidateListView->IsItemSelected(PressedCandidate);
		{
			TGuardValue<bool> SuppressSelectionChanged(bSuppressCandidateSelectionChanged, true);
			CandidateListView->SetItemSelection(
				PressedCandidate,
				bSelectPressed,
				ESelectInfo::OnMouseClick);
		}
		CandidateSelectionAnchor = PressedCandidate;

		FCandidatePtr InspectorCandidate = bSelectPressed ? PressedCandidate : nullptr;
		if (!InspectorCandidate.IsValid())
		{
			for (const FCandidatePtr& VisibleCandidate : FilteredCandidates)
			{
				if (CandidateListView->IsItemSelected(VisibleCandidate))
				{
					InspectorCandidate = VisibleCandidate;
					break;
				}
			}
		}
		HandleCandidateSelectionChanged(InspectorCandidate, ESelectInfo::Direct);
	}

	return FReply::Handled().SetUserFocus(CandidateListView.ToSharedRef(), EFocusCause::Mouse);
}

void SConvaiSceneAutoTagger::HandleCandidateClicked(FCandidatePtr Candidate)
{
	PendingMouseSelectionCandidate.Reset();
	PendingContextMenuCandidate.Reset();
	if (CandidateListView.IsValid() && !CandidateListView->IsItemSelected(Candidate))
	{
		// Ctrl-click can remove the pressed row. The native selection-changed
		// callback has already moved the inspector to a remaining selected row
		// (or cleared it), so do not restore the row that was just deselected.
		return;
	}
	HandleCandidateSelectionChanged(MoveTemp(Candidate), ESelectInfo::OnMouseClick);
}

FReply SConvaiSceneAutoTagger::HandleCandidateListKeyDown(
	const FGeometry& /*Geometry*/,
	const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() != EKeys::SpaceBar || KeyEvent.IsAltDown() || KeyEvent.IsRepeat())
	{
		return FReply::Unhandled();
	}
	if (!Controller.IsValid() || Controller->IsBusy())
	{
		return FReply::Handled();
	}

	TArray<FCandidatePtr> SelectedRows;
	if (CandidateListView.IsValid())
	{
		SelectedRows = CandidateListView->GetSelectedItems();
	}

	TArray<FCandidatePtr> EligibleRows;
	for (const FCandidatePtr& Candidate : SelectedRows)
	{
		if (CanBatchSelectCandidate(Candidate) && !Candidate->ActorPath.IsEmpty())
		{
			EligibleRows.Add(Candidate);
		}
	}

	if (EligibleRows.IsEmpty())
	{
		return FReply::Handled();
	}

	const bool bAllAlreadyChecked = !EligibleRows.ContainsByPredicate([this](const FCandidatePtr& Candidate)
	{
		return !BatchSelectedActorPaths.Contains(Candidate->ActorPath);
	});
	for (const FCandidatePtr& Candidate : EligibleRows)
	{
		if (bAllAlreadyChecked)
		{
			BatchSelectedActorPaths.Remove(Candidate->ActorPath);
		}
		else
		{
			BatchSelectedActorPaths.Add(Candidate->ActorPath);
		}
	}

	CandidateListView->RequestListRefresh();
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::HandleCandidateSelectionChanged(FCandidatePtr Candidate, ESelectInfo::Type SelectInfo)
{
	if (bSuppressCandidateSelectionChanged)
	{
		return;
	}
	if (SelectInfo == ESelectInfo::OnMouseClick && PendingMouseSelectionCandidate.IsValid())
	{
		const FCandidatePtr ClickedCandidate = PendingMouseSelectionCandidate;
		PendingMouseSelectionCandidate.Reset();
		if (!CandidateListView.IsValid() || CandidateListView->IsItemSelected(ClickedCandidate))
		{
			// SelectionChanged receives an arbitrary member of a multi-selection.
			// Prefer the row the user actually pressed while it remains selected.
			Candidate = ClickedCandidate;
		}
		else if (!Candidate.IsValid() || !CandidateListView->IsItemSelected(Candidate))
		{
			const TArray<FCandidatePtr> SelectedRows = CandidateListView->GetSelectedItems();
			Candidate = SelectedRows.IsEmpty() ? nullptr : SelectedRows[0];
		}
	}
	else if (SelectInfo != ESelectInfo::Direct)
	{
		PendingMouseSelectionCandidate.Reset();
	}
	if (Candidate == SelectedCandidate)
	{
		return;
	}
	EndEditView();
	SelectedCandidate = MoveTemp(Candidate);
	CommitPendingCandidateEdit();
	RefreshSelectedPreview();
	LocalError.Reset();
}

void SConvaiSceneAutoTagger::HandleCandidateDoubleClicked(FCandidatePtr Candidate)
{
	EndEditView();
	SelectedCandidate = MoveTemp(Candidate);
	CommitPendingCandidateEdit();
	RefreshSelectedPreview();
	if (CandidateListView.IsValid() && SelectedCandidate.IsValid())
	{
		CandidateListView->SetSelection(SelectedCandidate, ESelectInfo::Direct);
	}
	HandleFocusActor();
}

void SConvaiSceneAutoTagger::HandleControllerChanged()
{
	RefreshCandidateList();
	const bool bSummaryVisible =
		GetCompletionSummaryVisibility() == EVisibility::Visible;
	if (bSummaryVisible && !bCompletionSummaryWasVisible
		&& CompletionContinueButton.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			CompletionContinueButton,
			EFocusCause::SetDirectly);
	}
	bCompletionSummaryWasVisible = bSummaryVisible;
}

void SConvaiSceneAutoTagger::HandleSearchChanged(const FText& NewText)
{
	SearchText = NewText.ToString();
	SearchText.TrimStartAndEndInline();
	RefreshCandidateList();
}

void SConvaiSceneAutoTagger::RefreshCandidateList()
{
	const FString PreviousActorPath = SelectedCandidate.IsValid() ? SelectedCandidate->ActorPath : FString();
	const int32 PreviousVisibleIndex = FilteredCandidates.IndexOfByKey(SelectedCandidate);
	TSet<FString> PreviousListSelectedActorPaths;
	if (CandidateListView.IsValid())
	{
		for (const FCandidatePtr& PreviouslySelected : CandidateListView->GetSelectedItems())
		{
			if (PreviouslySelected.IsValid() && !PreviouslySelected->ActorPath.IsEmpty())
			{
				PreviousListSelectedActorPaths.Add(PreviouslySelected->ActorPath);
			}
		}
	}
	FilteredCandidates.Reset();
	TSet<FString> EligibleBatchActorPaths;

	if (Controller.IsValid())
	{
		for (const FCandidatePtr& Candidate : Controller->GetCandidates())
		{
			if (Candidate.IsValid() && !Candidate->bApplied
				&& !Candidate->bDismissedFromReview && !Candidate->ActorPath.IsEmpty())
			{
				EligibleBatchActorPaths.Add(Candidate->ActorPath);
			}
			if (Candidate.IsValid() && !Candidate->bDismissedFromReview
				&& CandidateMatchesSearch(*Candidate))
			{
				FilteredCandidates.Add(Candidate);
			}
		}
	}
	for (auto It = BatchSelectedActorPaths.CreateIterator(); It; ++It)
	{
		if (!EligibleBatchActorPaths.Contains(*It))
		{
			It.RemoveCurrent();
		}
	}
	FilteredCandidates.StableSort([](const FCandidatePtr& A, const FCandidatePtr& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid();
		}
		if (A->bTagComplete != B->bTagComplete)
		{
			return A->bTagComplete;
		}
		if (A->bTagComplete && !FMath::IsNearlyEqual(A->Confidence, B->Confidence))
		{
			return A->Confidence > B->Confidence;
		}
		if (!FMath::IsNearlyEqual(A->SignificanceScore, B->SignificanceScore))
		{
			return A->SignificanceScore > B->SignificanceScore;
		}
		return A->ActorLabel < B->ActorLabel;
	});

	if (!SelectedCandidate.IsValid() || !FilteredCandidates.Contains(SelectedCandidate))
	{
		SelectedCandidate.Reset();
		if (!PreviousActorPath.IsEmpty())
		{
			if (const FCandidatePtr* FoundCandidate = FilteredCandidates.FindByPredicate([&PreviousActorPath](const FCandidatePtr& Candidate)
			{
				return Candidate.IsValid() && Candidate->ActorPath == PreviousActorPath;
			}))
			{
				SelectedCandidate = *FoundCandidate;
			}
		}
		if (!SelectedCandidate.IsValid() && !FilteredCandidates.IsEmpty())
		{
			const int32 FallbackIndex = PreviousVisibleIndex == INDEX_NONE
				? 0
				: FMath::Clamp(PreviousVisibleIndex, 0, FilteredCandidates.Num() - 1);
			SelectedCandidate = FilteredCandidates[FallbackIndex];
		}
	}
	if (!FilteredCandidates.Contains(CandidateSelectionAnchor))
	{
		CandidateSelectionAnchor.Reset();
	}

	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
		TArray<FCandidatePtr> RestoredListSelection;
		for (const FCandidatePtr& Candidate : FilteredCandidates)
		{
			if (Candidate.IsValid() && PreviousListSelectedActorPaths.Contains(Candidate->ActorPath))
			{
				RestoredListSelection.Add(Candidate);
			}
		}
		TGuardValue<bool> SuppressSelectionChanged(bSuppressCandidateSelectionChanged, true);
		if (!RestoredListSelection.IsEmpty())
		{
			CandidateListView->ClearSelection();
			CandidateListView->SetItemSelection(
				RestoredListSelection,
				true,
				ESelectInfo::Direct);
		}
		else if (SelectedCandidate.IsValid())
		{
			CandidateListView->SetSelection(SelectedCandidate, ESelectInfo::Direct);
		}
		else
		{
			CandidateListView->ClearSelection();
		}
	}
	RefreshCompletionSummaryLists();
	RefreshSelectedPreview();
}

void SConvaiSceneAutoTagger::RefreshCompletionSummaryLists()
{
	AttentionCandidates = Controller.IsValid()
		? Controller->GetAttentionCandidates()
		: TArray<FCandidatePtr>();
	AttentionCandidates.StableSort([](const FCandidatePtr& A, const FCandidatePtr& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid();
		}
		return A->ActorLabel < B->ActorLabel;
	});
	LowInformationCandidates = Controller.IsValid()
		? Controller->GetLowInformationFilteredCandidates()
		: TArray<FCandidatePtr>();
	LowInformationCandidates.StableSort([](const FCandidatePtr& A, const FCandidatePtr& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid();
		}
		return A->ActorLabel < B->ActorLabel;
	});

	TSet<FString> AvailableActorPaths;
	for (const FCandidatePtr& Candidate : LowInformationCandidates)
	{
		if (Candidate.IsValid() && Candidate->Actor.IsValid() && !Candidate->ActorPath.IsEmpty())
		{
			AvailableActorPaths.Add(Candidate->ActorPath);
		}
	}
	for (auto It = LowInformationSelectedActorPaths.CreateIterator(); It; ++It)
	{
		if (!AvailableActorPaths.Contains(*It))
		{
			It.RemoveCurrent();
		}
	}
	if (LowInformationListView.IsValid())
	{
		LowInformationListView->RequestListRefresh();
	}
	if (AttentionListView.IsValid())
	{
		AttentionListView->RequestListRefresh();
	}
}

void SConvaiSceneAutoTagger::RefreshSelectedPreview()
{
	if (PreviewBrush.IsValid()
		&& PreviewSourceCandidate.Pin() == SelectedCandidate
		&& SelectedCandidate.IsValid()
		&& PreviewSourceSize == SelectedCandidate->PreviewSize
		&& PreviewSourceByteCount == SelectedCandidate->PreviewBGRA.Num()
		&& PreviewSourceRevision == SelectedCandidate->PreviewRevision)
	{
		return;
	}

	const uint64 ResourceGeneration = ++PreviewResourceGeneration;
	bPreviewResourceReady = false;
	UpdatePreviewImageBrush();
	ReleaseEvidenceBrush(PreviewBrush);
	PreviewSourceCandidate.Reset();
	PreviewSourceSize = FIntPoint::ZeroValue;
	PreviewSourceByteCount = 0;
	PreviewSourceRevision = 0;
	if (!SelectedCandidate.IsValid()
		|| SelectedCandidate->PreviewSize.X <= 0
		|| SelectedCandidate->PreviewSize.Y <= 0)
	{
		return;
	}

	const int64 ExpectedBytes = static_cast<int64>(SelectedCandidate->PreviewSize.X)
		* SelectedCandidate->PreviewSize.Y * sizeof(FColor);
	if (SelectedCandidate->PreviewBGRA.Num() != ExpectedBytes)
	{
		return;
	}

	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(SelectedCandidate->PreviewBGRA.Num() / sizeof(FColor));
	FMemory::Memcpy(Pixels.GetData(), SelectedCandidate->PreviewBGRA.GetData(), ExpectedBytes);
	PreviewBrush = CreateEvidenceBrush(Pixels, SelectedCandidate->PreviewSize.X);
	if (!PreviewBrush.IsValid())
	{
		return;
	}
	PreviewSourceCandidate = SelectedCandidate;
	PreviewSourceSize = SelectedCandidate->PreviewSize;
	PreviewSourceByteCount = SelectedCandidate->PreviewBGRA.Num();
	PreviewSourceRevision = SelectedCandidate->PreviewRevision;
	WaitForPreviewResource(ResourceGeneration);
}

void SConvaiSceneAutoTagger::UpdatePreviewImageBrush()
{
	const FSlateBrush* Image = !bEditingView && bPreviewResourceReady && PreviewBrush.IsValid()
		? PreviewBrush.Get()
		: nullptr;
	const bool bHasLiveImage = bEditingView
		&& bEditablePreviewResourceReady
		&& LivePreviewBrush.IsValid();
	if (PreviewImageWidget.IsValid())
	{
		// Visibility is set explicitly with the resource. A bound visibility can be
		// evaluated before the render-fence callback and leave the leaf collapsed
		// even after its brush becomes ready in a retained Slate window.
		PreviewImageWidget->SetImage(Image);
		PreviewImageWidget->SetVisibility(Image ? EVisibility::Visible : EVisibility::Collapsed);
		PreviewImageWidget->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
	}
	if (LivePreviewImageWidget.IsValid())
	{
		LivePreviewImageWidget->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
	}
	if (PreviewPlaceholderWidget.IsValid())
	{
		PreviewPlaceholderWidget->SetVisibility(Image || bHasLiveImage
			? EVisibility::Collapsed
			: EVisibility::Visible);
		PreviewPlaceholderWidget->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
	}
	Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
}

void SConvaiSceneAutoTagger::WaitForPreviewResource(const uint64 ResourceGeneration)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Scene Auto Tagger preview fences must begin on the game thread.")))
	{
		return;
	}
	// Slate's dynamic BGRA upload is asynchronous. Do not block or recursively
	// flush rendering from a Slate callback; publish the brush on a later game
	// tick after its queued render command has crossed this fence.
	const TSharedRef<FRenderCommandFence> ResourceFence = MakeShared<FRenderCommandFence>();
	ResourceFence->BeginFence();
	const TWeakPtr<SConvaiSceneAutoTagger> WeakSelfForCallback = SharedThis(this);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(
			[WeakSelfForCallback, ResourceFence, ResourceGeneration](float)
			{
				const TSharedPtr<SConvaiSceneAutoTagger> Pinned = WeakSelfForCallback.Pin();
				if (!Pinned.IsValid())
				{
					return false;
				}
				if (Pinned->PreviewResourceGeneration != ResourceGeneration)
				{
					return false;
				}
				if (!ResourceFence->IsFenceComplete())
				{
					return true;
				}
				if (Pinned->PreviewResourceGeneration != ResourceGeneration
					|| !Pinned->PreviewBrush.IsValid())
				{
					return false;
				}
				Pinned->bPreviewResourceReady = true;
				const FSlateDynamicImageBrush* ReadyBrush = Pinned->PreviewBrush.Get();
				const FSlateResourceHandle ResourceHandle = ReadyBrush
					? FSlateApplication::Get().GetRenderer()->GetResourceHandle(*ReadyBrush)
					: FSlateResourceHandle();
				UE_LOG(
					LogConvaiSceneAutoTaggerUi,
					Verbose,
					TEXT("Evidence preview ready: resource=%s size=%.0fx%.0f handle=%s"),
					ReadyBrush ? *ReadyBrush->GetResourceName().ToString() : TEXT("none"),
					ReadyBrush ? ReadyBrush->ImageSize.X : 0.0f,
					ReadyBrush ? ReadyBrush->ImageSize.Y : 0.0f,
					ResourceHandle.IsValid() ? TEXT("valid") : TEXT("invalid"));
				Pinned->UpdatePreviewImageBrush();
				return false;
			}),
		0.01f);
}

bool SConvaiSceneAutoTagger::CandidateMatchesSearch(const FSceneAutoTaggerCandidate& Candidate) const
{
	if (SearchText.IsEmpty())
	{
		return true;
	}

	FString Searchable = FString::Printf(
		TEXT("%s %s %s %s %s %s %s %s %s %s"),
		*Candidate.ActorLabel,
		*Candidate.ActorPath,
		*Candidate.AssetSummary,
		*Candidate.SuggestedName,
		*Candidate.Description,
		*Candidate.RefinementNote,
		*Candidate.SignificanceReason,
		*Candidate.Error,
		LexToString(Candidate.Source),
		*DecisionText(Candidate.Decision).ToString());
	return Searchable.Contains(SearchText, ESearchCase::IgnoreCase);
}

FReply SConvaiSceneAutoTagger::HandleExplore()
{
	LocalError.Reset();
	if (!Controller.IsValid())
	{
		LocalError = TEXT("The scene exploration controller is unavailable.");
		return FReply::Handled();
	}
	if (!Controller->IsNativeCoreReady())
	{
		LocalError = Controller->GetNativeCoreDiagnostic();
		return FReply::Handled();
	}
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		LocalError = TEXT("Open an editable level before exploring the scene.");
		return FReply::Handled();
	}
	if (!IsAgentReady())
	{
		LocalError = AgentStatusDetail.ToString();
		return FReply::Handled();
	}
	if (!bPrivacyAcknowledged)
	{
		LocalError = TEXT("Allow AI scene analysis before starting.");
		return FReply::Handled();
	}
	FSceneAutoTaggerRunOptions Options;
	const FSceneAutoTaggerCharacterOption* SelectedCharacter = CharacterSetupModel
		? CharacterSetupModel->GetSelectedCharacter() : nullptr;
	if (!SelectedCharacter)
	{
		LocalError = TEXT("Choose a Convai character before exploring the scene.");
		return FReply::Handled();
	}
	Options.VisionCharacterID = SelectedCharacter->ID;
	Options.VisionCharacterName = SelectedCharacter->Name;
	if (ExplorationScope == EExplorationScope::SelectedActors)
	{
		Options.ScopedActors = GetSelectedEditorActors();
		if (Options.ScopedActors.IsEmpty())
		{
			LocalError = TEXT("Select at least one actor in the current level, or switch the scope to Current Level.");
			return FReply::Handled();
		}
		Options.bForceAnalyzeScopedActors = true;
	}

	if (!Controller->GetCandidates().IsEmpty()
		&& FMessageDialog::Open(
			EAppMsgType::YesNo,
			LOCTEXT(
				"ConfirmNewExploration",
				"The current review, edits, decisions, and checked rows will be replaced. Applied Convai objects stay in the scene."),
			LOCTEXT("ConfirmNewExplorationTitle", "Replace current review?")) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}
	SyncSceneContextEditorToCurrentMap();
	if (GetCurrentMapPackageName().StartsWith(TEXT("/Game/"))
		&& SessionSceneContextWorld.Get() == World
		&& (!SessionSceneContext.IsEmpty() || !SessionDescriptionFocus.IsEmpty()))
	{
		// Preserve guidance entered before an unsaved map was named via Save As.
		SetSceneContextForCurrentMap(SessionSceneContext);
		SetDescriptionFocusForCurrentMap(SessionDescriptionFocus);
	}
	// Exploring is the single commit point for both prominent optional guidance
	// fields, so entry never dead-ends on a separate Save action.
	SceneContextDraft = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SceneContextDraft);
	DescriptionFocusDraft = ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(
		DescriptionFocusDraft);
	SetSceneContextForCurrentMap(SceneContextDraft);
	SetDescriptionFocusForCurrentMap(DescriptionFocusDraft);
	bSceneContextDraftDirty = false;
	bDescriptionFocusDraftDirty = false;
	Options.SceneContext = SceneContextDraft;
	Options.DescriptionFocus = DescriptionFocusDraft;
	Options.SignificanceThreshold = SignificanceThreshold;
	Options.bIncludeAlreadyTaggedActors = bIncludeAlreadyTaggedActors;
	if (const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>())
	{
		// Freeze the profile at the Explore boundary; later settings edits cannot
		// mix packing, model policy, or cache identities inside this review.
		Options.AnalysisDetail = Settings->AnalysisDetail;
		Options.AnalysisGridDimension = Settings->GetEffectiveGridDimension();
		Options.AnalysisCellResolution = Settings->GetEffectiveCellResolution();
		Options.EvidencePreviewResolution = Settings->GetEffectiveEvidencePreviewResolution();
	}

	// End the channel-isolated live preview only after validation and confirmation;
	// both capture paths intentionally use the same private view-light channel.
	EndEditView();
	CommitPendingCandidateEdit();
	BatchSelectedActorPaths.Reset();
	Controller->StartExploration(World, Options);
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::RefreshCharacterCatalog()
{
	if (!CharacterCatalog || !CharacterSetupModel)
	{
		return;
	}
	const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	if (!FSceneAutoTaggerSetupModel::HasUsableCredentials(
		AuthHeaderAndKey.Key,
		AuthHeaderAndKey.Value))
	{
		CharacterCatalog->Cancel();
		bCharacterCatalogLoading = false;
		CharacterCatalogError.Reset();
		CharacterSetupModel->SetCharacters({});
		RefreshAgentStatus();
		RefreshCharacterGrid();
		return;
	}
	bCharacterCatalogLoading = true;
	CharacterCatalogError.Reset();
	RefreshCharacterGrid();
	const TWeakPtr<SConvaiSceneAutoTagger> WeakSelfForCallback = SharedThis(this);
	CharacterCatalog->Refresh([WeakSelfForCallback](bool bSuccess, TArray<FSceneAutoTaggerCharacterOption> Characters, FString Error)
	{
		const TSharedPtr<SConvaiSceneAutoTagger> Pinned = WeakSelfForCallback.Pin();
		if (!Pinned.IsValid() || !Pinned->CharacterSetupModel)
		{
			return;
		}
		Pinned->bCharacterCatalogLoading = false;
		if (!bSuccess)
		{
			Pinned->CharacterCatalogError = MoveTemp(Error);
			Pinned->RefreshAgentStatus();
			Pinned->RefreshCharacterGrid();
			return;
		}
		Pinned->CharacterCatalogError.Reset();
		Pinned->CharacterSetupModel->SetCharacters(MoveTemp(Characters));
		if (UConvaiSceneAutoTaggerUserState* UserState = GetMutableDefault<UConvaiSceneAutoTaggerUserState>())
		{
			const FString SavedID = UserState->GetLastSelectedVisionCharacterID();
			if (!SavedID.IsEmpty() && !Pinned->CharacterSetupModel->SelectCharacter(SavedID))
			{
				UserState->SetLastSelectedVisionCharacterID(FString());
			}
		}
		Pinned->RefreshAgentStatus();
		Pinned->RefreshCharacterGrid();
	});
}

void SConvaiSceneAutoTagger::RefreshCharacterGrid()
{
	if (CharacterGridContainer.IsValid())
	{
		const float PreviousScrollOffset = CharacterScrollBox.IsValid()
			? CharacterScrollBox->GetScrollOffset()
			: 0.0f;
		CharacterGridContainer->SetContent(BuildCharacterGrid());
		if (CharacterScrollBox.IsValid())
		{
			CharacterScrollBox->SetScrollOffset(PreviousScrollOffset);
		}
	}
}

void SConvaiSceneAutoTagger::HandleCharacterSearchChanged(const FText& NewText)
{
	if (CharacterSetupModel)
	{
		CharacterSetupModel->SetSearchText(NewText.ToString());
		RefreshCharacterGrid();
		if (CharacterScrollBox.IsValid())
		{
			CharacterScrollBox->ScrollToStart();
		}
	}
}

void SConvaiSceneAutoTagger::HandleCharacterSelected(FString CharacterID)
{
	if (CharacterSetupModel && CharacterSetupModel->GetSelectedCharacterID() == CharacterID.TrimStartAndEnd())
	{
		return;
	}
	if (!CharacterSetupModel || !CharacterSetupModel->SelectCharacter(CharacterID))
	{
		return;
	}
	// A draft started for another character must never overwrite the user's text.
	++SceneContextDraftGeneration;
	bSceneContextDraftBusy = false;
	SceneContextDraftViewportPng.Reset();
	SceneContextDraftFeedback.Reset();
	SceneContextDraftError.Reset();
	DraftRequestCharacterID.Reset();
	if (UConvaiSceneAutoTaggerUserState* UserState = GetMutableDefault<UConvaiSceneAutoTaggerUserState>())
	{
		UserState->SetLastSelectedVisionCharacterID(CharacterID);
	}
	RefreshAgentStatus();
	RefreshCharacterGrid();
}

FReply SConvaiSceneAutoTagger::HandleRefreshCharacters()
{
	RefreshCharacterCatalog();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleLoadMoreCharacters()
{
	if (CharacterSetupModel)
	{
		CharacterSetupModel->LoadMore();
		RefreshCharacterGrid();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleOpenCharacterInConvai()
{
	if (CharacterSetupModel && CharacterSetupModel->GetSelectedCharacter())
	{
		const FString CharacterID = CharacterSetupModel->GetSelectedCharacterID();
		const FString CharacterUrl = FString::Printf(
			TEXT("https://convai.com/dashboard/character/%s"),
			*FGenericPlatformHttp::UrlEncode(CharacterID));
		FPlatformProcess::LaunchURL(*CharacterUrl, nullptr, nullptr);
	}
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::HandleSceneContextMenuOpenChanged(bool bIsOpen)
{
	if (bIsOpen)
	{
		if (Controller.IsValid() && !Controller->GetCandidates().IsEmpty())
		{
			SceneContextDraftFeedback.Reset();
			SceneContextDraftError.Reset();
			return;
		}
		SyncSceneContextEditorToCurrentMap();
		if (GetCurrentMapPackageName().StartsWith(TEXT("/Game/"))
			&& SessionSceneContextWorld.Get() == GetEditorWorld()
			&& !SessionSceneContext.IsEmpty())
		{
			SetSceneContextForCurrentMap(SessionSceneContext);
		}
		SceneContextDraftFeedback.Reset();
		SceneContextDraftError.Reset();
		return;
	}

	// Scene context is also visible in the persistent pre-Explore card. Closing
	// the optional header popover must not cancel a viewport draft that remains
	// visible and explicitly cancellable there.
}

bool SConvaiSceneAutoTagger::SyncSceneContextEditorToCurrentMap()
{
	UWorld* CurrentWorld = GetEditorWorld();
	const FString CurrentMapKey = GetCurrentMapPackageName();
	if (SceneContextEditorWorld.Get() == CurrentWorld && SceneContextEditorMapKey == CurrentMapKey)
	{
		return false;
	}
	const bool bSameWorldSaveAs = CurrentWorld
		&& SceneContextEditorWorld.Get() == CurrentWorld
		&& SceneContextEditorMapKey != CurrentMapKey
		&& (bSceneContextDraftDirty || bDescriptionFocusDraftDirty);
	const FString ContextToCarryForward = bSameWorldSaveAs ? SceneContextDraft : FString();
	const FString FocusToCarryForward = bSameWorldSaveAs ? DescriptionFocusDraft : FString();

	++SceneContextDraftGeneration;
	bSceneContextDraftBusy = false;
	SceneContextDraftViewportPng.Reset();
	SceneContextDraftWorld.Reset();
	SceneContextDraftMapKey.Reset();
	SceneContextEditorWorld = CurrentWorld;
	SceneContextEditorMapKey = CurrentMapKey;
	SceneContextDraft = bSameWorldSaveAs ? ContextToCarryForward : GetSceneContextForCurrentMap();
	DescriptionFocusDraft = bSameWorldSaveAs
		? FocusToCarryForward
		: GetDescriptionFocusForCurrentMap();
	if (bSameWorldSaveAs)
	{
		SceneContextDraft = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SceneContextDraft);
		DescriptionFocusDraft = ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(
			DescriptionFocusDraft);
		SetSceneContextForCurrentMap(SceneContextDraft);
		SetDescriptionFocusForCurrentMap(DescriptionFocusDraft);
	}
	bSceneContextDraftDirty = false;
	bDescriptionFocusDraftDirty = false;
	SceneContextDraftFeedback = bSameWorldSaveAs
		? TEXT("Guidance carried forward to the renamed map.")
		: FString();
	SceneContextDraftError.Reset();
	return true;
}

void SConvaiSceneAutoTagger::HandleSceneContextDraftChanged(const FText& NewText)
{
	if (SyncSceneContextEditorToCurrentMap())
	{
		return;
	}
	SceneContextDraft = NewText.ToString().Left(
		ConvaiSceneAutoTagger::VisionProtocol::GetMaximumSceneContextLength());
	bSceneContextDraftDirty = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SceneContextDraft)
		!= GetSceneContextForCurrentMap();
	SceneContextDraftFeedback.Reset();
	SceneContextDraftError.Reset();
}

void SConvaiSceneAutoTagger::HandleDescriptionFocusDraftChanged(const FText& NewText)
{
	if (SyncSceneContextEditorToCurrentMap())
	{
		return;
	}
	DescriptionFocusDraft = NewText.ToString().Left(
		ConvaiSceneAutoTagger::VisionProtocol::GetMaximumDescriptionFocusLength());
	bDescriptionFocusDraftDirty =
		ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(DescriptionFocusDraft)
		!= GetDescriptionFocusForCurrentMap();
}

FReply SConvaiSceneAutoTagger::HandleDraftSceneContextFromViewport()
{
	SyncSceneContextEditorToCurrentMap();
	if (!CanDraftSceneContext())
	{
		SceneContextDraftError = IsAgentReady()
			? TEXT("Choose Back to setup before changing scene context.")
			: TEXT("Configure Convai Vision before drafting scene context.");
		return FReply::Handled();
	}

	TArray64<uint8> ViewportPng;
	FString CaptureError;
	if (!ConvaiSceneAutoTagger::CaptureActiveLevelViewportPng(ViewportPng, CaptureError))
	{
		SceneContextDraftError = MoveTemp(CaptureError);
		SceneContextDraftFeedback.Reset();
		return FReply::Handled();
	}

	SceneContextDraftViewportPng = MoveTemp(ViewportPng);
	SceneContextDraftWorld = GetEditorWorld();
	SceneContextDraftMapKey = GetCurrentMapPackageName();
	bSceneContextDraftBusy = true;
	SceneContextDraftError.Reset();
	SceneContextDraftFeedback = TEXT("Drafting scene context from the current view...");
	const uint64 RequestGeneration = ++SceneContextDraftGeneration;
	SubmitSceneContextDraftRequest(RequestGeneration);
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::SubmitSceneContextDraftRequest(const uint64 RequestGeneration)
{
	if (!bSceneContextDraftBusy || RequestGeneration != SceneContextDraftGeneration
		|| SceneContextDraftViewportPng.IsEmpty())
	{
		return;
	}

	const FString CharacterID = CharacterSetupModel
		? CharacterSetupModel->GetSelectedCharacterID().TrimStartAndEnd()
		: FString();
	const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	if (CharacterID.IsEmpty()
		|| AuthHeaderAndKey.Key.TrimStartAndEnd().IsEmpty()
		|| AuthHeaderAndKey.Value.TrimStartAndEnd().IsEmpty())
	{
		bSceneContextDraftBusy = false;
		SceneContextDraftError = TEXT("Configure Convai credentials and a Vision character before drafting scene context.");
		SceneContextDraftFeedback.Reset();
		SceneContextDraftViewportPng.Reset();
		return;
	}
	DraftRequestCharacterID = CharacterID;

	TArray<uint8> ViewportPng;
	ViewportPng.Append(
		SceneContextDraftViewportPng.GetData(),
		static_cast<int32>(SceneContextDraftViewportPng.Num()));
	FConvaiTaggingPipelineParams Params;
	Params.CharacterID = CharacterID;
	Params.MaxParallelRequests = 1;
	Params.BaseUrlOverride = Convai::Get().GetConvaiSettings()->CustomProdURL.TrimStartAndEnd();
	const TWeakPtr<SConvaiSceneAutoTagger> WeakSelfForCallback = SharedThis(this);
	FConvaiVisionService::RequestSceneContextDescription(
		AuthHeaderAndKey,
		MoveTemp(ViewportPng),
		TEXT("scene-context-draft"),
		Params,
		[WeakSelfForCallback, RequestGeneration, CharacterID](const FConvaiVisionService::FResult& Result)
		{
			const TSharedPtr<SConvaiSceneAutoTagger> Pinned = WeakSelfForCallback.Pin();
			if (!Pinned.IsValid() || RequestGeneration != Pinned->SceneContextDraftGeneration
				|| !Pinned->CharacterSetupModel
				|| Pinned->CharacterSetupModel->GetSelectedCharacterID() != CharacterID
				|| Pinned->DraftRequestCharacterID != CharacterID)
			{
				return;
			}
			if (Pinned->GetEditorWorld() != Pinned->SceneContextDraftWorld.Get()
				|| Pinned->GetCurrentMapPackageName() != Pinned->SceneContextDraftMapKey)
			{
				Pinned->bSceneContextDraftBusy = false;
				Pinned->SceneContextDraftFeedback.Reset();
				Pinned->SceneContextDraftError = TEXT("The active level changed before the draft completed. Try again in the current level.");
				Pinned->SceneContextDraftViewportPng.Reset();
				return;
			}
			Pinned->bSceneContextDraftBusy = false;
			Pinned->SceneContextDraftViewportPng.Reset();
			if (Result.bSuccess && Result.Objects.Num() == 1)
			{
				const FConvaiVisionService::FObjectResult& Object = Result.Objects[0];
				Pinned->SceneContextDraft = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(
					Object.Description.IsEmpty() ? Object.Name : Object.Description);
				Pinned->bSceneContextDraftDirty =
					ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(Pinned->SceneContextDraft)
					!= Pinned->GetSceneContextForCurrentMap();
				Pinned->SceneContextDraftError.Reset();
				Pinned->SceneContextDraftFeedback = TEXT("Draft ready - review it before saving.");
				return;
			}
			Pinned->SceneContextDraftFeedback.Reset();
			Pinned->SceneContextDraftError = Result.Error.IsEmpty()
				? TEXT("Couldn't create a scene-context draft. Check the active viewport and Convai connection, then try again.")
				: FString::Printf(TEXT("Couldn't create a scene-context draft. %s"), *Result.Error);
		});
}

FReply SConvaiSceneAutoTagger::HandleCancelSceneContextDraft()
{
	++SceneContextDraftGeneration;
	bSceneContextDraftBusy = false;
	SceneContextDraftViewportPng.Reset();
	SceneContextDraftWorld.Reset();
	SceneContextDraftMapKey.Reset();
	SceneContextDraftFeedback.Reset();
	SceneContextDraftError.Reset();
	DraftRequestCharacterID.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleConfigureAgent()
{
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer(
		"Project", "Plugins", "Convai");
	RefreshAgentStatus();
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleRefreshAgentStatus()
{
	const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	if (FSceneAutoTaggerSetupModel::HasUsableCredentials(
		AuthHeaderAndKey.Key,
		AuthHeaderAndKey.Value))
	{
		RefreshCharacterCatalog();
	}
	else
	{
		RefreshAgentStatus();
	}
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleAcceptPrivacy()
{
	SetPrivacyAcknowledged(true);
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::SetPrivacyAcknowledged(const bool bAccepted)
{
	bPrivacyAcknowledged = bAccepted;
	if (UConvaiSceneAutoTaggerUserState* UserState = GetMutableDefault<UConvaiSceneAutoTaggerUserState>())
	{
		UserState->SetPrivacyConsentAccepted(PrivacyConsentKey, bAccepted);
	}
	LocalError.Reset();
}

FReply SConvaiSceneAutoTagger::HandleDeclinePrivacy()
{
	// Declining closes only this nomad tab. No capture or vision request has started.
	if (const TSharedPtr<SDockTab> LiveTab = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(FName(TEXT("ConvaiSceneAutoTagger")))))
	{
		LiveTab->RequestCloseTab();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleReopenCompletionSummary()
{
	if (Controller.IsValid())
	{
		Controller->ReopenCompletionSummary();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleContinueFromCompletionSummary()
{
	LowInformationSelectedActorPaths.Reset();
	if (Controller.IsValid())
	{
		Controller->AcknowledgeCompletionSummary();
	}
	if (LowInformationListView.IsValid())
	{
		LowInformationListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleAnalyzeLowInformationSelection()
{
	if (!CanAnalyzeLowInformationSelection())
	{
		return FReply::Handled();
	}

	FString AnalysisError;
	if (!Controller->AnalyzeLowInformationCandidatesByActorPath(
		LowInformationSelectedActorPaths,
		AnalysisError))
	{
		LocalError = AnalysisError.IsEmpty()
			? TEXT("The checked objects could not be restored for analysis.")
			: MoveTemp(AnalysisError);
		return FReply::Handled();
	}

	LowInformationSelectedActorPaths.Reset();
	LocalError.Reset();
	Controller->AcknowledgeCompletionSummary();
	if (LowInformationListView.IsValid())
	{
		LowInformationListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleFocusLowInformationCandidate(FCandidatePtr Candidate)
{
	if (!Candidate.IsValid() || !Candidate->Actor.IsValid() || !GEditor)
	{
		LocalError = TEXT("The skipped actor no longer exists in the current editor world.");
		return FReply::Handled();
	}

	AActor* Actor = Candidate->Actor.Get();
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, true, true);
	GEditor->MoveViewportCamerasToActor(*Actor, true);
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleInspectAttentionCandidate(FCandidatePtr Candidate)
{
	if (!Candidate.IsValid() || !Controller.IsValid())
	{
		return FReply::Handled();
	}

	EndEditView();
	CommitPendingCandidateEdit();
	if (!FilteredCandidates.Contains(Candidate) && CandidateSearchBox.IsValid())
	{
		CandidateSearchBox->SetText(FText::GetEmpty());
	}
	Controller->AcknowledgeCompletionSummary();
	SelectedCandidate = Candidate;
	RefreshSelectedPreview();
	if (CandidateListView.IsValid())
	{
		TGuardValue<bool> SuppressSelectionChanged(bSuppressCandidateSelectionChanged, true);
		CandidateListView->SetSelection(Candidate, ESelectInfo::Direct);
		CandidateListView->RequestScrollIntoView(Candidate);
	}
	LocalError.Reset();
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::HandleLowInformationSelectionChanged(
	FCandidatePtr Candidate,
	const ECheckBoxState NewState)
{
	if (!Candidate.IsValid() || Candidate->ActorPath.IsEmpty())
	{
		return;
	}
	if (NewState == ECheckBoxState::Checked)
	{
		LowInformationSelectedActorPaths.Add(Candidate->ActorPath);
	}
	else
	{
		LowInformationSelectedActorPaths.Remove(Candidate->ActorPath);
	}
}

FReply SConvaiSceneAutoTagger::HandleCancel()
{
	if (Controller.IsValid())
	{
		Controller->Cancel();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleReturnToSetup()
{
	if (!CanClearResults())
	{
		return FReply::Handled();
	}
	if (FMessageDialog::Open(
			EAppMsgType::YesNo,
			LOCTEXT(
				"ConfirmReturnToSetup",
				"The current review, edits, decisions, and checked rows will be discarded. Applied Convai objects stay in the scene."),
			LOCTEXT("ConfirmReturnToSetupTitle", "Return to setup?")) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}
	if (SceneContextComboButton.IsValid())
	{
		SceneContextComboButton->SetIsOpen(false);
	}
	return HandleClearResults();
}

FReply SConvaiSceneAutoTagger::HandleClearResults()
{
	EndEditView();
	CommitPendingCandidateEdit();
	if (Controller.IsValid())
	{
		Controller->ClearResults();
	}
	SelectedCandidate.Reset();
	CandidateSelectionAnchor.Reset();
	FilteredCandidates.Reset();
	BatchSelectedActorPaths.Reset();
	AttentionCandidates.Reset();
	LowInformationCandidates.Reset();
	LowInformationSelectedActorPaths.Reset();
	LocalError.Reset();
	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
	}
	if (LowInformationListView.IsValid())
	{
		LowInformationListView->RequestListRefresh();
	}
	if (AttentionListView.IsValid())
	{
		AttentionListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleManageAppliedObjects()
{
	FModuleManager::LoadModuleChecked<FConvaiSceneTaggingEditorModule>(
		"ConvaiSceneTaggingEditor").OpenSceneObjectsManager();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleSelectActor()
{
	if (!SelectedCandidate.IsValid() || !SelectedCandidate->Actor.IsValid() || !GEditor)
	{
		LocalError = TEXT("The reviewed actor no longer exists in the current editor world.");
		return FReply::Handled();
	}
	AActor* Actor = SelectedCandidate->Actor.Get();
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, true, true);
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleFocusActor()
{
	if (!SelectedCandidate.IsValid() || !SelectedCandidate->Actor.IsValid() || !GEditor)
	{
		LocalError = TEXT("The reviewed actor no longer exists in the current editor world.");
		return FReply::Handled();
	}
	AActor* Actor = SelectedCandidate->Actor.Get();
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, true, true);
	GEditor->MoveViewportCamerasToActor(*Actor, true);
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleBeginEditView()
{
	if (!SelectedCandidate.IsValid() || !SelectedCandidate->Actor.IsValid()
		|| !Controller.IsValid() || Controller->IsBusy())
	{
		LocalError = TEXT("Select an available object before editing its capture view.");
		return FReply::Handled();
	}
	EditSourceCandidate = SelectedCandidate;
	bEditingView = true;
	bEditableCaptureMatchesCamera = false;
	bEditablePreviewResourceReady = false;
	EditViewDistanceScale = SelectedCandidate->EvidenceDistanceScale;
	EditViewTargetOffset = SelectedCandidate->EvidenceTargetOffset;
	const FVector SavedViewDirection = SelectedCandidate->Actor->GetActorQuat().RotateVector(
		SelectedCandidate->EvidenceViewDirectionActorLocal).GetSafeNormal();
	FVector CameraOffset = -SavedViewDirection;
	if (CameraOffset.IsNearlyZero())
	{
		CameraOffset = (SelectedCandidate->Actor->GetActorForwardVector()
			+ FVector::UpVector * 0.15f).GetSafeNormal();
	}
	const FRotator Orbit = CameraOffset.Rotation();
	EditViewYaw = Orbit.Yaw;
	EditViewPitch = FMath::Clamp(Orbit.Pitch, -80.0f, 80.0f);
	if (!StartLiveEditableCapture())
	{
		EndEditView();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleCancelEditView()
{
	EndEditView();
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleResetEditView()
{
	if (!bEditingView || !SelectedCandidate.IsValid())
	{
		return FReply::Handled();
	}
	const FVector SavedViewDirection = SelectedCandidate->Actor.IsValid()
		? SelectedCandidate->Actor->GetActorQuat().RotateVector(
			SelectedCandidate->EvidenceViewDirectionActorLocal).GetSafeNormal()
		: FVector::ZeroVector;
	FVector CameraOffset = -SavedViewDirection;
	if (CameraOffset.IsNearlyZero() && SelectedCandidate->Actor.IsValid())
	{
		CameraOffset = (SelectedCandidate->Actor->GetActorForwardVector()
			+ FVector::UpVector * 0.15f).GetSafeNormal();
	}
	const FRotator Orbit = CameraOffset.Rotation();
	EditViewYaw = Orbit.Yaw;
	EditViewPitch = FMath::Clamp(Orbit.Pitch, -80.0f, 80.0f);
	EditViewDistanceScale = SelectedCandidate->EvidenceDistanceScale;
	EditViewTargetOffset = SelectedCandidate->EvidenceTargetOffset;
	UpdateLiveEditableCapture();
	return FReply::Handled();
}

bool SConvaiSceneAutoTagger::CommitEditedView(FCandidatePtr& OutCandidate)
{
	OutCandidate.Reset();
	if (!bEditingView || !bEditableCaptureMatchesCamera || !LiveCaptureSession
		|| !Controller.IsValid() || !SelectedCandidate.IsValid())
	{
		return false;
	}
	FString CaptureError;
	TArray<FColor> CapturedPixels;
	if (!LiveCaptureSession->ReadCurrentPixels(CapturedPixels, CaptureError))
	{
		LocalError = FString::Printf(
			TEXT("The adjusted view could not be read: %s"),
			CaptureError.IsEmpty() ? TEXT("empty capture") : *CaptureError);
		bEditableCaptureMatchesCamera = false;
		return false;
	}
	EditableCaptureResolution = LiveCaptureSession->GetResolution();
	EditableCapturePixels = MoveTemp(CapturedPixels);
	const FVector ViewDirection = -FRotator(EditViewPitch, EditViewYaw, 0.0f).Vector();
	const FCandidatePtr CandidateToAnalyze = SelectedCandidate;
	FString EvidenceError;
	if (!Controller->ReplaceCandidateEvidence(
		CandidateToAnalyze,
		EditableCapturePixels,
		EditableCaptureResolution,
		ViewDirection,
		EditViewDistanceScale,
		EditViewTargetOffset,
		EvidenceError))
	{
		LocalError = EvidenceError;
		return false;
	}
	OutCandidate = CandidateToAnalyze;
	EndEditView();
	PreviewSourceCandidate.Reset();
	RefreshSelectedPreview();
	return true;
}

FReply SConvaiSceneAutoTagger::HandleUseEditedView()
{
	FCandidatePtr SavedCandidate;
	if (!CommitEditedView(SavedCandidate))
	{
		return FReply::Handled();
	}
	if (!SavedCandidate->ActorPath.IsEmpty())
	{
		BatchSelectedActorPaths.Add(SavedCandidate->ActorPath);
	}
	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
	}
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleUseAndAnalyzeEditedView()
{
	FCandidatePtr CandidateToAnalyze;
	if (!CommitEditedView(CandidateToAnalyze))
	{
		return FReply::Handled();
	}
	FString AnalysisError;
	if (!Controller->RedescribeCandidate(CandidateToAnalyze, AnalysisError))
	{
		// The exact adjusted image is already retained locally, so a setup or
		// launch failure can be retried without returning to Edit View.
		LocalError = AnalysisError;
	}
	else
	{
		LocalError.Reset();
	}
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::HandleEditViewOrbit(const FVector2D DragDelta)
{
	if (!bEditingView)
	{
		return;
	}
	EditViewYaw = FRotator::NormalizeAxis(EditViewYaw + DragDelta.X * 0.35f);
	EditViewPitch = FMath::Clamp(EditViewPitch + DragDelta.Y * 0.25f, -80.0f, 80.0f);
	UpdateLiveEditableCapture();
}

void SConvaiSceneAutoTagger::HandleEditViewPan(const FVector2D DragDelta)
{
	if (!bEditingView)
	{
		return;
	}
	EditViewTargetOffset.X = FMath::Clamp(EditViewTargetOffset.X - DragDelta.X * 0.0025f, -0.75f, 0.75f);
	EditViewTargetOffset.Y = FMath::Clamp(EditViewTargetOffset.Y + DragDelta.Y * 0.0025f, -0.75f, 0.75f);
	UpdateLiveEditableCapture();
}

void SConvaiSceneAutoTagger::HandleEditViewZoom(const float WheelDelta)
{
	if (!bEditingView || FMath::IsNearlyZero(WheelDelta))
	{
		return;
	}
	EditViewDistanceScale = FMath::Clamp(
		EditViewDistanceScale * FMath::Pow(0.88f, WheelDelta),
		0.55f,
		2.5f);
	UpdateLiveEditableCapture();
}

bool SConvaiSceneAutoTagger::StartLiveEditableCapture()
{
	if (!bEditingView || !SelectedCandidate.IsValid() || !SelectedCandidate->Actor.IsValid()
		|| EditSourceCandidate.Pin() != SelectedCandidate)
	{
		return false;
	}

	// Drop Slate's reference before destroying the render-target producer.
	LivePreviewBrush.Reset();
	if (LiveCaptureSession)
	{
		LiveCaptureSession->End();
		LiveCaptureSession.Reset();
	}
	bEditablePreviewResourceReady = false;
	bEditableCaptureMatchesCamera = false;
	EditableCapturePixels.Reset();
	EditableCaptureResolution = 0;

	TArray<UPrimitiveComponent*> Components;
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent :
		SelectedCandidate->PrimitiveComponents)
	{
		if (UPrimitiveComponent* Component = WeakComponent.Get())
		{
			Components.Add(Component);
		}
	}
	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	const int32 CaptureResolution = Controller.IsValid()
		? Controller->GetEditableCaptureResolution()
		: FMath::Max(
			Settings->GetEffectiveEvidencePreviewResolution(),
			Settings->GetEffectiveCellResolution());
	const FVector ViewDirection = -FRotator(EditViewPitch, EditViewYaw, 0.0f).Vector();
	FString CaptureError;
	LiveCaptureSession = ConvaiSceneAutoTagger::BeginLiveObjectCapture(
		SelectedCandidate->Actor->GetWorld(),
		Components,
		SelectedCandidate->WorldBounds,
		CaptureResolution,
		CaptureError,
		ViewDirection,
		EditViewDistanceScale,
		EditViewTargetOffset,
		ConvaiSceneAutoTagger::ShouldSuppressDirectSpecularHighlights(
			SelectedCandidate->CaptureLightingPolicy),
		ConvaiSceneAutoTagger::ShouldAllowDirectSpecularRecovery(
			SelectedCandidate->CaptureLightingPolicy),
		false,
		true);
	if (!LiveCaptureSession || !LiveCaptureSession->IsValid()
		|| !LiveCaptureSession->GetRenderTarget())
	{
		LiveCaptureSession.Reset();
		LocalError = FString::Printf(
			TEXT("The live adjusted view could not start: %s"),
			CaptureError.IsEmpty() ? TEXT("empty capture") : *CaptureError);
		return false;
	}

	LivePreviewBrush = FDeferredCleanupSlateBrush::CreateBrush(
		LiveCaptureSession->GetRenderTarget(),
		FVector2D(CaptureResolution, CaptureResolution));
	bEditablePreviewResourceReady = LivePreviewBrush.IsValid();
	bEditableCaptureMatchesCamera = bEditablePreviewResourceReady;
	bEditViewReadabilityCalibrated = false;
	EditViewWarmupFrames = 0;
	EditViewLastObservedFrame = GFrameCounter;
	EditViewWarmupStartedAtSeconds = FPlatformTime::Seconds();
	UpdatePreviewImageBrush();
	LocalError.Reset();
	return bEditablePreviewResourceReady;
}

bool SConvaiSceneAutoTagger::UpdateLiveEditableCapture()
{
	bEditableCaptureMatchesCamera = false;
	if (!bEditingView || !LiveCaptureSession || !LiveCaptureSession->IsValid())
	{
		return false;
	}
	const FVector ViewDirection = -FRotator(EditViewPitch, EditViewYaw, 0.0f).Vector();
	FString CaptureError;
	if (!LiveCaptureSession->UpdateView(
		ViewDirection,
		EditViewDistanceScale,
		EditViewTargetOffset,
		CaptureError))
	{
		LocalError = FString::Printf(
			TEXT("The live adjusted view could not update: %s"),
			CaptureError.IsEmpty() ? TEXT("capture session unavailable") : *CaptureError);
		return false;
	}
	bEditableCaptureMatchesCamera = true;
	bEditablePreviewResourceReady = LivePreviewBrush.IsValid();
	if (LivePreviewImageWidget.IsValid())
	{
		LivePreviewImageWidget->Invalidate(EInvalidateWidgetReason::Paint);
	}
	LocalError.Reset();
	return true;
}

void SConvaiSceneAutoTagger::EndEditView()
{
	bEditingView = false;
	bEditableCaptureMatchesCamera = false;
	bEditablePreviewResourceReady = false;
	bEditViewReadabilityCalibrated = false;
	EditViewWarmupFrames = 0;
	EditViewLastObservedFrame = 0;
	EditViewWarmupStartedAtSeconds = 0.0;
	EditSourceCandidate.Reset();
	UpdatePreviewImageBrush();
	LivePreviewBrush.Reset();
	if (LiveCaptureSession)
	{
		LiveCaptureSession->End();
		LiveCaptureSession.Reset();
	}
	EditableCapturePixels.Reset();
	EditableCaptureResolution = 0;
	EditViewDistanceScale = 1.0f;
	EditViewTargetOffset = FVector2D::ZeroVector;
}

void SConvaiSceneAutoTagger::HandleCandidateBatchSelectionChanged(
	FCandidatePtr Candidate,
	const ECheckBoxState NewState)
{
	if (bEditingView || !Controller.IsValid() || Controller->IsBusy()
		|| !CanBatchSelectCandidate(Candidate) || Candidate->ActorPath.IsEmpty())
	{
		return;
	}
	if (NewState == ECheckBoxState::Checked)
	{
		BatchSelectedActorPaths.Add(Candidate->ActorPath);
	}
	else
	{
		BatchSelectedActorPaths.Remove(Candidate->ActorPath);
	}
	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
	}
}

void SConvaiSceneAutoTagger::HandleVisibleBatchSelectionChanged(const ECheckBoxState NewState)
{
	if (bEditingView || !Controller.IsValid() || Controller->IsBusy())
	{
		return;
	}

	// Slate reports an Undetermined checkbox activation as Unchecked. Treat a
	// mixed visible set like the standard table gesture: first click selects
	// every shown row, and a subsequent click clears them.
	TSet<FString> VisibleEligiblePaths;
	for (const FCandidatePtr& Candidate : FilteredCandidates)
	{
		if (!CanBatchSelectCandidate(Candidate) || Candidate->ActorPath.IsEmpty())
		{
			continue;
		}
		VisibleEligiblePaths.Add(Candidate->ActorPath);
	}
	ApplyVisibleBatchSelection(
		GetVisibleBatchSelectionState(),
		NewState,
		VisibleEligiblePaths,
		BatchSelectedActorPaths);
	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
	}
}

bool SConvaiSceneAutoTagger::ShouldCheckAllVisibleRows(
	const ECheckBoxState CurrentState,
	const ECheckBoxState RequestedState)
{
	return CurrentState == ECheckBoxState::Undetermined
		|| RequestedState == ECheckBoxState::Checked;
}

void SConvaiSceneAutoTagger::ApplyVisibleBatchSelection(
	const ECheckBoxState CurrentState,
	const ECheckBoxState RequestedState,
	const TSet<FString>& VisibleEligiblePaths,
	TSet<FString>& InOutSelectedPaths)
{
	const bool bShouldCheck = ShouldCheckAllVisibleRows(CurrentState, RequestedState);
	for (const FString& ActorPath : VisibleEligiblePaths)
	{
		if (bShouldCheck)
		{
			InOutSelectedPaths.Add(ActorPath);
		}
		else
		{
			InOutSelectedPaths.Remove(ActorPath);
		}
	}
}

TSet<FString> SConvaiSceneAutoTagger::ResolveContextActionPaths(
	const TArray<TSharedPtr<FSceneAutoTaggerCandidate>>& HighlightedCandidates,
	const TSharedPtr<FSceneAutoTaggerCandidate>& PressedCandidate)
{
	TSet<FString> Result;
	if (!PressedCandidate.IsValid() || PressedCandidate->ActorPath.IsEmpty())
	{
		return Result;
	}

	const bool bPressedAlreadyHighlighted = HighlightedCandidates.ContainsByPredicate(
		[&PressedCandidate](const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate)
		{
			return Candidate == PressedCandidate;
		});
	if (!bPressedAlreadyHighlighted)
	{
		Result.Add(PressedCandidate->ActorPath);
		return Result;
	}

	for (const TSharedPtr<FSceneAutoTaggerCandidate>& Candidate : HighlightedCandidates)
	{
		if (Candidate.IsValid() && !Candidate->ActorPath.IsEmpty())
		{
			Result.Add(Candidate->ActorPath);
		}
	}
	return Result;
}

FReply SConvaiSceneAutoTagger::HandleBatchDecision(const ESceneAutoTaggerDecision Decision)
{
	return HandleDecisionForActorPaths(BatchSelectedActorPaths, Decision, true);
}

FReply SConvaiSceneAutoTagger::HandleDecisionForActorPaths(
	const TSet<FString>& ActorPaths,
	const ESceneAutoTaggerDecision Decision,
	const bool bClearMatchingChecks)
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before changing review decisions.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (!Controller.IsValid() || Controller->IsBusy() || ActorPaths.IsEmpty())
	{
		LocalError = TEXT("Select one or more eligible review rows first.");
		return FReply::Handled();
	}

	Controller->SetDecisionsByActorPath(ActorPaths, Decision);
	if (bClearMatchingChecks)
	{
		for (const FCandidatePtr& Candidate : Controller->GetCandidates())
		{
			if (Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
				&& Candidate->Decision == Decision)
			{
				BatchSelectedActorPaths.Remove(Candidate->ActorPath);
			}
		}
	}
	LocalError.Reset();
	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleAnalyzeBatchSelection()
{
	return HandleAnalyzeActorPaths(BatchSelectedActorPaths);
}

FReply SConvaiSceneAutoTagger::HandleAnalyzeActorPaths(const TSet<FString>& ActorPaths)
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before analyzing review objects.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (!Controller.IsValid() || Controller->IsBusy() || ActorPaths.IsEmpty())
	{
		LocalError = TEXT("Choose one or more objects with saved images before analyzing them.");
		return FReply::Handled();
	}
	FString AnalysisError;
	if (!Controller->AnalyzeCandidatesByActorPath(ActorPaths, AnalysisError))
	{
		LocalError = AnalysisError;
	}
	else
	{
		// Keep the checked set so successful rows can be included together next.
		LocalError.Reset();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleRecaptureAndAnalyzeBatchSelection()
{
	return HandleRecaptureAndAnalyzeActorPaths(BatchSelectedActorPaths);
}

FReply SConvaiSceneAutoTagger::HandleRecaptureAndAnalyzeActorPaths(
	const TSet<FString>& ActorPaths)
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before recapturing review objects.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (!Controller.IsValid() || Controller->IsBusy() || ActorPaths.IsEmpty())
	{
		LocalError = TEXT("Choose one or more available objects before recapturing them.");
		return FReply::Handled();
	}
	FString AnalysisError;
	if (!Controller->RecaptureAndAnalyzeCandidatesByActorPath(
		ActorPaths,
		AnalysisError))
	{
		LocalError = AnalysisError;
	}
	else
	{
		// Keep the checked set so successful rows can be included together next.
		LocalError.Reset();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleClearBatchSelection()
{
	BatchSelectedActorPaths.Reset();
	if (CandidateListView.IsValid())
	{
		CandidateListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleRemoveCandidatesFromReview(
	const TSet<FString>& ActorPaths)
{
	if (bEditingView || !Controller.IsValid() || Controller->IsBusy()
		|| GetRemovableCandidateCountForPaths(ActorPaths) <= 0)
	{
		LocalError = TEXT("Only unapplied review rows can be removed while no analysis is running.");
		return FReply::Handled();
	}

	CommitPendingCandidateEdit();
	const int32 RemovedCount = Controller->DismissCandidatesFromReviewByActorPath(ActorPaths);
	if (RemovedCount <= 0)
	{
		LocalError = TEXT("The selected rows could not be removed while another operation is running.");
		return FReply::Handled();
	}
	for (const FString& ActorPath : ActorPaths)
	{
		BatchSelectedActorPaths.Remove(ActorPath);
	}
	PendingMouseSelectionCandidate.Reset();
	PendingContextMenuCandidate.Reset();
	LocalError.Reset();
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleAddSelectedActorsToReview()
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before adding Level actors.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (!Controller.IsValid() || Controller->IsBusy())
	{
		LocalError = TEXT("Wait for the current Scene Auto Tagger operation to finish.");
		return FReply::Handled();
	}

	FSceneAutoTaggerAddToReviewResult Result;
	FString AddError;
	if (!Controller->AddActorsToCurrentReview(GetSelectedEditorActors(), Result, AddError))
	{
		LocalError = AddError;
		return FReply::Handled();
	}

	LocalError.Reset();
	FNotificationInfo Notification(FText::Format(
		LOCTEXT(
			"ActorsAddedToReviewNotification",
			"Added {0} · Restored {1} · Skipped {2}. Analyzing selected actors…"),
		FText::AsNumber(Result.Added),
		FText::AsNumber(Result.Restored),
		FText::AsNumber(Result.Skipped)));
	Notification.ExpireDuration = 4.0f;
	Notification.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Notification);
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleAcceptAtOrAbove()
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before changing review decisions.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (Controller.IsValid())
	{
		Controller->AcceptAtOrAbove(BulkConfidenceThreshold);
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleResetAll()
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before changing review decisions.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (Controller.IsValid())
	{
		Controller->SetAllDecisions(ESceneAutoTaggerDecision::Pending);
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleApplyAccepted()
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before applying scene changes.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	LocalError.Reset();
	if (Controller.IsValid())
	{
		Controller->ApplyAccepted();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleAnalyzeWithRefinementNote()
{
	if (bEditingView)
	{
		LocalError = TEXT("Finish or cancel Edit View before refining this suggestion.");
		return FReply::Handled();
	}
	CommitPendingCandidateEdit();
	if (!Controller.IsValid() || !SelectedCandidate.IsValid())
	{
		LocalError = TEXT("Select an object with a saved image before refining it.");
		return FReply::Handled();
	}
	FString RefinementError;
	const TSet<FString> ActorPaths = { SelectedCandidate->ActorPath };
	if (!Controller->AnalyzeCandidatesByActorPath(ActorPaths, RefinementError))
	{
		LocalError = MoveTemp(RefinementError);
	}
	else
	{
		LocalError.Reset();
	}
	return FReply::Handled();
}

FReply SConvaiSceneAutoTagger::HandleClearRefinementNote()
{
	if (CanEditRefinementNote() && !SelectedCandidate->RefinementNote.IsEmpty())
	{
		SelectedCandidate->RefinementNote.Reset();
		++SelectedCandidate->RefinementNoteRevision;
		LocalError.Reset();
		if (CandidateListView.IsValid())
		{
			CandidateListView->RequestListRefresh();
		}
	}
	return FReply::Handled();
}

void SConvaiSceneAutoTagger::HandleNameChanged(const FText& NewText)
{
	if (CanModifySelected())
	{
		SelectedCandidate->SuggestedName = NewText.ToString().Left(96);
		PendingEditedCandidate = SelectedCandidate;
	}
}

void SConvaiSceneAutoTagger::HandleNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	CommitPendingCandidateEdit();
}

void SConvaiSceneAutoTagger::HandleDescriptionChanged(const FText& NewText)
{
	if (CanModifySelected())
	{
		SelectedCandidate->Description = NewText.ToString().Left(320);
		PendingEditedCandidate = SelectedCandidate;
	}
}

void SConvaiSceneAutoTagger::HandleDescriptionCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	CommitPendingCandidateEdit();
}

void SConvaiSceneAutoTagger::HandleRefinementNoteChanged(const FText& NewText)
{
	if (!CanEditRefinementNote())
	{
		return;
	}
	FString BoundedNote = NewText.ToString();
	BoundedNote.LeftInline(
		ConvaiSceneAutoTagger::VisionProtocol::GetMaximumRefinementNoteLength(),
		EAllowShrinking::No);
	if (SelectedCandidate->RefinementNote != BoundedNote)
	{
		SelectedCandidate->RefinementNote = MoveTemp(BoundedNote);
		++SelectedCandidate->RefinementNoteRevision;
		if (CandidateListView.IsValid())
		{
			CandidateListView->RequestListRefresh();
		}
	}
}

void SConvaiSceneAutoTagger::CommitPendingCandidateEdit()
{
	if (Controller.IsValid() && PendingEditedCandidate.IsValid())
	{
		Controller->NotifyCandidateEdited(PendingEditedCandidate);
	}
	PendingEditedCandidate.Reset();
}

void SConvaiSceneAutoTagger::SetScope(ECheckBoxState /*NewState*/, EExplorationScope NewScope)
{
	// These checkboxes form a radio-style scope selector. Reassert the clicked
	// scope regardless of the transient state Slate reports so the active scope
	// can never be toggled into an indeterminate/no-selection state.
	ExplorationScope = NewScope;
	LocalError.Reset();
}

ECheckBoxState SConvaiSceneAutoTagger::GetScopeCheckState(EExplorationScope Scope) const
{
	return ExplorationScope == Scope ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

UWorld* SConvaiSceneAutoTagger::GetEditorWorld() const
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString SConvaiSceneAutoTagger::GetCurrentMapPackageName() const
{
	const UWorld* World = GetEditorWorld();
	return World && World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
}

FString SConvaiSceneAutoTagger::GetSceneContextForCurrentMap() const
{
	const FString MapPackageName = GetCurrentMapPackageName();
	if (MapPackageName.StartsWith(TEXT("/Game/")))
	{
		if (const UConvaiSceneAutoTaggerUserState* UserState = GetDefault<UConvaiSceneAutoTaggerUserState>())
		{
			const FString Persisted = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(
				UserState->GetSceneContextForMap(MapPackageName));
			if (!Persisted.IsEmpty())
			{
				return Persisted;
			}
		}
		return SessionSceneContextWorld.Get() == GetEditorWorld()
			? ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SessionSceneContext)
			: FString();
	}
	return MapPackageName == SessionSceneContextMapKey
		? ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SessionSceneContext)
		: FString();
}

void SConvaiSceneAutoTagger::SetSceneContextForCurrentMap(const FString& SceneContext)
{
	const FString MapPackageName = GetCurrentMapPackageName();
	const FString Normalized = ConvaiSceneAutoTagger::VisionProtocol::NormalizeSceneContext(SceneContext);
	if (MapPackageName.StartsWith(TEXT("/Game/")))
	{
		if (UConvaiSceneAutoTaggerUserState* UserState = GetMutableDefault<UConvaiSceneAutoTaggerUserState>())
		{
			UserState->SetSceneContextForMap(MapPackageName, Normalized);
		}
		if (SessionSceneContextWorld.Get() == GetEditorWorld())
		{
			SessionSceneContext.Reset();
			if (SessionDescriptionFocus.IsEmpty())
			{
				SessionSceneContextMapKey.Reset();
				SessionSceneContextWorld.Reset();
			}
		}
		return;
	}
	SessionSceneContextMapKey = MapPackageName;
	SessionSceneContext = Normalized;
	SessionSceneContextWorld = GetEditorWorld();
}

FString SConvaiSceneAutoTagger::GetDescriptionFocusForCurrentMap() const
{
	const FString MapPackageName = GetCurrentMapPackageName();
	if (MapPackageName.StartsWith(TEXT("/Game/")))
	{
		if (const UConvaiSceneAutoTaggerUserState* UserState =
			GetDefault<UConvaiSceneAutoTaggerUserState>())
		{
			const FString Persisted =
				ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(
					UserState->GetDescriptionFocusForMap(MapPackageName));
			if (!Persisted.IsEmpty())
			{
				return Persisted;
			}
		}
		return SessionSceneContextWorld.Get() == GetEditorWorld()
			? ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(
				SessionDescriptionFocus)
			: FString();
	}
	return MapPackageName == SessionSceneContextMapKey
		? ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(
			SessionDescriptionFocus)
		: FString();
}

void SConvaiSceneAutoTagger::SetDescriptionFocusForCurrentMap(
	const FString& DescriptionFocus)
{
	const FString MapPackageName = GetCurrentMapPackageName();
	const FString Normalized =
		ConvaiSceneAutoTagger::VisionProtocol::NormalizeDescriptionFocus(DescriptionFocus);
	if (MapPackageName.StartsWith(TEXT("/Game/")))
	{
		if (UConvaiSceneAutoTaggerUserState* UserState =
			GetMutableDefault<UConvaiSceneAutoTaggerUserState>())
		{
			UserState->SetDescriptionFocusForMap(MapPackageName, Normalized);
		}
		if (SessionSceneContextWorld.Get() == GetEditorWorld())
		{
			SessionDescriptionFocus.Reset();
			if (SessionSceneContext.IsEmpty())
			{
				SessionSceneContextMapKey.Reset();
				SessionSceneContextWorld.Reset();
			}
		}
		return;
	}
	SessionSceneContextMapKey = MapPackageName;
	SessionDescriptionFocus = Normalized;
	SessionSceneContextWorld = GetEditorWorld();
}

int32 SConvaiSceneAutoTagger::GetSelectedActorCount() const
{
	return GetSelectedEditorActors().Num();
}

TArray<TWeakObjectPtr<AActor>> SConvaiSceneAutoTagger::GetSelectedEditorActors() const
{
	TArray<TWeakObjectPtr<AActor>> Actors;
	if (!GEditor || !GEditor->GetSelectedActors())
	{
		return Actors;
	}
	UWorld* World = GetEditorWorld();
	TSet<FString> SeenActorPaths;
	for (FSelectionIterator Iterator(*GEditor->GetSelectedActors()); Iterator; ++Iterator)
	{
		if (AActor* Actor = Cast<AActor>(*Iterator); Actor && Actor->GetWorld() == World)
		{
			const FString ActorPath = Actor->GetPathName();
			if (!ActorPath.IsEmpty() && !SeenActorPaths.Contains(ActorPath))
			{
				SeenActorPaths.Add(ActorPath);
				Actors.Add(Actor);
			}
		}
	}
	return Actors;
}

bool SConvaiSceneAutoTagger::IsAgentReady() const
{
	return bAgentReady;
}

bool SConvaiSceneAutoTagger::CanExplore() const
{
	if (!Controller.IsValid() || !Controller->IsNativeCoreReady()
		|| Controller->IsBusy() || bEditingView
		|| bSceneContextDraftBusy || !GetEditorWorld())
	{
		return false;
	}
	if (ExplorationScope == EExplorationScope::SelectedActors && GetSelectedActorCount() == 0)
	{
		return false;
	}
	return IsAgentReady() && bPrivacyAcknowledged;
}

bool SConvaiSceneAutoTagger::CanAnalyzeLowInformationSelection() const
{
	return !bEditingView && Controller.IsValid() && !Controller->IsBusy()
		&& LowInformationSelectedActorPaths.Num() > 0;
}

bool SConvaiSceneAutoTagger::CanEditSceneContext() const
{
	return !bSceneContextDraftBusy
		&& (!Controller.IsValid()
			|| (!Controller->IsBusy() && Controller->GetCandidates().IsEmpty()));
}

bool SConvaiSceneAutoTagger::CanDraftSceneContext() const
{
	return CanEditSceneContext()
		&& IsAgentReady()
		&& bPrivacyAcknowledged
		&& GetEditorWorld() != nullptr;
}

bool SConvaiSceneAutoTagger::CanClearResults() const
{
	return Controller.IsValid() && !Controller->IsBusy() && !bEditingView
		&& !Controller->GetCandidates().IsEmpty();
}

bool SConvaiSceneAutoTagger::CanModifySelected() const
{
	if (!Controller.IsValid() || !SelectedCandidate.IsValid() || !SelectedCandidate->bTagComplete
		|| SelectedCandidate->bUserCapturePendingAnalysis || SelectedCandidate->bApplied
		|| SelectedCandidate->bDismissedFromReview)
	{
		return false;
	}
	return Controller->GetProgress().State != ESceneAutoTaggerState::Applying;
}

bool SConvaiSceneAutoTagger::CanEditRefinementNote() const
{
	return !bEditingView && Controller.IsValid() && SelectedCandidate.IsValid()
		&& !Controller->IsBusy() && !SelectedCandidate->bApplied
		&& !SelectedCandidate->bDismissedFromReview;
}

bool SConvaiSceneAutoTagger::CanAnalyzeWithRefinementNote() const
{
	if (!CanEditRefinementNote() || !IsAgentReady() || !bPrivacyAcknowledged)
	{
		return false;
	}
	const int32 Resolution = SelectedCandidate->PreviewSize.X;
	return Resolution > 0 && SelectedCandidate->PreviewSize.Y == Resolution
		&& SelectedCandidate->PreviewBGRA.Num()
			== Resolution * Resolution * static_cast<int32>(sizeof(FColor))
		&& !ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			SelectedCandidate->RefinementNote).IsEmpty();
}

bool SConvaiSceneAutoTagger::CanRunBulkActions() const
{
	return !bEditingView && Controller.IsValid()
		&& Controller->HasReviewResults()
		&& Controller->GetProgress().State != ESceneAutoTaggerState::Applying;
}

bool SConvaiSceneAutoTagger::CanBatchSelectCandidate(const FCandidatePtr& Candidate) const
{
	return !bEditingView && Controller.IsValid() && Candidate.IsValid()
		&& !Candidate->bApplied && !Candidate->bDismissedFromReview
		&& !Candidate->ActorPath.IsEmpty();
}

int32 SConvaiSceneAutoTagger::GetIncludeEligibleCountForPaths(
	const TSet<FString>& ActorPaths) const
{
	if (!Controller.IsValid())
	{
		return 0;
	}
	int32 Count = 0;
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		Count += Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
			&& Controller->CanIncludeCandidate(Candidate) ? 1 : 0;
	}
	return Count;
}

int32 SConvaiSceneAutoTagger::GetClearDecisionEligibleCountForPaths(
	const TSet<FString>& ActorPaths) const
{
	if (!Controller.IsValid())
	{
		return 0;
	}
	int32 Count = 0;
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		Count += Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
			&& !Candidate->bApplied && !Candidate->bDismissedFromReview
			&& Candidate->Decision != ESceneAutoTaggerDecision::Pending ? 1 : 0;
	}
	return Count;
}

int32 SConvaiSceneAutoTagger::GetAnalyzeEligibleCountForPaths(
	const TSet<FString>& ActorPaths) const
{
	if (!Controller.IsValid())
	{
		return 0;
	}
	int32 Count = 0;
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		Count += Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
			&& !Candidate->bApplied && !Candidate->bDismissedFromReview
			&& Candidate->Actor.IsValid() && HasValidEvidence(Candidate.Get()) ? 1 : 0;
	}
	return Count;
}

TSet<FString> SConvaiSceneAutoTagger::GetRefinementNoteActorPaths(
	const TSet<FString>& ActorPaths) const
{
	TSet<FString> Result;
	if (!Controller.IsValid())
	{
		return Result;
	}
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		if (Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
			&& !Candidate->bApplied && !Candidate->bDismissedFromReview
			&& Candidate->Actor.IsValid() && HasValidEvidence(Candidate.Get())
			&& !ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
				Candidate->RefinementNote).IsEmpty())
		{
			Result.Add(Candidate->ActorPath);
		}
	}
	return Result;
}

int32 SConvaiSceneAutoTagger::GetRecaptureEligibleCountForPaths(
	const TSet<FString>& ActorPaths) const
{
	if (!Controller.IsValid())
	{
		return 0;
	}
	int32 Count = 0;
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		Count += Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
			&& !Candidate->bApplied && !Candidate->bDismissedFromReview
			&& Candidate->Actor.IsValid() ? 1 : 0;
	}
	return Count;
}

int32 SConvaiSceneAutoTagger::GetRemovableCandidateCountForPaths(
	const TSet<FString>& ActorPaths) const
{
	if (!Controller.IsValid())
	{
		return 0;
	}
	int32 Count = 0;
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		Count += Candidate.IsValid() && ActorPaths.Contains(Candidate->ActorPath)
			&& !Candidate->bApplied && !Candidate->bDismissedFromReview
			&& !Candidate->ActorPath.IsEmpty() ? 1 : 0;
	}
	return Count;
}

bool SConvaiSceneAutoTagger::CanRunBatchSelectionActions() const
{
	return !bEditingView && Controller.IsValid()
		&& !Controller->IsBusy() && !BatchSelectedActorPaths.IsEmpty();
}

bool SConvaiSceneAutoTagger::CanIncludeBatchSelection() const
{
	return !bEditingView && Controller.IsValid() && !Controller->IsBusy()
		&& GetIncludeEligibleCountForPaths(BatchSelectedActorPaths) > 0;
}

bool SConvaiSceneAutoTagger::CanClearBatchDecision() const
{
	if (bEditingView || !Controller.IsValid() || Controller->IsBusy())
	{
		return false;
	}
	return GetClearDecisionEligibleCountForPaths(BatchSelectedActorPaths) > 0;
}

int32 SConvaiSceneAutoTagger::GetBatchAnalyzeEligibleCount() const
{
	return GetAnalyzeEligibleCountForPaths(BatchSelectedActorPaths);
}

int32 SConvaiSceneAutoTagger::GetBatchRecaptureEligibleCount() const
{
	return GetRecaptureEligibleCountForPaths(BatchSelectedActorPaths);
}

bool SConvaiSceneAutoTagger::CanAnalyzeBatchSelection() const
{
	return !bEditingView && Controller.IsValid() && !Controller->IsBusy()
		&& GetBatchAnalyzeEligibleCount() > 0;
}

bool SConvaiSceneAutoTagger::CanRecaptureBatchSelection() const
{
	return !bEditingView && Controller.IsValid() && !Controller->IsBusy()
		&& GetBatchRecaptureEligibleCount() > 0;
}

ECheckBoxState SConvaiSceneAutoTagger::GetCandidateBatchSelectionState(const FCandidatePtr& Candidate) const
{
	return Candidate.IsValid() && BatchSelectedActorPaths.Contains(Candidate->ActorPath)
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

ECheckBoxState SConvaiSceneAutoTagger::GetVisibleBatchSelectionState() const
{
	int32 EligibleCount = 0;
	int32 SelectedCount = 0;
	for (const FCandidatePtr& Candidate : FilteredCandidates)
	{
		if (!CanBatchSelectCandidate(Candidate))
		{
			continue;
		}
		++EligibleCount;
		SelectedCount += BatchSelectedActorPaths.Contains(Candidate->ActorPath) ? 1 : 0;
	}
	if (EligibleCount == 0 || SelectedCount == 0)
	{
		return ECheckBoxState::Unchecked;
	}
	return SelectedCount == EligibleCount ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
}

FText SConvaiSceneAutoTagger::GetScopeDetailText() const
{
	const FText ScopeDetail = ExplorationScope == EExplorationScope::SelectedActors
		? FText::Format(LOCTEXT("SelectedCountFormat", "{0} selected"), FText::AsNumber(GetSelectedActorCount()))
		: LOCTEXT("CurrentLevelDetail", "all eligible actors");
	if (const UWorld* World = GetEditorWorld())
	{
		return FText::Format(
			LOCTEXT("ScopeAndMapFormat", "{0} in {1}"),
			ScopeDetail,
			FText::FromString(World->GetMapName()));
	}
	return ScopeDetail;
}

FText SConvaiSceneAutoTagger::GetSceneContextButtonText() const
{
	return HasGuidanceForCurrentReview()
		? LOCTEXT("GuidanceUsedButton", "Guidance used")
		: LOCTEXT("ImagesOnlyContextButton", "Images only");
}

FText SConvaiSceneAutoTagger::GetSceneContextDraftText() const
{
	if (Controller.IsValid() && !Controller->GetCandidates().IsEmpty())
	{
		return FText::FromString(Controller->GetRunSceneContext());
	}
	return FText::FromString(SceneContextDraft);
}

FText SConvaiSceneAutoTagger::GetDescriptionFocusDraftText() const
{
	if (Controller.IsValid() && !Controller->GetCandidates().IsEmpty())
	{
		return FText::FromString(Controller->GetRunDescriptionFocus());
	}
	return FText::FromString(DescriptionFocusDraft);
}

bool SConvaiSceneAutoTagger::HasSceneContextForCurrentReview() const
{
	return Controller.IsValid()
		&& !Controller->GetCandidates().IsEmpty()
		&& !Controller->GetRunSceneContext().IsEmpty();
}

bool SConvaiSceneAutoTagger::HasDescriptionFocusForCurrentReview() const
{
	return Controller.IsValid()
		&& !Controller->GetCandidates().IsEmpty()
		&& !Controller->GetRunDescriptionFocus().IsEmpty();
}

bool SConvaiSceneAutoTagger::HasGuidanceForCurrentReview() const
{
	return HasSceneContextForCurrentReview() || HasDescriptionFocusForCurrentReview();
}

FText SConvaiSceneAutoTagger::GetSceneContextCharacterCountText() const
{
	return FText::Format(
		LOCTEXT("SceneContextCountFormat", "{0}/{1}"),
		FText::AsNumber(SceneContextDraft.Len()),
		FText::AsNumber(ConvaiSceneAutoTagger::VisionProtocol::GetMaximumSceneContextLength()));
}

FText SConvaiSceneAutoTagger::GetSceneContextDraftFeedbackText() const
{
	return FText::FromString(SceneContextDraftFeedback);
}

FText SConvaiSceneAutoTagger::GetSceneContextDraftToolTipText() const
{
	if (!bPrivacyAcknowledged)
	{
		return LOCTEXT("DraftContextPrivacyTooltip", "Allow AI scene analysis first.");
	}
	if (!IsAgentReady())
	{
		return AgentStatusDetail;
	}
	if (!CanEditSceneContext())
	{
		return LOCTEXT("DraftContextLockedTooltip", "Choose Back to setup before changing scene context.");
	}
	const FText Provider = AgentDisplayName.IsEmpty()
		? LOCTEXT("SelectedAgentDraftTooltip", "Convai Vision")
		: AgentDisplayName;
	return FText::Format(
		LOCTEXT("DraftContextTooltip", "Send one image of the active Level Editor viewport to {0} and create an editable draft. For a cleaner result, use a Perspective/Lit view and hide unnecessary overlays."),
		Provider);
}

FText SConvaiSceneAutoTagger::GetDescriptionFocusCharacterCountText() const
{
	return FText::Format(
		LOCTEXT("DescriptionFocusCountFormat", "{0}/{1}"),
		FText::AsNumber(DescriptionFocusDraft.Len()),
		FText::AsNumber(
			ConvaiSceneAutoTagger::VisionProtocol::GetMaximumDescriptionFocusLength()));
}

FText SConvaiSceneAutoTagger::GetAnalysisQualitySummaryText() const
{
	const UConvaiSceneAutoTaggerSettings* Settings =
		GetDefault<UConvaiSceneAutoTaggerSettings>();
	if (Settings->AnalysisDetail == EConvaiSceneAutoTaggerAnalysisDetail::Detailed)
	{
		return LOCTEXT("AnalysisQualityAccurateSummary", "Better recognition; takes longer");
	}
	return LOCTEXT("AnalysisQualityFastSummary", "Quick general descriptions");
}

FText SConvaiSceneAutoTagger::GetReviewSummaryText() const
{
	if (!Controller.IsValid())
	{
		return FText::GetEmpty();
	}
	int32 CompleteCount = 0;
	int32 AttentionCount = 0;
	int32 NotAnalyzedCount = 0;
	for (const FCandidatePtr& Candidate : Controller->GetCandidates())
	{
		if (!Candidate.IsValid() || Candidate->bDismissedFromReview)
		{
			continue;
		}
		if (Candidate->Actor.IsValid()
			&& Candidate->bTagComplete
			&& Candidate->Error.IsEmpty()
			&& !Candidate->bUserCapturePendingAnalysis
			&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
				Candidate->RefinementNote).IsEmpty())
		{
			++CompleteCount;
		}
		else if (!Candidate->Error.IsEmpty() || !Candidate->Actor.IsValid())
		{
			++AttentionCount;
		}
		else
		{
			++NotAnalyzedCount;
		}
	}
	return FText::Format(
		LOCTEXT("ReviewSummaryFormat", "{0} complete  |  {1} not analyzed  |  {2} attention  |  {3} included"),
		FText::AsNumber(CompleteCount),
		FText::AsNumber(NotAnalyzedCount),
		FText::AsNumber(AttentionCount),
		FText::AsNumber(Controller->CountDecision(ESceneAutoTaggerDecision::Accepted)));
}

FText SConvaiSceneAutoTagger::GetAttentionChipText() const
{
	const int32 AttentionCount = Controller.IsValid()
		? Controller->CountAttentionCandidates()
		: 0;
	return FText::Format(
		LOCTEXT("AttentionCountFormat", "{0} need attention"),
		FText::AsNumber(AttentionCount));
}

FText SConvaiSceneAutoTagger::GetLowInformationChipText() const
{
	const int32 SkippedCount = Controller.IsValid()
		? Controller->CountLowInformationFilteredCandidates()
		: 0;
	return FText::Format(
		LOCTEXT("LowInformationCountFormat", "{0} skipped"),
		FText::AsNumber(SkippedCount));
}

FText SConvaiSceneAutoTagger::GetCompletionSummaryCountText() const
{
	int32 ReadyCount = 0;
	if (Controller.IsValid())
	{
		for (const FCandidatePtr& Candidate : Controller->GetCandidates())
		{
			ReadyCount += Candidate.IsValid() && !Candidate->bDismissedFromReview
				&& Candidate->Actor.IsValid() && Candidate->bTagComplete
				&& Candidate->Error.IsEmpty()
				&& !Candidate->bUserCapturePendingAnalysis
				&& ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
					Candidate->RefinementNote).IsEmpty()
				? 1
				: 0;
		}
	}
	const int32 AttentionCount = Controller.IsValid()
		? Controller->CountAttentionCandidates()
		: 0;
	const int32 SkippedCount = Controller.IsValid()
		? Controller->CountLowInformationFilteredCandidates()
		: 0;
	return FText::Format(
		LOCTEXT("CompletionSummaryCountFormat", "{0} ready  |  {1} need attention  |  {2} skipped"),
		FText::AsNumber(ReadyCount),
		FText::AsNumber(AttentionCount),
		FText::AsNumber(SkippedCount));
}

FText SConvaiSceneAutoTagger::GetCompletionSummaryBodyText() const
{
	const bool bHasAttention = Controller.IsValid()
		&& Controller->CountAttentionCandidates() > 0;
	const bool bHasSkipped = Controller.IsValid()
		&& Controller->CountLowInformationFilteredCandidates() > 0;
	if (bHasAttention && bHasSkipped)
	{
		return LOCTEXT(
			"CompletionSummaryMixedBody",
			"Most objects are ready for review. Items under Needs attention did not produce a reliable suggestion; inspect one to retry its saved image or recapture it. Skipped items had too little useful detail in their saved captures. No scene actors were changed.");
	}
	if (bHasAttention)
	{
		return LOCTEXT(
			"CompletionSummaryAttentionBody",
			"Most objects are ready for review. The items below did not produce a reliable suggestion. Inspect one to retry its saved image or recapture it. No scene actors were changed.");
	}
	return LOCTEXT(
		"CompletionSummarySkippedBody",
		"Most objects are ready for review. The items below were skipped because their saved captures had too little useful detail. Check any item you still want analyzed. No scene actors were changed.");
}

FText SConvaiSceneAutoTagger::GetAnalyzeLowInformationButtonText() const
{
	return FText::Format(
		LOCTEXT("AnalyzeLowInformationButtonFormat", "Analyze checked ({0})"),
		FText::AsNumber(LowInformationSelectedActorPaths.Num()));
}

FText SConvaiSceneAutoTagger::GetReadyAgentText() const
{
	const FText Provider = AgentDisplayName.IsEmpty()
		? LOCTEXT("GenericReadyAgent", "Convai Vision")
		: AgentDisplayName;
	return FText::Format(LOCTEXT("ReadyAgentFormat", "{0} ready"), Provider);
}

FText SConvaiSceneAutoTagger::GetEmptyStateTitleText() const
{
	if (ExplorationScope == EExplorationScope::SelectedActors && GetSelectedActorCount() == 0)
	{
		return LOCTEXT("EmptyStateSelectActorsTitle", "Select actors to explore");
	}
	if (Controller.IsValid()
		&& Controller->GetProgress().State == ESceneAutoTaggerState::ReviewReady
		&& Controller->GetCandidates().IsEmpty())
	{
		if (ExplorationScope == EExplorationScope::SelectedActors)
		{
			return LOCTEXT("EmptyStateNoSelectedMatchesTitle", "No selected actors could be scanned");
		}
		return LOCTEXT("EmptyStateNoMatchesTitle", "No eligible objects found");
	}
	return LOCTEXT("EmptyStateReadyTitle", "Explore scene objects");
}

FText SConvaiSceneAutoTagger::GetEmptyStateBodyText() const
{
	if (ExplorationScope == EExplorationScope::SelectedActors && GetSelectedActorCount() == 0)
	{
		return LOCTEXT(
			"EmptyStateSelectActorsBody",
			"Select one or more actors in the Level Editor, or switch the scope to Current Level.");
	}
	if (Controller.IsValid()
		&& Controller->GetProgress().State == ESceneAutoTaggerState::ReviewReady
		&& Controller->GetCandidates().IsEmpty())
	{
		if (ExplorationScope == EExplorationScope::SelectedActors)
		{
			return LOCTEXT(
				"EmptyStateNoSelectedMatchesBody",
				"The selection has no eligible visible mesh geometry, or project settings exclude it. Try another visible actor or include already tagged objects.");
		}
		return LOCTEXT(
			"EmptyStateNoMatchesBody",
			"Try lowering Minimum local relevance in Settings or including already tagged objects.");
	}
	return LOCTEXT(
		"EmptyStateReadyBody",
		"The selected scope controls which objects are analyzed. Scene context and description focus are optional.");
}

FText SConvaiSceneAutoTagger::GetAgentStatusText() const
{
	return AgentStatusDetail.IsEmpty()
		? LOCTEXT("AgentStatusUnknown", "Agent status unavailable")
		: AgentStatusDetail;
}

FSlateColor SConvaiSceneAutoTagger::GetAgentStatusColor() const
{
	return IsAgentReady()
		? FSlateColor(FLinearColor(0.22f, 0.78f, 0.42f))
		: FSlateColor(FLinearColor(0.95f, 0.60f, 0.20f));
}

FText SConvaiSceneAutoTagger::GetPrivacyDisclosureText() const
{
	return LOCTEXT(
		"PrivacyDisclosure",
		"The Auto Tagger sends captured images and optional guidance to the Convai character you select for this run.");
}

void SConvaiSceneAutoTagger::RefreshAgentStatus()
{
	const UConvaiSceneAutoTaggerSettings* Settings = GetDefault<UConvaiSceneAutoTaggerSettings>();
	const FSceneAutoTaggerCharacterOption* SelectedCharacter = CharacterSetupModel
		? CharacterSetupModel->GetSelectedCharacter()
		: nullptr;
	const bool bHasCharacter = SelectedCharacter != nullptr;
	const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
	const bool bHasCredentials = FSceneAutoTaggerSetupModel::HasUsableCredentials(
		AuthHeaderAndKey.Key,
		AuthHeaderAndKey.Value);
	const bool bCoreReady = FSceneAutoTaggerNativeCoreAdapter::Get().Initialize();
	bAgentReady = bHasCharacter && bHasCredentials && bCoreReady;
	AgentDisplayName = LOCTEXT("ConvaiVisionProvider", "Convai Vision");
	AgentAnalysisPolicyText = Settings->IsAccurateAnalysisEnabled()
		? LOCTEXT("AccurateAnalysisPolicy", "Convai Vision - Accurate image detail")
		: LOCTEXT("FastAnalysisPolicy", "Convai Vision - Fast image detail");
	if (!bHasCharacter)
	{
		AgentStatusDetail = LOCTEXT("VisionCharacterMissingDetail", "Choose a Convai character for this tagging run.");
	}
	else if (!bHasCredentials)
	{
		AgentStatusDetail = LOCTEXT("VisionApiKeyMissingDetail", "Set a Convai API key to enable scene analysis.");
	}
	else if (!bCoreReady)
	{
		AgentStatusDetail = FText::FromString(FSceneAutoTaggerNativeCoreAdapter::Get().GetDiagnostic());
	}
	else
	{
		AgentStatusDetail = FText::Format(
			LOCTEXT("VisionReadyDetail", "{0} is ready for scene analysis."),
			FText::FromString(SelectedCharacter->Name));
	}
}

FText SConvaiSceneAutoTagger::GetCharacterCountText() const
{
	if (!CharacterSetupModel || bCharacterCatalogLoading)
	{
		return FText::GetEmpty();
	}
	return FText::Format(
		LOCTEXT("CharacterCountFormat", "Showing {0} of {1} characters"),
		FText::AsNumber(CharacterSetupModel->GetVisibleCharacters().Num()),
		FText::AsNumber(CharacterSetupModel->GetFilteredCount()));
}

FText SConvaiSceneAutoTagger::GetCharacterCatalogFeedbackText() const
{
	if (bCharacterCatalogLoading)
	{
		return LOCTEXT("CharacterCatalogLoadingFeedback", "Loading characters...");
	}
	if (!CharacterCatalogError.IsEmpty())
	{
		return FText::FromString(CharacterCatalogError);
	}
	if (CharacterSetupModel && CharacterSetupModel->GetAllCharacters().IsEmpty())
	{
		return LOCTEXT("CharacterCatalogEmptyFeedback", "No characters are available for this account.");
	}
	return FText::GetEmpty();
}

FText SConvaiSceneAutoTagger::GetSelectedCharacterNameText() const
{
	return LOCTEXT("CharacterManagementTitle", "Knowledge Bank and character settings");
}

FText SConvaiSceneAutoTagger::GetSelectedCharacterDescriptionText() const
{
	const FSceneAutoTaggerCharacterOption* Character = CharacterSetupModel
		? CharacterSetupModel->GetSelectedCharacter()
		: nullptr;
	if (!Character)
	{
		return FText::GetEmpty();
	}
	return FText::Format(
		LOCTEXT("CharacterManagementDescription", "Manage {0} in Convai, then return here."),
		FText::FromString(Character->Name));
}
EVisibility SConvaiSceneAutoTagger::GetAgentRecoveryVisibility() const
{
	const bool bHasSelectedCharacter = CharacterSetupModel && CharacterSetupModel->GetSelectedCharacter();
	return !bHasSelectedCharacter || bAgentReady ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SConvaiSceneAutoTagger::GetBusyOverlayVisibility() const
{
	return Controller.IsValid() && Controller->IsBusy()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetCompletionSummaryVisibility() const
{
	return Controller.IsValid()
		&& Controller->ShouldShowCompletionSummary()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetAttentionChipVisibility() const
{
	return Controller.IsValid() && Controller->CountAttentionCandidates() > 0
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetLowInformationChipVisibility() const
{
	return Controller.IsValid()
		&& Controller->CountLowInformationFilteredCandidates() > 0
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetAttentionSummarySectionVisibility() const
{
	return AttentionCandidates.IsEmpty()
		? EVisibility::Collapsed
		: EVisibility::Visible;
}

EVisibility SConvaiSceneAutoTagger::GetLowInformationSummarySectionVisibility() const
{
	return LowInformationCandidates.IsEmpty()
		? EVisibility::Collapsed
		: EVisibility::Visible;
}

EVisibility SConvaiSceneAutoTagger::GetConsentOverlayVisibility() const
{
	return !bPrivacyAcknowledged
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetEditViewVisibility() const
{
	return bEditingView ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetStaticViewVisibility() const
{
	return !bEditingView && SelectedCandidate.IsValid()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SConvaiSceneAutoTagger::GetEvidenceCaptionText() const
{
	const bool bHasValidEvidence = HasValidEvidence(SelectedCandidate.Get());
	if (bEditingView)
	{
		return LOCTEXT("EditingEvidenceCaption", "Adjust this isolated view. Use view saves it for later batch analysis; Use & analyze saves and analyzes it now. The actor and level camera are not moved.");
	}
	if (SelectedCandidate.IsValid() && !SelectedCandidate->Error.IsEmpty() && bHasValidEvidence)
	{
		return LOCTEXT("RetryEvidenceCaption", "This image is still available. Check this row, then choose More > Analyze checked; use Edit View first if the image itself is wrong.");
	}
	if (SelectedCandidate.IsValid() && !SelectedCandidate->Error.IsEmpty() && !bHasValidEvidence)
	{
		return LOCTEXT("RecaptureEvidenceCaption", "No usable image was captured. Check this row, then choose More > Recapture & analyze checked.");
	}
	if (SelectedCandidate.IsValid() && SelectedCandidate->bUserCapturePendingAnalysis)
	{
		return LOCTEXT("PendingAdjustedEvidenceCaption", "Saved view needs analysis. The suggestion and score still belong to the previous image. Check this row, then choose More > Analyze checked.");
	}
	if (SelectedCandidate.IsValid() && SelectedCandidate->bAnalysisCancelled)
	{
		return bHasValidEvidence
			? LOCTEXT("CancelledEvidenceCaption", "Exploration stopped before this image was analyzed. Check this row, then choose More > Analyze checked.")
			: LOCTEXT("CancelledWithoutEvidenceCaption", "Exploration stopped before an image was captured. Check this row, then choose More > Recapture & analyze checked.");
	}
	if (SelectedCandidate.IsValid() && SelectedCandidate->bHasUserCapture)
	{
		return LOCTEXT("AdjustedEvidenceCaption", "This adjusted view was used for the object's most recent description.");
	}
	return LOCTEXT("EvidenceCaption", "Image used for this suggestion. It may differ from the Level Editor viewport.");
}

FText SConvaiSceneAutoTagger::GetPreviewPlaceholderText() const
{
	if ((bEditingView && !bEditablePreviewResourceReady
			&& (LiveCaptureSession || LivePreviewBrush.IsValid()))
		|| (!bEditingView && PreviewBrush.IsValid() && !bPreviewResourceReady))
	{
		return LOCTEXT("PreparingAnalysisImage", "Preparing analysis image...");
	}
	if (!SelectedCandidate.IsValid())
	{
		return LOCTEXT("SelectModelInputView", "Select an object to inspect the image used for analysis.");
	}
	if (SelectedCandidate->Source == ESceneAutoTaggerSource::GeometryCache
		|| (SelectedCandidate->Source == ESceneAutoTaggerSource::DuplicateGeometry && SelectedCandidate->bTagComplete))
	{
		return LOCTEXT("CachedModelInputView", "This suggestion was reused. The previous analysis image was not retained.");
	}
	if (SelectedCandidate->bAnalysisCancelled)
	{
		return LOCTEXT("CancelledModelInputView", "No image was retained before exploration stopped. Check this row, then choose More > Recapture & analyze checked.");
	}
	if (!SelectedCandidate->Error.IsEmpty() && !HasValidEvidence(SelectedCandidate.Get()))
	{
		return LOCTEXT("RecaptureModelInputView", "No usable analysis image was captured for this object.");
	}
	if (!SelectedCandidate->bTagComplete)
	{
		return LOCTEXT("PendingModelInputView", "The selected object's analysis image will appear here.");
	}
	return LOCTEXT("UnavailableModelInputView", "No analysis image is available for this suggestion.");
}

FText SConvaiSceneAutoTagger::GetExploreButtonText() const
{
	if (ExplorationScope == EExplorationScope::SelectedActors)
	{
		return LOCTEXT("ExploreSelectedActors", "Explore selected actors");
	}
	return LOCTEXT("ExploreCurrentLevel", "Explore current level");
}

FText SConvaiSceneAutoTagger::GetExploreToolTip() const
{
	if (!Controller.IsValid())
	{
		return LOCTEXT("ExploreNoControllerTooltip", "The exploration controller is unavailable.");
	}
	if (!Controller->IsNativeCoreReady())
	{
		return FText::FromString(Controller->GetNativeCoreDiagnostic());
	}
	if (Controller->IsBusy())
	{
		return LOCTEXT("ExploreBusyTooltip", "Cancel or wait for the current operation to finish.");
	}
	if (!GetEditorWorld())
	{
		return LOCTEXT("ExploreNoWorldTooltip", "Open an editable level first.");
	}
	if (ExplorationScope == EExplorationScope::SelectedActors && GetSelectedActorCount() == 0)
	{
		return LOCTEXT("ExploreNoSelectionTooltip", "Select one or more actors, or switch to Current Level.");
	}
	if (!IsAgentReady())
	{
		return AgentStatusDetail;
	}
	if (!bPrivacyAcknowledged)
	{
		return LOCTEXT("ExploreNoConsentTooltip", "Allow AI scene analysis before starting.");
	}
	return LOCTEXT("ExploreVisionTooltip", "Use the optional guidance, find relevant objects in the selected scope, and create editable suggestions with the configured Convai character. The scene is not modified until Apply.");
}

FText SConvaiSceneAutoTagger::GetPipelineStateText() const
{
	if (!Controller.IsValid())
	{
		return LOCTEXT("UnavailableState", "Unavailable");
	}
	const ESceneAutoTaggerState State = Controller->GetProgress().State;
	switch (State)
	{
	case ESceneAutoTaggerState::Discovering: return LOCTEXT("DiscoveringState", "Discovering");
	case ESceneAutoTaggerState::Capturing: return LOCTEXT("CapturingState", "Capturing");
	case ESceneAutoTaggerState::Tagging: return LOCTEXT("TaggingState", "Analyzing");
	case ESceneAutoTaggerState::ReviewReady: return LOCTEXT("ReviewReadyState", "Review ready");
	case ESceneAutoTaggerState::Applying: return LOCTEXT("ApplyingState", "Applying");
	case ESceneAutoTaggerState::Completed: return LOCTEXT("CompletedState", "Completed");
	case ESceneAutoTaggerState::Cancelled: return LOCTEXT("CancelledState", "Cancelled");
	case ESceneAutoTaggerState::Failed: return LOCTEXT("FailedState", "Needs attention");
	default: return LOCTEXT("IdleState", "Ready");
	}
}

FText SConvaiSceneAutoTagger::GetProgressMessageText() const
{
	if (!Controller.IsValid())
	{
		return LOCTEXT("ControllerUnavailable", "Controller unavailable");
	}
	if (!Controller->IsBusy())
	{
		if (!GetEditorWorld())
		{
			return LOCTEXT("ProgressNoWorld", "Open an editable level before exploring.");
		}
		if (ExplorationScope == EExplorationScope::SelectedActors && GetSelectedActorCount() == 0)
		{
			return LOCTEXT("ProgressNoSelection", "Selected Actors has no actors. Select one or more Level Editor actors, or switch to Current Level.");
		}
		if (!IsAgentReady())
		{
			return AgentStatusDetail;
		}
		if (!bPrivacyAcknowledged)
		{
			return LOCTEXT("ProgressNoPrivacy", "Allow AI scene analysis to enable Explore.");
		}
	}
	const FText Message = Controller->GetProgress().Message;
	return Message.IsEmpty() ? LOCTEXT("IdleMessage", "Configure a scope, review privacy, then explore.") : Message;
}

FText SConvaiSceneAutoTagger::GetProgressCountText() const
{
	if (!Controller.IsValid())
	{
		return FText::GetEmpty();
	}
	const FSceneAutoTaggerProgress& Progress = Controller->GetProgress();
	return Progress.Total > 0
		? FText::Format(LOCTEXT("ProgressCountFormat", "{0} of {1} complete"), FText::AsNumber(Progress.Completed), FText::AsNumber(Progress.Total))
		: FText::GetEmpty();
}

FText SConvaiSceneAutoTagger::GetVisibleCandidateCountText() const
{
	int32 Total = 0;
	if (Controller.IsValid())
	{
		for (const FCandidatePtr& Candidate : Controller->GetCandidates())
		{
			Total += Candidate.IsValid() && !Candidate->bDismissedFromReview ? 1 : 0;
		}
	}
	if (SearchText.IsEmpty())
	{
		return FText::Format(
			LOCTEXT("ResultCountFormat", "{0} results"),
			FText::AsNumber(Total));
	}
	return FText::Format(
		LOCTEXT("VisibleCountFormat", "{0} of {1}"),
		FText::AsNumber(FilteredCandidates.Num()),
		FText::AsNumber(Total));
}

FText SConvaiSceneAutoTagger::GetBatchSelectionCountText() const
{
	if (BatchSelectedActorPaths.IsEmpty())
	{
		return LOCTEXT("BatchSelectionEmpty", "0 checked");
	}
	int32 NeedsAnalysisCount = 0;
	const int32 RefinementNoteCount =
		GetRefinementNoteActorPaths(BatchSelectedActorPaths).Num();
	if (Controller.IsValid())
	{
		for (const FCandidatePtr& Candidate : Controller->GetCandidates())
		{
			NeedsAnalysisCount += Candidate.IsValid()
				&& Candidate->bUserCapturePendingAnalysis
				&& BatchSelectedActorPaths.Contains(Candidate->ActorPath)
				? 1
				: 0;
		}
	}
	if (RefinementNoteCount > 0 && NeedsAnalysisCount > 0)
	{
		return FText::Format(
			LOCTEXT(
				"BatchSelectionNotesAndAnalysisFormat",
				"{0} checked  |  {1} notes  |  {2} need analysis"),
			FText::AsNumber(BatchSelectedActorPaths.Num()),
			FText::AsNumber(RefinementNoteCount),
			FText::AsNumber(NeedsAnalysisCount));
	}
	if (RefinementNoteCount > 0)
	{
		return FText::Format(
			LOCTEXT("BatchSelectionNotesFormat", "{0} checked  |  {1} notes"),
			FText::AsNumber(BatchSelectedActorPaths.Num()),
			FText::AsNumber(RefinementNoteCount));
	}
	if (NeedsAnalysisCount > 0)
	{
		return FText::Format(
			LOCTEXT("BatchSelectionNeedsAnalysisFormat", "{0} checked · {1} need analysis"),
			FText::AsNumber(BatchSelectedActorPaths.Num()),
			FText::AsNumber(NeedsAnalysisCount));
	}
	return FText::Format(
		LOCTEXT("BatchSelectionCountFormat", "{0} checked"),
		FText::AsNumber(BatchSelectedActorPaths.Num()));
}

FText SConvaiSceneAutoTagger::GetEmptyResultsText() const
{
	if (!SearchText.IsEmpty() && Controller.IsValid() && !Controller->GetCandidates().IsEmpty())
	{
		return LOCTEXT("NoSearchMatches", "No suggestions match this search.\nYour edits and decisions are unchanged.");
	}
	if (ExplorationScope == EExplorationScope::SelectedActors
		&& Controller.IsValid()
		&& Controller->GetProgress().State == ESceneAutoTaggerState::ReviewReady)
	{
		return LOCTEXT(
			"NoSelectedCandidates",
			"No selected actor could be scanned. Choose a visible mesh actor or review project exclusions.");
	}
	if (Controller.IsValid() && Controller->CountLowInformationFilteredCandidates() > 0)
	{
		return FText::Format(
			LOCTEXT(
				"OnlySkippedCandidatesRemain",
				"No objects are in the main review. Use {0} skipped above to inspect or analyze the captures Auto Tagger set aside."),
			FText::AsNumber(Controller->CountLowInformationFilteredCandidates()));
	}
	if (Controller.IsValid() && Controller->GetCandidates().ContainsByPredicate(
		[](const FCandidatePtr& Candidate)
		{
			return Candidate.IsValid() && Candidate->bDismissedFromReview;
		}))
	{
		return LOCTEXT(
			"AllCandidatesRemovedFromReview",
			"All suggestions were removed from this review. Choose Back to setup, then explore again to see them again.");
	}
	return LOCTEXT("NoCandidates", "No eligible object suggestions yet.");
}

FText SConvaiSceneAutoTagger::GetSelectedActorTitle() const
{
	return SelectedCandidate.IsValid()
		? FText::FromString(SelectedCandidate->ActorLabel)
		: LOCTEXT("NoSelectionTitle", "Select a suggestion");
}

FText SConvaiSceneAutoTagger::GetSelectedActorSubtext() const
{
	if (!SelectedCandidate.IsValid())
	{
		return LOCTEXT("NoSelectionSubtext", "Choose a row to inspect or edit it. Use the checkboxes for batch review actions.");
	}
	return FText::Format(
		LOCTEXT("ActorSubtextFormat", "{0}  •  {1}"),
		FText::FromString(LexToString(SelectedCandidate->Source)),
		FText::FromString(SelectedCandidate->ActorPath));
}

FText SConvaiSceneAutoTagger::GetSelectedNameText() const
{
	return SelectedCandidate.IsValid() ? FText::FromString(SelectedCandidate->SuggestedName) : FText::GetEmpty();
}

FText SConvaiSceneAutoTagger::GetSelectedDescriptionText() const
{
	return SelectedCandidate.IsValid() ? FText::FromString(SelectedCandidate->Description) : FText::GetEmpty();
}

FText SConvaiSceneAutoTagger::GetSelectedRefinementNoteText() const
{
	return SelectedCandidate.IsValid()
		? FText::FromString(SelectedCandidate->RefinementNote)
		: FText::GetEmpty();
}

FText SConvaiSceneAutoTagger::GetRefinementNoteCharacterCountText() const
{
	return FText::Format(
		LOCTEXT("RefinementNoteCountFormat", "{0}/{1}"),
		FText::AsNumber(SelectedCandidate.IsValid() ? SelectedCandidate->RefinementNote.Len() : 0),
		FText::AsNumber(
			ConvaiSceneAutoTagger::VisionProtocol::GetMaximumRefinementNoteLength()));
}

FText SConvaiSceneAutoTagger::GetRefinementNoteFooterText() const
{
	if (SelectedCandidate.IsValid()
		&& !SelectedCandidate->Error.IsEmpty()
		&& !ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			SelectedCandidate->RefinementNote).IsEmpty())
	{
		return LOCTEXT(
			"RefinementNotAppliedFooter",
			"The note was not applied. Edit it or try Analyze with note again.");
	}
	if (SelectedCandidate.IsValid()
		&& SelectedCandidate->Decision == ESceneAutoTaggerDecision::Accepted
		&& !ConvaiSceneAutoTagger::VisionProtocol::NormalizeRefinementNote(
			SelectedCandidate->RefinementNote).IsEmpty())
	{
		return LOCTEXT(
			"IncludedRefinementFooter",
			"Analyze this note to replace the included suggestion, or clear it to keep the current text.");
	}
	return LOCTEXT(
		"RefinementFooter",
		"Uses the saved image. The note clears after a successful update.");
}

FText SConvaiSceneAutoTagger::GetSelectedScoreText() const
{
	if (!SelectedCandidate.IsValid())
	{
		return FText::GetEmpty();
	}
	if (SelectedCandidate->bUserCapturePendingAnalysis)
	{
		return FText::Format(
			LOCTEXT("StaleScoresFormat", "Local relevance {0}  •  AI confidence — (previous image: {1})  •  Source {2}"),
			FText::AsPercent(SelectedCandidate->SignificanceScore),
			SelectedCandidate->bTagComplete
				? FText::AsPercent(SelectedCandidate->Confidence)
				: LOCTEXT("NoPreviousConfidence", "not available"),
			FText::FromString(LexToString(SelectedCandidate->Source)));
	}
	const FText Confidence = SelectedCandidate->bTagComplete
		? FText::AsPercent(SelectedCandidate->Confidence)
		: LOCTEXT("PendingConfidence", "pending");
	return FText::Format(
		LOCTEXT("ScoresFormat", "Local relevance {0}  •  AI confidence {1} (uncalibrated)  •  Source {2}"),
		FText::AsPercent(SelectedCandidate->SignificanceScore),
		Confidence,
		FText::FromString(LexToString(SelectedCandidate->Source)));
}

FText SConvaiSceneAutoTagger::GetSelectedReasonText() const
{
	if (!SelectedCandidate.IsValid())
	{
		return FText::GetEmpty();
	}
	FString Reason = SelectedCandidate->SignificanceReason;
	if (SelectedCandidate->DuplicateGroupSize > 1)
	{
		Reason += FString::Printf(TEXT("  •  Verified duplicate group: %d actors"), SelectedCandidate->DuplicateGroupSize);
	}
	return FText::Format(LOCTEXT("ReasonFormat", "Why analyzed: {0}"), FText::FromString(Reason));
}

FText SConvaiSceneAutoTagger::GetSelectedErrorText() const
{
	if (!SelectedCandidate.IsValid() || SelectedCandidate->Error.IsEmpty())
	{
		return FText::GetEmpty();
	}
	return FText::Format(
		LOCTEXT("SelectedErrorWithRecovery", "{0}\n\n{1}"),
		FText::FromString(SelectedCandidate->Error),
		HasValidEvidence(SelectedCandidate.Get())
			? LOCTEXT("SelectedErrorAnalyzeRecovery", "Check this row, then choose More > Analyze checked.")
			: LOCTEXT("SelectedErrorRecaptureRecovery", "Check this row, then choose More > Recapture & analyze checked."));
}

FText SConvaiSceneAutoTagger::GetDecisionCountsText() const
{
	if (!Controller.IsValid())
	{
		return LOCTEXT("NoDecisionCounts", "Not included 0  •  Included 0  •  Applied 0");
	}
	const int32 NotIncluded = Controller->CountDecision(ESceneAutoTaggerDecision::Pending)
		+ Controller->CountDecision(ESceneAutoTaggerDecision::Rejected);
	return FText::Format(
		LOCTEXT("DecisionCountsFormat", "Not included {0}  •  Included {1}  •  Applied {2}"),
		FText::AsNumber(NotIncluded),
		FText::AsNumber(Controller->CountDecision(ESceneAutoTaggerDecision::Accepted)),
		FText::AsNumber(Controller->CountApplied()));
}

FText SConvaiSceneAutoTagger::GetApplyButtonText() const
{
	const int32 ReadyToApply = Controller.IsValid() ? Controller->CountReadyToApply() : 0;
	return FText::Format(LOCTEXT("ApplyButtonFormat", "Apply {0}"), FText::AsNumber(ReadyToApply));
}

FText SConvaiSceneAutoTagger::GetApplyToolTip() const
{
	if (!Controller.IsValid())
	{
		return LOCTEXT("ApplyNoController", "The apply controller is unavailable.");
	}
	if (Controller->IsBusy())
	{
		return LOCTEXT("ApplyBusy", "Wait for exploration to finish before applying scene changes.");
	}
	if (Controller->CountReadyToApply() == 0)
	{
		return LOCTEXT("ApplyNoneAccepted", "Include at least one completed suggestion first.");
	}
	return LOCTEXT("ApplyReady", "Add or update Convai Object Components for included actors in one undoable editor transaction. Saving remains a separate action.");
}

FText SConvaiSceneAutoTagger::GetErrorBannerText() const
{
	if (!LocalError.IsEmpty())
	{
		return FText::FromString(LocalError);
	}
	if (Controller.IsValid() && !Controller->IsNativeCoreReady())
	{
		return FText::FromString(Controller->GetNativeCoreDiagnostic());
	}
	return Controller.IsValid() ? FText::FromString(Controller->GetLastError()) : FText::GetEmpty();
}

FText SConvaiSceneAutoTagger::GetApplySummaryText() const
{
	return Controller.IsValid() ? FText::FromString(Controller->GetLastApplySummary()) : FText::GetEmpty();
}

FSlateColor SConvaiSceneAutoTagger::GetSelectedErrorColor() const
{
	return FSlateColor(FLinearColor(1.0f, 0.62f, 0.58f));
}

EVisibility SConvaiSceneAutoTagger::GetReviewVisibility() const
{
	return Controller.IsValid() && !Controller->GetCandidates().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetEmptyStateVisibility() const
{
	return Controller.IsValid() && !Controller->GetCandidates().IsEmpty()
		? EVisibility::Collapsed
		: EVisibility::Visible;
}

EVisibility SConvaiSceneAutoTagger::GetEmptyResultsVisibility() const
{
	return FilteredCandidates.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetSelectedErrorVisibility() const
{
	return SelectedCandidate.IsValid() && !SelectedCandidate->Error.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetRefinementSectionVisibility() const
{
	if (!SelectedCandidate.IsValid() || SelectedCandidate->bApplied
		|| SelectedCandidate->bDismissedFromReview)
	{
		return EVisibility::Collapsed;
	}
	const int32 Resolution = SelectedCandidate->PreviewSize.X;
	return Resolution > 0 && SelectedCandidate->PreviewSize.Y == Resolution
		&& SelectedCandidate->PreviewBGRA.Num()
			== Resolution * Resolution * static_cast<int32>(sizeof(FColor))
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetClearRefinementNoteVisibility() const
{
	return SelectedCandidate.IsValid() && !SelectedCandidate->RefinementNote.IsEmpty()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetErrorBannerVisibility() const
{
	const bool bHasControllerError = Controller.IsValid() && !Controller->GetLastError().IsEmpty();
	const bool bNativeCoreUnavailable = Controller.IsValid() && !Controller->IsNativeCoreReady();
	return !LocalError.IsEmpty() || bHasControllerError || bNativeCoreUnavailable
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SConvaiSceneAutoTagger::GetApplySummaryVisibility() const
{
	return Controller.IsValid() && !Controller->GetLastApplySummary().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
