// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "Widgets/SCompoundWidget.h"

class UConvaiObjectComponent;
class ITableRow;
class STableViewBase;
template <typename ItemType> class SListView;

struct FConvaiSceneObjectManagerRow;

/** Inventory, movement-point authoring, and safe instance removal for loaded Convai objects. */
class SConvaiSceneObjectsManager final : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SConvaiSceneObjectsManager) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SConvaiSceneObjectsManager() override;
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
	using FRowPtr = TSharedPtr<FConvaiSceneObjectManagerRow>;

	void RefreshInventory(bool bClearSelection = false);
	void RefreshFilter();
	void HandleSearchChanged(const FText& NewText);
	void HandleRowChecked(ECheckBoxState NewState, FRowPtr Row);
	ECheckBoxState GetRowChecked(FRowPtr Row) const;
	TSharedRef<ITableRow> GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable);
	FReply HandleCheckVisible();
	FReply HandleClearSelection();
	FReply HandleRefresh();
	FReply HandleLocate(FRowPtr Row);
	FReply HandleGenerateMovementPoints();
	void HandleRegenerateMovementPoints();
	void HandleClearGeneratedMovementPoints();
	TSharedRef<SWidget> BuildMovementPointMenu();
	bool CanEditSelectedMovementPoints() const;
	FReply HandleRemoveSelected();
	bool CanRemoveSelected() const;
	int32 GetCheckedRemovableCount() const;
	int32 GetVisibleCheckableCount() const;
	FText GetInventorySummary() const;
	FText GetSelectionSummary() const;
	FText GetEmptyStateText() const;
	EVisibility GetStatusVisibility() const;

	TSharedPtr<SListView<FRowPtr>> ListView;
	TArray<FRowPtr> InventoryRows;
	TArray<FRowPtr> FilteredRows;
	TSet<TWeakObjectPtr<UConvaiObjectComponent>> CheckedComponents;
	FString SearchText;
	FText LastStatus;
	int32 GeneratedCount = 0;
	int32 EditorInstanceCount = 0;
	int32 ProtectedCount = 0;
};
