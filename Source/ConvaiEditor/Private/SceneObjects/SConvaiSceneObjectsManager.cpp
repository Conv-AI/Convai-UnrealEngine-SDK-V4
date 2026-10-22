// Copyright Convai Inc. All Rights Reserved.

#include "SConvaiSceneObjectsManager.h"

#include "SceneObjects/ConvaiMovementPointGenerator.h"

#include "ConvaiObjectComponent.h"
#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "LevelUtils.h"
#include "Misc/MessageDialog.h"
#include "Utility/ConvaiEditorEngineCompat.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UnrealEdGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SConvaiSceneObjectsManager"

struct FConvaiSceneObjectManagerRow
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UConvaiObjectComponent> Component;
	FString ActorLabel;
	FString ActorPath;
	FString ObjectName;
	FString ComponentName;
	FString ScopeText;
	FText RemovalProtectionReason;
	FText MovementPointEditReason;
	int32 GeneratedMovementPointCount = 0;
	int32 ManualMovementPointCount = 0;
	bool bGenerated = false;
	bool bRemovable = false;
	bool bMovementPointsEditable = false;
};

namespace ConvaiSceneObjectsManagerPrivate
{
	const FName GeneratedSourceMarker(TEXT("ConvaiAutoTag.Managed"));

	FSlateColor SourceColor(const FConvaiSceneObjectManagerRow& Row)
	{
		return Row.bGenerated
			? FSlateColor(FLinearColor(0.20f, 0.78f, 0.66f))
			: FSlateColor::UseSubduedForeground();
	}

	FText SourceText(const FConvaiSceneObjectManagerRow& Row)
	{
		if (Row.bGenerated)
		{
			return LOCTEXT("GeneratedSource", "Generated");
		}
		return Row.bRemovable
			? LOCTEXT("EditorInstanceSource", "Editor instance")
			: LOCTEXT("SourceDefined", "Blueprint / native");
	}

	FText MovementPointText(const FConvaiSceneObjectManagerRow& Row)
	{
		if (Row.GeneratedMovementPointCount > 0 && Row.ManualMovementPointCount > 0)
		{
			return FText::Format(
				LOCTEXT("MixedMovementPoints", "{0} generated + {1} manual"),
				FText::AsNumber(Row.GeneratedMovementPointCount),
				FText::AsNumber(Row.ManualMovementPointCount));
		}
		if (Row.GeneratedMovementPointCount > 0)
		{
			return FText::Format(
				LOCTEXT("GeneratedMovementPoints", "{0} generated"),
				FText::AsNumber(Row.GeneratedMovementPointCount));
		}
		if (Row.ManualMovementPointCount > 0)
		{
			return FText::Format(
				LOCTEXT("ManualMovementPoints", "{0} manual"),
				FText::AsNumber(Row.ManualMovementPointCount));
		}
		return LOCTEXT("NoMovementPoints", "None");
	}

	bool IsCheckable(const FConvaiSceneObjectManagerRow& Row)
	{
		return Row.bRemovable || Row.bMovementPointsEditable;
	}

	FText CheckBoxToolTip(const FConvaiSceneObjectManagerRow& Row)
	{
		if (IsCheckable(Row))
		{
			return Row.bRemovable
				? FText::GetEmpty()
				: FText::Format(
					LOCTEXT(
						"MovementOnlyCheckTooltip",
						"This object can receive movement points. Removing its Convai component is unavailable: {0}"),
					Row.RemovalProtectionReason);
		}
		return !Row.MovementPointEditReason.IsEmpty()
			? Row.MovementPointEditReason
			: Row.RemovalProtectionReason;
	}

	FText SkippedMovementPointSummary(
		const FConvaiMovementPointGenerationResult& Result)
	{
		TArray<FString> Reasons;
		if (Result.ExistingPointComponents > 0)
		{
			Reasons.Add(FString::Printf(
				TEXT("%d already had points"),
				Result.ExistingPointComponents));
		}
		if (Result.NoGeneratedPointComponents > 0)
		{
			Reasons.Add(FString::Printf(
				TEXT("%d had no generated points"),
				Result.NoGeneratedPointComponents));
		}
		if (Result.MissingNavigationComponents > 0)
		{
			Reasons.Add(FString::Printf(
				TEXT("%d need navigation"),
				Result.MissingNavigationComponents));
		}
		if (Result.NoValidCandidateComponents > 0)
		{
			Reasons.Add(FString::Printf(
				TEXT("%d had no safe visible position"),
				Result.NoValidCandidateComponents));
		}
		if (Result.NonEditableComponents + Result.InvalidComponents > 0)
		{
			Reasons.Add(FString::Printf(
				TEXT("%d were unavailable or read-only"),
				Result.NonEditableComponents + Result.InvalidComponents));
		}
		return Reasons.IsEmpty()
			? FText::GetEmpty()
			: FText::FromString(FString::Printf(
				TEXT(" Skipped: %s."),
				*FString::Join(Reasons, TEXT(", "))));
	}

