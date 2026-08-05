// Copyright 2022 Convai Inc. All Rights Reserved.

#include "ConvaiMovementPointVisualizer.h"
#include "ConvaiObjectComponent.h"
#include "ConvaiDefinitions.h"
#include "ActorEditorUtils.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "SceneManagement.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersionComparison.h"

IMPLEMENT_HIT_PROXY(HConvaiMovementPointProxy, HComponentVisProxy);

namespace
{
	const FLinearColor EnabledColor  (0.0f, 0.9f, 0.45f); // spring green
	const FLinearColor DisabledColor (0.5f, 0.5f, 0.5f);
	constexpr float GrabHandleSize = 15.0f;
	constexpr float PinLength      = 90.0f; // vertical pin above the point, like the old arrow marker
	constexpr float RingLift       = 2.0f;  // keeps the radius ring from z-fighting into floors

	/** Struct value → world, mirroring FConvaiMovementPoint::ResolveWorldLocation:
	 *  relative points compose onto the anchor, world-locked points ARE world. */
	FTransform PointWorldTransform(const FConvaiMovementPoint& Point, const FTransform& AnchorTM)
	{
		return (Point.Attachment == EConvaiMovementPointAttachment::KeepWorldPosition)
			? Point.Transform
			: Point.Transform * AnchorTM;
	}

	/** The property NotifyPropertyModified propagates: MovementPoints lives
	 *  inside ObjectEntry, and propagation copies whole property values. */
	FProperty* ObjectEntryProperty()
	{
		static FProperty* Prop = FindFProperty<FProperty>(
			UConvaiObjectComponent::StaticClass(),
			GET_MEMBER_NAME_CHECKED(UConvaiObjectComponent, ObjectEntry));
		return Prop;
	}

	/** Undo-aware notify; also copies preview-actor edits onto the Blueprint
	 *  archetype and its instances. The change-type overload arrived in UE5. */
	void NotifyPointsModified(UConvaiObjectComponent* Component, EPropertyChangeType::Type ChangeType)
	{
#if UE_VERSION_OLDER_THAN(5, 0, 0)
		FComponentVisualizer::NotifyPropertyModified(Component, ObjectEntryProperty());
#else
		FComponentVisualizer::NotifyPropertyModified(Component, ObjectEntryProperty(), ChangeType);
#endif
	}
}

void FConvaiMovementPointVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UConvaiObjectComponent* ObjectComp = Cast<UConvaiObjectComponent>(Component);
	if (!ObjectComp || !IsValid(ObjectComp->GetOwner()))
	{
		return;
	}

	const FConvaiObjectEntry& Entry = ObjectComp->ObjectEntry;
	const FTransform AnchorTM = ObjectComp->GetMovementPointAnchorTransform();
	const FVector OwnerLocation = ObjectComp->GetOwner()->GetActorLocation();
	const bool bEditingThisComponent = (ObjectComp == GetEditedComponent());

	for (int32 i = 0; i < Entry.MovementPoints.Num(); ++i)
	{
		const FConvaiMovementPoint& Point = Entry.MovementPoints[i];
		const FTransform WorldTM = PointWorldTransform(Point, AnchorTM);
		const FVector Location = WorldTM.GetLocation();

		const bool bSelected = bEditingThisComponent && (i == SelectedPointIndex);
		const FLinearColor Color = bSelected ? FLinearColor::White
			: (Point.bEnabled ? EnabledColor : DisabledColor);

		// The grab handle: a vertical pin with the grab square at the point.
		// Drawn foreground so it reads even when the point hugs the floor.
		// Clicking anywhere on it selects the point and brings up the gizmo
		// (disabled points stay grey but remain editable).
		PDI->SetHitProxy(new HConvaiMovementPointProxy(Component, i));
		PDI->DrawPoint(Location, Color, GrabHandleSize, SDPG_Foreground);
		PDI->DrawLine(Location, Location + FVector(0.0f, 0.0f, PinLength), Color, SDPG_Foreground, 2.0f);
		PDI->SetHitProxy(nullptr);

		// Context: acceptance-radius ring (lifted a touch off the ground) and
		// tether to the owning object. Ring shows the EFFECTIVE tolerance —
		// the resolver floors the radius at 150 (see PointTolerance), so
		// drawing the raw value would understate what arrival actually tests.
		DrawCircle(PDI, Location + FVector(0.0f, 0.0f, RingLift), FVector::ForwardVector,
			FVector::RightVector, Color, FMath::Max(Entry.AcceptanceRadius, 150.0f), 32, SDPG_World, 1.0f);
		PDI->DrawLine(Location, OwnerLocation, Color * 0.5f, SDPG_World, 0.5f);
	}
}

