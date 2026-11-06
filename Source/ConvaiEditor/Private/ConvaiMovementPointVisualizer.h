// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComponentVisualizer.h"
#include "Misc/EngineVersionComparison.h"

class UConvaiObjectComponent;

/** Clickable hit proxy for one Movement Point grab handle. */
struct HConvaiMovementPointProxy : public HComponentVisProxy
{
	DECLARE_HIT_PROXY();

	HConvaiMovementPointProxy(const UActorComponent* InComponent, int32 InPointIndex)
		: HComponentVisProxy(InComponent, HPP_Wireframe)
		, PointIndex(InPointIndex)
	{}

	int32 PointIndex;
};

/**
 * Interactive visualizer for the Movement Points authored on a Convai Object
 * component — the same editing flow the engine uses for nav-link and spline
 * points: while the owning actor (or the component, in the Blueprint editor)
 * is selected, every point draws as a clickable grab handle plus its
 * acceptance-radius ring and a tether line to the owning object. Clicking a
 * handle brings up the native transform gizmo; dragging writes straight back
 * into ObjectEntry.MovementPoints with undo support, and edits made on the
 * Blueprint preview actor propagate to the archetype and its instances.
 * Nothing is ever spawned into the actor, so markers can't duplicate or go
 * stale.
 */
class FConvaiMovementPointVisualizer : public FComponentVisualizer
{
public:
	//~ Begin FComponentVisualizer Interface
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	/** Index + name label above every point, so a designer can tell which array
	 *  element is which while typing names in the Details panel (clicking a
	 *  handle normalizes selection to the actor, which flips Details away). */
	virtual void DrawVisualizationHUD(const UActorComponent* Component, const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;
	virtual bool VisProxyHandleClick(FEditorViewportClient* InViewportClient, HComponentVisProxy* VisProxy, const FViewportClick& Click) override;
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	/** Never auto-select the clicked component: it isn't a scene component, and
	 *  a selection holding only non-scene components makes the level viewport
	 *  hide the transform widget — the gizmo would vanish on click. (The base
	 *  virtual — and the auto-select behavior it disables — exist since 5.6.) */
	virtual bool ShouldAutoSelectElementOnHandleClick() const override { return false; }
#endif
	virtual bool GetWidgetLocation(const FEditorViewportClient* ViewportClient, FVector& OutLocation) const override;
	virtual bool GetCustomInputCoordinateSystem(const FEditorViewportClient* ViewportClient, FMatrix& OutMatrix) const override;
	virtual bool HandleInputDelta(FEditorViewportClient* ViewportClient, FViewport* Viewport, FVector& DeltaTranslate, FRotator& DeltaRotate, FVector& DeltaScale) override;
#if !UE_VERSION_OLDER_THAN(5, 3, 0)
	// The base virtual doesn't exist on the oldest supported engines; there
	// HandleInputDelta sends the final-strength notify per delta instead.
	virtual void TrackingStopped(FEditorViewportClient* InViewportClient, bool bInDidMove) override;
#endif
	virtual void EndEditing() override;
	virtual UActorComponent* GetEditedComponent() const override;
	virtual bool IsVisualizingArchetype() const override;
	//~ End FComponentVisualizer Interface

private:
	/** The component whose point is being edited, re-resolved and re-validated
	 *  on every call — Blueprint compiles/undo can replace the instance (or
	 *  shrink the points array) between calls. Null when editing can't proceed. */
	UConvaiObjectComponent* GetEditedObjectComponent() const;

	/** Property path from the owning actor to the edited component — survives
	 *  actor reconstruction where a raw pointer would dangle. */
	FComponentPropertyPath ComponentPropertyPath;

	/** Index of the selected Movement Point; INDEX_NONE when not editing. */
	int32 SelectedPointIndex = INDEX_NONE;

	/** Set while a drag has applied interactive changes, so TrackingStopped
	 *  can send the one final ValueSet notification. */
	bool bPendingFinalNotify = false;
};