	bool IsGeneratedComponent(const UConvaiObjectComponent& Component)
	{
		return Component.ComponentTags.Contains(GeneratedSourceMarker);
	}

	bool CanRemoveInstanceComponent(
		const UConvaiObjectComponent& Component,
		FText& OutReason)
	{
		OutReason = FText::GetEmpty();
		const AActor* Owner = Component.GetOwner();
		const UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
		const UWorld* CurrentEditorWorld = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		const bool bIsPlacedInstance = IsValid(Owner)
			&& Component.CreationMethod == EComponentCreationMethod::Instance
			&& Owner->GetInstanceComponents().Contains(&Component);
		if (!bIsPlacedInstance || Owner->IsTemplate() || !World
			|| World != CurrentEditorWorld || World->WorldType != EWorldType::Editor
			|| !Owner->GetLevel())
		{
			OutReason = LOCTEXT(
				"ProtectedComponentSource",
				"This Blueprint, native, construction-script, or non-current-world component must be edited at its source.");
			return false;
		}
		if (FLevelUtils::IsLevelLocked(Owner->GetLevel()))
		{
			OutReason = LOCTEXT(
				"ProtectedLockedLevel",
				"Unlock this actor's level before removing its Convai component.");
			return false;
		}
		if (!GUnrealEd)
		{
			OutReason = LOCTEXT(
				"ProtectedEditorUnavailable",
				"The Unreal Editor deletion service is not available.");
			return false;
		}
		return GUnrealEd->CanDeleteComponent(&Component, &OutReason);
	}

	struct FRemovalSummary
	{
		int32 RemovedComponents = 0;
		int32 RemovedActors = 0;
		int32 SkippedComponents = 0;
	};

	FRemovalSummary RemoveInstanceComponents(
		const TArray<TWeakObjectPtr<UConvaiObjectComponent>>& RequestedComponents)
	{
		FRemovalSummary Result;
		if (RequestedComponents.IsEmpty() || !IsInGameThread() || !GEditor || !GUnrealEd)
		{
			Result.SkippedComponents = RequestedComponents.Num();
			return Result;
		}

		struct FValidatedRemoval
		{
			TWeakObjectPtr<UConvaiObjectComponent> Component;
			TWeakObjectPtr<AActor> Owner;
		};
		TArray<FValidatedRemoval> ValidatedComponents;
		TSet<TWeakObjectPtr<UConvaiObjectComponent>> SeenComponents;
		for (const TWeakObjectPtr<UConvaiObjectComponent>& WeakComponent : RequestedComponents)
		{
			UConvaiObjectComponent* Component = WeakComponent.Get();
			FText CannotDeleteReason;
			if (!IsValid(Component) || SeenComponents.Contains(Component)
				|| !CanRemoveInstanceComponent(*Component, CannotDeleteReason))
			{
				++Result.SkippedComponents;
				continue;
			}
			SeenComponents.Add(Component);
			ValidatedComponents.Add({ Component, Component->GetOwner() });
		}
		if (ValidatedComponents.IsEmpty())
		{
			return Result;
		}

		FScopedTransaction Transaction(FText::Format(
			LOCTEXT("RemoveTransaction", "Remove {0} Convai Object Component(s)"),
			FText::AsNumber(ValidatedComponents.Num())));
		TArray<UActorComponent*> ComponentsToDelete;
		for (const FValidatedRemoval& Removal : ValidatedComponents)
		{
			UConvaiObjectComponent* Component = Removal.Component.Get();
			AActor* Owner = Removal.Owner.Get();
			if (!IsValid(Component) || !IsValid(Owner))
			{
				++Result.SkippedComponents;
				continue;
			}
			Owner->Modify();
			Component->SetFlags(RF_Transactional);
			Component->Modify();
			Component->MarkPackageDirty();
			Owner->MarkPackageDirty();
			if (ULevel* Level = Owner->GetLevel())
			{
				Level->MarkPackageDirty();
			}
			ComponentsToDelete.Add(Component);
		}

		UTypedElementSelectionSet* SelectionSet = GEditor->GetSelectedComponents()
			? GEditor->GetSelectedComponents()->GetElementSelectionSet()
			: nullptr;
		const bool bDeletionRan = SelectionSet && !ComponentsToDelete.IsEmpty()
			&& GUnrealEd->DeleteComponents(ComponentsToDelete, SelectionSet, true);
		TSet<TWeakObjectPtr<AActor>> ChangedActors;
		for (const FValidatedRemoval& Removal : ValidatedComponents)
		{
			UConvaiObjectComponent* Component = Removal.Component.Get();
			AActor* Owner = Removal.Owner.Get();
			const bool bRemoved = bDeletionRan
				&& (!IsValid(Component) || !IsValid(Owner) || !Owner->OwnsComponent(Component));
			if (bRemoved)
			{
				++Result.RemovedComponents;
				if (IsValid(Owner))
				{
					ChangedActors.Add(Owner);
				}
			}
			else
			{
				++Result.SkippedComponents;
			}
		}
		Result.RemovedActors = ChangedActors.Num();
		if (Result.RemovedComponents == 0)
		{
			Transaction.Cancel();
		}
		else
		{
			if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
			{
				FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"))
					.BroadcastComponentsEdited();
			}
			GEditor->BroadcastLevelActorListChanged();
			GEditor->RedrawLevelEditingViewports();
		}
		return Result;
	}
}