void FConvaiMovementPointVisualizer::DrawVisualizationHUD(const UActorComponent* Component, const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	const UConvaiObjectComponent* ObjectComp = Cast<UConvaiObjectComponent>(Component);
	if (!ObjectComp || !IsValid(ObjectComp->GetOwner()))
	{
		return;
	}

	const FConvaiObjectEntry& Entry = ObjectComp->ObjectEntry;
	const FTransform AnchorTM = ObjectComp->GetMovementPointAnchorTransform();
	const bool bEditingThisComponent = (ObjectComp == GetEditedComponent());

	for (int32 i = 0; i < Entry.MovementPoints.Num(); ++i)
	{
		const FConvaiMovementPoint& Point = Entry.MovementPoints[i];
		const FVector Location = PointWorldTransform(Point, AnchorTM).GetLocation()
			+ FVector(0.0f, 0.0f, PinLength);

		FVector2D Pixel;
		if (!View->ScreenToPixel(View->WorldToScreen(Location), Pixel))
		{
			continue;
		}
		Pixel /= Canvas->GetDPIScale();

		// "[2] Other Side" — the index is what the Details array shows, the
		// name is what the AI will be able to say. Only separate destinations
		// carry a name; object-itself points read as their bare index.
		const FString PointName = FConvaiObjectEntry::EffectiveMovementPointName(Point);
		const FString Label = PointName.IsEmpty()
			? FString::Printf(TEXT("[%d]"), i)
			: FString::Printf(TEXT("[%d] %s"), i, *PointName);

		const bool bSelected = bEditingThisComponent && (i == SelectedPointIndex);
		const FLinearColor Color = bSelected ? FLinearColor::White
			: (Point.bEnabled ? EnabledColor : DisabledColor);

		FCanvasTextItem Text(Pixel + FVector2D(8.0f, -8.0f),
			FText::FromString(Label), GEngine->GetSmallFont(), Color);
		Text.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Text);
	}
}

bool FConvaiMovementPointVisualizer::VisProxyHandleClick(FEditorViewportClient* InViewportClient, HComponentVisProxy* VisProxy, const FViewportClick& Click)
{
	if (!VisProxy || !VisProxy->Component.IsValid()
		|| !VisProxy->IsA(HConvaiMovementPointProxy::StaticGetType()))
	{
		return false;
	}

	// The level viewport hides the transform widget while the selection holds
	// only non-scene components (FLegacyEdModeWidgetHelper::ShouldDrawWidget) —
	// and this IS a non-scene component, typically selected in the Details
	// panel while authoring points. Normalize selection back to the owning
	// actor so the gizmo can show; it still gets its location from this
	// visualizer. Must happen BEFORE recording the click state below: the
	// selection change triggers NoteSelectionChange → EndEditing.
	AActor* Owner = VisProxy->Component->GetOwner();
	if (Owner && GEditor->GetSelectedComponentCount() > 0
		&& !FActorEditorUtils::IsAPreviewOrInactiveActor(Owner))
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Owner, /*bInSelected*/ true, /*bNotify*/ true);
	}

	ComponentPropertyPath = FComponentPropertyPath(VisProxy->Component.Get());
	if (!ComponentPropertyPath.IsValid())
	{
		EndEditing();
		return false;
	}

	SelectedPointIndex = static_cast<HConvaiMovementPointProxy*>(VisProxy)->PointIndex;
	return true;
}

UConvaiObjectComponent* FConvaiMovementPointVisualizer::GetEditedObjectComponent() const
{
	UConvaiObjectComponent* ObjectComp = Cast<UConvaiObjectComponent>(ComponentPropertyPath.GetComponent());
	return (ObjectComp && ObjectComp->ObjectEntry.MovementPoints.IsValidIndex(SelectedPointIndex))
		? ObjectComp : nullptr;
}