void SConvaiSceneObjectsManager::Construct(const FArguments& InArgs)
{
	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}

	ChildSlot
	[
		SNew(SBorder)
#if UE_VERSION_OLDER_THAN(5, 1, 0)
		.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
#else
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
#endif
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ManagerHeading", "Convai Scene Objects"))
#if UE_VERSION_OLDER_THAN(5, 1, 0)
				.Font(FAppStyle::Get().GetFontStyle("HeadingExtraSmall"))
#else
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
#endif
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT(
					"ManagerHelp",
					"Inspect loaded Convai objects, generate sensible places for characters to stand, "
					"or remove checked components. Nothing is checked automatically."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search actor, Convai name, or component"))
					.OnTextChanged(this, &SConvaiSceneObjectsManager::HandleSearchChanged)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneObjectsManager::GetInventorySummary)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(9.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("Refresh", "Refresh"))
					.OnClicked(this, &SConvaiSceneObjectsManager::HandleRefresh)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 6.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 4.0f))
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.ContentPadding(FMargin(9.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text_Lambda([this]()
					{
						return FText::Format(
							LOCTEXT("CheckVisible", "Check visible ({0})"),
							FText::AsNumber(GetVisibleCheckableCount()));
					})
					.OnClicked(this, &SConvaiSceneObjectsManager::HandleCheckVisible)
					.IsEnabled_Lambda([this]() { return GetVisibleCheckableCount() > 0; })
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.ContentPadding(FMargin(9.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("ClearSelection", "Uncheck all"))
					.OnClicked(this, &SConvaiSceneObjectsManager::HandleClearSelection)
					.IsEnabled_Lambda([this]() { return !CheckedComponents.IsEmpty(); })
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.ContentPadding(FMargin(11.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.ButtonColorAndOpacity(FLinearColor(0.03f, 0.50f, 0.43f))
					.ForegroundColor(FLinearColor::White)
					.Text(LOCTEXT("GenerateMovementPoints", "Generate for checked"))
					.ToolTipText(LOCTEXT(
						"GenerateMovementPointsTooltip",
						"Create up to two navigation-projected, visible standing points for checked objects that do not already have movement points."))
					.OnClicked(this, &SConvaiSceneObjectsManager::HandleGenerateMovementPoints)
					.IsEnabled(this, &SConvaiSceneObjectsManager::CanEditSelectedMovementPoints)
				]
				+ SWrapBox::Slot()
				[
					SNew(SComboButton)
					.ContentPadding(FMargin(10.0f, 3.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.ButtonContent()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("MovementPointMore", "Point options"))
						.Justification(ETextJustify::Center)
					]
					.OnGetMenuContent(this, &SConvaiSceneObjectsManager::BuildMovementPointMenu)
					.IsEnabled(this, &SConvaiSceneObjectsManager::CanEditSelectedMovementPoints)
				]
				+ SWrapBox::Slot()
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneObjectsManager::GetSelectionSummary)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
#if UE_VERSION_OLDER_THAN(5, 1, 0)
				.BorderImage(FAppStyle::Get().GetBrush("Brushes.Header"))
#else
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
#endif
				.Padding(FMargin(8.0f, 5.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(28.0f)]
					+ SHorizontalBox::Slot().FillWidth(0.23f)[SNew(STextBlock).Text(LOCTEXT("ActorColumn", "Actor"))]
					+ SHorizontalBox::Slot().FillWidth(0.25f)[SNew(STextBlock).Text(LOCTEXT("ObjectColumn", "Convai object"))]
					+ SHorizontalBox::Slot().FillWidth(0.14f)[SNew(STextBlock).Text(LOCTEXT("SourceColumn", "Source"))]
					+ SHorizontalBox::Slot().FillWidth(0.18f)[SNew(STextBlock).Text(LOCTEXT("ScopeColumn", "Component scope"))]
					+ SHorizontalBox::Slot().FillWidth(0.20f)[SNew(STextBlock).Text(LOCTEXT("PointsColumn", "Movement points"))]
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(84.0f)]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
			.MinHeight(280.0f)
#endif
			[
#if UE_VERSION_OLDER_THAN(5, 5, 0)
				SNew(SBox)
				.MinDesiredHeight(280.0f)
				[
#endif
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(ListView, SListView<FRowPtr>)
					.ListItemsSource(&FilteredRows)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SConvaiSceneObjectsManager::GenerateRow)
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SConvaiSceneObjectsManager::GetEmptyStateText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Visibility_Lambda([this]()
					{
						return FilteredRows.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
					})
				]
#if UE_VERSION_OLDER_THAN(5, 5, 0)
				]
#endif
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SSeparator)
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
					.Text_Lambda([this]() { return LastStatus; })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Visibility(this, &SConvaiSceneObjectsManager::GetStatusVisibility)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ContentPadding(FMargin(12.0f, 4.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.ButtonColorAndOpacity(FLinearColor(0.68f, 0.16f, 0.14f))
					.ForegroundColor(FLinearColor::White)
					.Text_Lambda([this]()
					{
						return FText::Format(
							LOCTEXT("RemoveSelected", "Remove {0} component(s)"),
							FText::AsNumber(GetCheckedRemovableCount()));
					})
					.ToolTipText(LOCTEXT(
						"RemoveTooltip",
						"Removes only checked instance components in one undoable editor transaction."))
					.OnClicked(this, &SConvaiSceneObjectsManager::HandleRemoveSelected)
					.IsEnabled(this, &SConvaiSceneObjectsManager::CanRemoveSelected)
				]
			]
		]
	];

	RefreshInventory(true);
}

SConvaiSceneObjectsManager::~SConvaiSceneObjectsManager()
{
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
}

void SConvaiSceneObjectsManager::PostUndo(const bool bSuccess)
{
	if (bSuccess)
	{
		LastStatus = FText::GetEmpty();
		RefreshInventory(false);
	}
}

void SConvaiSceneObjectsManager::PostRedo(const bool bSuccess)
{
	if (bSuccess)
	{
		LastStatus = FText::GetEmpty();
		RefreshInventory(false);
	}
}

void SConvaiSceneObjectsManager::RefreshInventory(const bool bClearSelection)
{
	if (bClearSelection)
	{
		CheckedComponents.Reset();
	}
	InventoryRows.Reset();
	GeneratedCount = 0;
	EditorInstanceCount = 0;
	ProtectedCount = 0;

	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TArray<AActor*> Actors;
	if (EditorWorld)
	{
		for (TActorIterator<AActor> Iterator(EditorWorld); Iterator; ++Iterator)
		{
			Actors.Add(*Iterator);
		}
	}

	for (AActor* Actor : Actors)
	{
		if (!IsValid(Actor) || Actor->IsTemplate())
		{
			continue;
		}
		TArray<UConvaiObjectComponent*> Components;
		Actor->GetComponents<UConvaiObjectComponent>(Components);
		for (UConvaiObjectComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}
			FRowPtr Row = MakeShared<FConvaiSceneObjectManagerRow>();
			Row->Actor = Actor;
			Row->Component = Component;
			Row->ActorLabel = Actor->GetActorLabel();
			Row->ActorPath = Actor->GetPathName();
			Row->ObjectName = Component->ObjectEntry.Name.IsEmpty()
				? TEXT("Unnamed Convai object")
				: Component->ObjectEntry.Name;
			Row->ComponentName = Component->GetName();
			Row->ScopeText = Component->ObjectEntry.ObjectReference
				== EConvaiObjectReference::SpecificComponent
				? TEXT("Specific component")
				: TEXT("Whole actor");
			Row->GeneratedMovementPointCount =
				FConvaiMovementPointGenerator::CountGeneratedPoints(*Component);
			Row->ManualMovementPointCount =
				FConvaiMovementPointGenerator::CountManualPoints(*Component);
			Row->bMovementPointsEditable =
				FConvaiMovementPointGenerator::CanEditMovementPoints(
					*Component,
					Row->MovementPointEditReason);
			Row->bGenerated = ConvaiSceneObjectsManagerPrivate::IsGeneratedComponent(*Component);
			Row->bRemovable = ConvaiSceneObjectsManagerPrivate::CanRemoveInstanceComponent(
				*Component,
				Row->RemovalProtectionReason);
			if (!Row->bRemovable)
			{
				++ProtectedCount;
			}
			if (Row->bGenerated)
			{
				++GeneratedCount;
			}
			else if (Row->bRemovable)
			{
				++EditorInstanceCount;
			}
			InventoryRows.Add(MoveTemp(Row));
		}
	}

	InventoryRows.Sort([](const FRowPtr& Left, const FRowPtr& Right)
	{
		const int32 ActorCompare = Left->ActorLabel.Compare(Right->ActorLabel, ESearchCase::IgnoreCase);
		return ActorCompare == 0
			? Left->ObjectName.Compare(Right->ObjectName, ESearchCase::IgnoreCase) < 0
			: ActorCompare < 0;
	});

	TSet<TWeakObjectPtr<UConvaiObjectComponent>> SelectableComponents;
	for (const FRowPtr& Row : InventoryRows)
	{
		if (Row.IsValid() && ConvaiSceneObjectsManagerPrivate::IsCheckable(*Row))
		{
			SelectableComponents.Add(Row->Component);
		}
	}
	for (auto Iterator = CheckedComponents.CreateIterator(); Iterator; ++Iterator)
	{
		if (!SelectableComponents.Contains(*Iterator))
		{
			Iterator.RemoveCurrent();
		}
	}
	RefreshFilter();
}