bool FConvaiMovementPointVisualizer::GetWidgetLocation(const FEditorViewportClient* ViewportClient, FVector& OutLocation) const
{
	if (const UConvaiObjectComponent* ObjectComp = GetEditedObjectComponent())
	{
		OutLocation = PointWorldTransform(
			ObjectComp->ObjectEntry.MovementPoints[SelectedPointIndex],
			ObjectComp->GetMovementPointAnchorTransform()).GetLocation();
		return true;
	}
	return false;
}

bool FConvaiMovementPointVisualizer::GetCustomInputCoordinateSystem(const FEditorViewportClient* ViewportClient, FMatrix& OutMatrix) const
{
	if (ViewportClient->GetWidgetCoordSystemSpace() == COORD_Local)
	{
		if (const UConvaiObjectComponent* ObjectComp = GetEditedObjectComponent())
		{
			OutMatrix = FQuatRotationMatrix(PointWorldTransform(
				ObjectComp->ObjectEntry.MovementPoints[SelectedPointIndex],
				ObjectComp->GetMovementPointAnchorTransform()).GetRotation());
			return true;
		}
	}
	return false;
}

bool FConvaiMovementPointVisualizer::HandleInputDelta(FEditorViewportClient* ViewportClient, FViewport* Viewport, FVector& DeltaTranslate, FRotator& DeltaRotate, FVector& DeltaScale)
{
	UConvaiObjectComponent* ObjectComp = GetEditedObjectComponent();
	if (!ObjectComp)
	{
		return false;
	}
	if (DeltaTranslate.IsZero() && DeltaRotate.IsZero())
	{
		// Scale is meaningless for a point the resolver reads as a location;
		// swallow it so the gizmo doesn't fall through to moving the actor.
		return true;
	}

	ObjectComp->Modify();

	// Apply the delta in world space, then store back in the point's own frame.
	FConvaiMovementPoint& Point = ObjectComp->ObjectEntry.MovementPoints[SelectedPointIndex];
	const FTransform AnchorTM = ObjectComp->GetMovementPointAnchorTransform();
	FTransform WorldTM = PointWorldTransform(Point, AnchorTM);
	WorldTM.SetTranslation(WorldTM.GetTranslation() + DeltaTranslate);
	WorldTM.SetRotation(DeltaRotate.Quaternion() * WorldTM.GetRotation());

	const FTransform NewPointTM = (Point.Attachment == EConvaiMovementPointAttachment::KeepWorldPosition)
		? WorldTM
		: WorldTM.GetRelativeTransform(AnchorTM);
	// Location/rotation only — the stored scale is inert at runtime, and
	// GetRelativeTransform would churn it under a scaled anchor.
	Point.Transform.SetTranslation(NewPointTM.GetTranslation());
	Point.Transform.SetRotation(NewPointTM.GetRotation());

#if !UE_VERSION_OLDER_THAN(5, 3, 0)
	// Interactive during the drag; TrackingStopped sends the final ValueSet.
	NotifyPointsModified(ObjectComp, EPropertyChangeType::Interactive);
	bPendingFinalNotify = true;
#else
	// No TrackingStopped virtual on this engine — send the final-strength
	// notify on every delta so the drag still lands on archetype/instances.
	NotifyPointsModified(ObjectComp, EPropertyChangeType::ValueSet);
#endif
	return true;
}

#if !UE_VERSION_OLDER_THAN(5, 3, 0)
void FConvaiMovementPointVisualizer::TrackingStopped(FEditorViewportClient* InViewportClient, bool bInDidMove)
{
	if (bPendingFinalNotify)
	{
		bPendingFinalNotify = false;
		if (UConvaiObjectComponent* ObjectComp = GetEditedObjectComponent())
		{
			NotifyPointsModified(ObjectComp, EPropertyChangeType::ValueSet);
		}
	}
}
#endif

void FConvaiMovementPointVisualizer::EndEditing()
{
	ComponentPropertyPath.Reset();
	SelectedPointIndex = INDEX_NONE;
	bPendingFinalNotify = false;
}

UActorComponent* FConvaiMovementPointVisualizer::GetEditedComponent() const
{
	return ComponentPropertyPath.GetComponent();
}

bool FConvaiMovementPointVisualizer::IsVisualizingArchetype() const
{
	const UActorComponent* Component = GetEditedComponent();
	return Component && Component->GetOwner()
		&& FActorEditorUtils::IsAPreviewOrInactiveActor(Component->GetOwner());
}