void SConvaiSceneObjectsManager::RefreshFilter()
{
	FilteredRows.Reset();
	const FString NormalizedSearch = SearchText.TrimStartAndEnd();
	for (const FRowPtr& Row : InventoryRows)
	{
		if (!Row.IsValid())
		{
			continue;
		}
		const bool bMatches = NormalizedSearch.IsEmpty()
			|| Row->ActorLabel.Contains(NormalizedSearch, ESearchCase::IgnoreCase)
			|| Row->ObjectName.Contains(NormalizedSearch, ESearchCase::IgnoreCase)
			|| Row->ComponentName.Contains(NormalizedSearch, ESearchCase::IgnoreCase);
		if (bMatches)
		{
			FilteredRows.Add(Row);
		}
	}
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

void SConvaiSceneObjectsManager::HandleSearchChanged(const FText& NewText)
{
	SearchText = NewText.ToString();
	RefreshFilter();
}

void SConvaiSceneObjectsManager::HandleRowChecked(
	const ECheckBoxState NewState,
	FRowPtr Row)
{
	if (!Row.IsValid()
		|| !ConvaiSceneObjectsManagerPrivate::IsCheckable(*Row)
		|| !Row->Component.IsValid())
	{
		return;
	}
	if (NewState == ECheckBoxState::Checked)
	{
		CheckedComponents.Add(Row->Component);
	}
	else
	{
		CheckedComponents.Remove(Row->Component);
	}
}

ECheckBoxState SConvaiSceneObjectsManager::GetRowChecked(FRowPtr Row) const
{
	return Row.IsValid() && CheckedComponents.Contains(Row->Component)
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

TSharedRef<ITableRow> SConvaiSceneObjectsManager::GenerateRow(
	FRowPtr Row,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Padding(FMargin(8.0f, 5.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(28.0f)
				[
					SNew(SCheckBox)
					.IsChecked(this, &SConvaiSceneObjectsManager::GetRowChecked, Row)
					.OnCheckStateChanged(this, &SConvaiSceneObjectsManager::HandleRowChecked, Row)
					.IsEnabled(ConvaiSceneObjectsManagerPrivate::IsCheckable(*Row))
					.ToolTipText(ConvaiSceneObjectsManagerPrivate::CheckBoxToolTip(*Row))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.23f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Row->ActorLabel))
				.ToolTipText(FText::FromString(Row->ActorPath))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.25f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Row->ObjectName))
				.ToolTipText(FText::FromString(Row->ComponentName))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.14f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(ConvaiSceneObjectsManagerPrivate::SourceText(*Row))
				.ColorAndOpacity(ConvaiSceneObjectsManagerPrivate::SourceColor(*Row))
				.ToolTipText(Row->RemovalProtectionReason)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.18f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Row->ScopeText))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.20f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(ConvaiSceneObjectsManagerPrivate::MovementPointText(*Row))
				.ColorAndOpacity(Row->GeneratedMovementPointCount > 0
					? FSlateColor(FLinearColor(0.20f, 0.78f, 0.66f))
					: FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(84.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(9.0f, 2.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(LOCTEXT("Locate", "Locate"))
					.OnClicked(this, &SConvaiSceneObjectsManager::HandleLocate, Row)
					.IsEnabled(Row->Actor.IsValid())
				]
			]
		];
}

FReply SConvaiSceneObjectsManager::HandleCheckVisible()
{
	for (const FRowPtr& Row : FilteredRows)
	{
		if (Row.IsValid()
			&& ConvaiSceneObjectsManagerPrivate::IsCheckable(*Row)
			&& Row->Component.IsValid())
		{
			CheckedComponents.Add(Row->Component);
		}
	}
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneObjectsManager::HandleClearSelection()
{
	CheckedComponents.Reset();
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SConvaiSceneObjectsManager::HandleRefresh()
{
	LastStatus = FText::GetEmpty();
	RefreshInventory(false);
	return FReply::Handled();
}

FReply SConvaiSceneObjectsManager::HandleLocate(FRowPtr Row)
{
	AActor* Actor = Row.IsValid() ? Row->Actor.Get() : nullptr;
	if (IsValid(Actor) && GEditor)
	{
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(Actor, true, true, true);
		GEditor->MoveViewportCamerasToActor(*Actor, true);
	}
	return FReply::Handled();
}

FReply SConvaiSceneObjectsManager::HandleGenerateMovementPoints()
{
	if (!CanEditSelectedMovementPoints())
	{
		return FReply::Handled();
	}
	TArray<TWeakObjectPtr<UConvaiObjectComponent>> Components = CheckedComponents.Array();
	const FConvaiMovementPointGenerationResult Result =
		FConvaiMovementPointGenerator::Apply(
			Components,
			EConvaiMovementPointGenerationOperation::Generate);
	if (Result.ChangedComponents > 0)
	{
		LastStatus = FText::Format(
			LOCTEXT(
				"GeneratedMovementPointResult",
				"Generated {0} standing point(s) for {1} object(s).{2}"),
			FText::AsNumber(Result.GeneratedPoints),
			FText::AsNumber(Result.ChangedComponents),
			ConvaiSceneObjectsManagerPrivate::SkippedMovementPointSummary(Result));
	}
	else if (Result.NonEditableComponents > 0)
	{
		LastStatus = LOCTEXT(
			"MovementPointObjectsUnavailable",
			"No points were generated. Refresh the list and unlock the actors' level, then try again.");
	}
	else if (Result.MissingNavigationComponents > 0)
	{
		LastStatus = LOCTEXT(
			"MovementPointNavigationMissing",
			"No points were generated. Build navigation for this level, then try again.");
	}
	else if (Result.NoValidCandidateComponents > 0)
	{
		LastStatus = LOCTEXT(
			"MovementPointNoCandidate",
			"No safe visible standing point was found. Check the navmesh and nearby collision.");
	}
	else
	{
		LastStatus = LOCTEXT(
			"MovementPointExistingPoints",
			"Nothing changed. Checked objects that already have movement points were preserved.");
	}
	RefreshInventory(false);
	return FReply::Handled();
}

void SConvaiSceneObjectsManager::HandleRegenerateMovementPoints()
{
	if (!CanEditSelectedMovementPoints())
	{
		return;
	}
	TArray<TWeakObjectPtr<UConvaiObjectComponent>> Components = CheckedComponents.Array();
	const FConvaiMovementPointGenerationResult Result =
		FConvaiMovementPointGenerator::Apply(
			Components,
			EConvaiMovementPointGenerationOperation::Regenerate);
	if (Result.ChangedComponents > 0)
	{
		LastStatus = FText::Format(
			LOCTEXT(
				"RegeneratedMovementPointResult",
				"Regenerated {0} standing point(s) for {1} object(s); manual points were kept.{2}"),
			FText::AsNumber(Result.GeneratedPoints),
			FText::AsNumber(Result.ChangedComponents),
			ConvaiSceneObjectsManagerPrivate::SkippedMovementPointSummary(Result));
	}
	else if (Result.NonEditableComponents > 0)
	{
		LastStatus = LOCTEXT(
			"RegenerateObjectsUnavailable",
			"No points were regenerated. Refresh the list and unlock the actors' level, then try again.");
	}
	else if (Result.MissingNavigationComponents > 0)
	{
		LastStatus = LOCTEXT(
			"RegenerateNavigationMissing",
			"No points were regenerated. Build navigation for this level, then try again.");
	}
	else if (Result.NoValidCandidateComponents > 0)
	{
		LastStatus = LOCTEXT(
			"RegenerateNoCandidate",
			"Existing generated points were kept because no safe replacement was found.");
	}
	else
	{
		LastStatus = LOCTEXT(
			"NoGeneratedPointsToRegenerate",
			"Nothing changed. The checked objects have no unchanged generated points to replace.");
	}
	RefreshInventory(false);
}

void SConvaiSceneObjectsManager::HandleClearGeneratedMovementPoints()
{
	if (!CanEditSelectedMovementPoints())
	{
		return;
	}
	TArray<TWeakObjectPtr<UConvaiObjectComponent>> Components = CheckedComponents.Array();
	const FConvaiMovementPointGenerationResult Result =
		FConvaiMovementPointGenerator::Apply(
			Components,
			EConvaiMovementPointGenerationOperation::ClearGenerated);
	if (Result.ChangedComponents > 0)
	{
		LastStatus = FText::Format(
			LOCTEXT(
				"ClearedMovementPointResult",
				"Removed {0} generated point(s) from {1} object(s); manual points were kept.{2}"),
			FText::AsNumber(Result.RemovedGeneratedPoints),
			FText::AsNumber(Result.ChangedComponents),
			ConvaiSceneObjectsManagerPrivate::SkippedMovementPointSummary(Result));
	}
	else if (Result.NonEditableComponents > 0)
	{
		LastStatus = LOCTEXT(
			"ClearObjectsUnavailable",
			"No points were cleared. Refresh the list and unlock the actors' level, then try again.");
	}
	else
	{
		LastStatus = LOCTEXT(
			"NoGeneratedPointsToClear",
			"Nothing changed. The checked objects have no unchanged generated points to clear.");
	}
	RefreshInventory(false);
}

TSharedRef<SWidget> SConvaiSceneObjectsManager::BuildMovementPointMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);
	MenuBuilder.BeginSection(
		"MovementPoints",
		LOCTEXT("MovementPointsMenuSection", "Movement points"));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("RegenerateGeneratedPoints", "Regenerate generated points"),
		LOCTEXT(
			"RegenerateGeneratedPointsTooltip",
			"Replace only unchanged points created by this tool. Manual or edited points are preserved."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &SConvaiSceneObjectsManager::HandleRegenerateMovementPoints),
			FCanExecuteAction::CreateSP(this, &SConvaiSceneObjectsManager::CanEditSelectedMovementPoints)));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ClearGeneratedPoints", "Clear generated points"),
		LOCTEXT(
			"ClearGeneratedPointsTooltip",
			"Remove only unchanged points created by this tool. Manual or edited points are preserved."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &SConvaiSceneObjectsManager::HandleClearGeneratedMovementPoints),
			FCanExecuteAction::CreateSP(this, &SConvaiSceneObjectsManager::CanEditSelectedMovementPoints)));
	MenuBuilder.EndSection();
	return MenuBuilder.MakeWidget();
}

bool SConvaiSceneObjectsManager::CanEditSelectedMovementPoints() const
{
	if (CheckedComponents.IsEmpty() || !GEditor || GEditor->PlayWorld)
	{
		return false;
	}
	for (const FRowPtr& Row : InventoryRows)
	{
		if (Row.IsValid() && Row->bMovementPointsEditable
			&& CheckedComponents.Contains(Row->Component))
		{
			return true;
		}
	}
	return false;
}

FReply SConvaiSceneObjectsManager::HandleRemoveSelected()
{
	if (!CanRemoveSelected())
	{
		return FReply::Handled();
	}

	TArray<FRowPtr> SelectedRows;
	TSet<TWeakObjectPtr<AActor>> SelectedActors;
	int32 AuthoredInstanceCount = 0;
	for (const FRowPtr& Row : InventoryRows)
	{
		if (Row.IsValid() && Row->bRemovable && CheckedComponents.Contains(Row->Component))
		{
			SelectedRows.Add(Row);
			SelectedActors.Add(Row->Actor);
			AuthoredInstanceCount += Row->bGenerated ? 0 : 1;
		}
	}
	if (SelectedRows.IsEmpty())
	{
		RefreshInventory(false);
		return FReply::Handled();
	}
	const auto SelectedRowsBelongToCurrentEditorWorld = [&SelectedRows]()
	{
		UWorld* CurrentEditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!CurrentEditorWorld)
		{
			return false;
		}
		for (const FRowPtr& Row : SelectedRows)
		{
			AActor* Actor = Row.IsValid() ? Row->Actor.Get() : nullptr;
			UConvaiObjectComponent* Component = Row.IsValid() ? Row->Component.Get() : nullptr;
			if (!IsValid(Actor) || !IsValid(Component) || Actor->GetWorld() != CurrentEditorWorld
				|| Component->GetOwner() != Actor)
			{
				return false;
			}
		}
		return true;
	};
	if (!SelectedRowsBelongToCurrentEditorWorld())
	{
		RefreshInventory(true);
		LastStatus = LOCTEXT(
			"LoadedLevelChangedBeforeRemoval",
			"The loaded level changed. The list was refreshed; select objects again.");
		return FReply::Handled();
	}

	const UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FText MapName = EditorWorld
		? FText::FromString(EditorWorld->GetName())
		: LOCTEXT("UnknownMap", "the current map");
	const FText Warning = AuthoredInstanceCount > 0
		? FText::Format(
			LOCTEXT(
				"AuthoredInstanceRemovalWarning",
				"\n\n{0} selected component(s) were added directly to placed actors but are not marked as generated. They may contain hand-authored data."),
			FText::AsNumber(AuthoredInstanceCount))
		: FText::GetEmpty();
	const FText Body = FText::Format(
		LOCTEXT(
			"RemoveConfirmationBody",
			"This removes {0} editor-instance Convai Object Component(s) from {1} loaded actor(s) in {2}. Blueprint, native, and construction-script components are source-protected and cannot be removed here. Actors and unrelated components are not changed. Unreal can undo the operation in one step while this editor session remains open.{3}"),
		FText::AsNumber(SelectedRows.Num()),
		FText::AsNumber(SelectedActors.Num()),
		MapName,
		Warning);
	const FText Title = FText::Format(
		AuthoredInstanceCount > 0
			? LOCTEXT("RemoveMixedTitle", "Remove {0} Convai Object Components?")
			: LOCTEXT("RemoveGeneratedTitle", "Remove {0} generated Convai Object Components?"),
		FText::AsNumber(SelectedRows.Num()));
	if (ConvaiOpenMessageDialog(EAppMsgType::YesNo, Body, Title) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}
	if (!SelectedRowsBelongToCurrentEditorWorld())
	{
		RefreshInventory(true);
		LastStatus = LOCTEXT(
			"LoadedLevelChangedDuringRemoval",
			"The loaded level changed. Nothing was removed; select objects again.");
		return FReply::Handled();
	}

	TArray<TWeakObjectPtr<UConvaiObjectComponent>> ComponentsToRemove;
	ComponentsToRemove.Reserve(SelectedRows.Num());
	for (const FRowPtr& Row : SelectedRows)
	{
		ComponentsToRemove.Add(Row->Component);
	}
	const ConvaiSceneObjectsManagerPrivate::FRemovalSummary Result =
		ConvaiSceneObjectsManagerPrivate::RemoveInstanceComponents(ComponentsToRemove);

	LastStatus = FText::Format(
		LOCTEXT(
			"RemovalResult",
			"Removed {0} component(s) from {1} actor(s); {2} skipped because they were protected or changed."),
		FText::AsNumber(Result.RemovedComponents),
		FText::AsNumber(Result.RemovedActors),
		FText::AsNumber(Result.SkippedComponents));
	RefreshInventory(true);
	return FReply::Handled();
}

bool SConvaiSceneObjectsManager::CanRemoveSelected() const
{
	if (GetCheckedRemovableCount() == 0 || !GEditor || GEditor->PlayWorld)
	{
		return false;
	}
	return true;
}

int32 SConvaiSceneObjectsManager::GetCheckedRemovableCount() const
{
	int32 Count = 0;
	for (const FRowPtr& Row : InventoryRows)
	{
		Count += Row.IsValid() && Row->bRemovable
			&& CheckedComponents.Contains(Row->Component)
			? 1
			: 0;
	}
	return Count;
}

int32 SConvaiSceneObjectsManager::GetVisibleCheckableCount() const
{
	int32 Count = 0;
	for (const FRowPtr& Row : FilteredRows)
	{
		Count += Row.IsValid()
			&& ConvaiSceneObjectsManagerPrivate::IsCheckable(*Row)
			? 1
			: 0;
	}
	return Count;
}

FText SConvaiSceneObjectsManager::GetInventorySummary() const
{
	return FText::Format(
		LOCTEXT("InventorySummary", "{0} objects  •  {1} generated  •  {2} editor-added  •  {3} source-protected"),
		FText::AsNumber(InventoryRows.Num()),
		FText::AsNumber(GeneratedCount),
		FText::AsNumber(EditorInstanceCount),
		FText::AsNumber(ProtectedCount));
}

FText SConvaiSceneObjectsManager::GetSelectionSummary() const
{
	int32 VisibleCheckedCount = 0;
	for (const FRowPtr& Row : FilteredRows)
	{
		VisibleCheckedCount += Row.IsValid() && CheckedComponents.Contains(Row->Component)
			? 1
			: 0;
	}
	const int32 HiddenCheckedCount = CheckedComponents.Num() - VisibleCheckedCount;
	if (HiddenCheckedCount > 0)
	{
		return FText::Format(
			LOCTEXT("CheckedSummaryWithHidden", "{0} checked  •  {1} hidden by filter"),
			FText::AsNumber(CheckedComponents.Num()),
			FText::AsNumber(HiddenCheckedCount));
	}
	return FText::Format(
		LOCTEXT("SelectionSummary", "{0} checked"),
		FText::AsNumber(CheckedComponents.Num()));
}

FText SConvaiSceneObjectsManager::GetEmptyStateText() const
{
	return SearchText.IsEmpty()
		? LOCTEXT("NoObjects", "No Convai Object Components were found in the loaded editor scene.")
		: LOCTEXT("NoSearchMatches", "No components match this search.");
}

EVisibility SConvaiSceneObjectsManager::GetStatusVisibility() const
{
	return LastStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

#undef LOCTEXT_NAMESPACE
