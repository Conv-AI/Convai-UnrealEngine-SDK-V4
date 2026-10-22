// Copyright 2026 Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

namespace ConvaiAvatarEnvironmentRemoval
{
	enum class ESourceAction { None, CancelAdd, Delete, RecreatedDelete, Unknown };
	inline ESourceAction SourceAction(bool bExists, bool bUnknown, bool bAdded, bool bControlled, bool bDeleted)
	{
		if (bUnknown) return ESourceAction::Unknown;
		if (bExists) return bDeleted ? ESourceAction::RecreatedDelete : ESourceAction::None;
		if (bAdded) return ESourceAction::CancelAdd;
		return bControlled && !bDeleted ? ESourceAction::Delete : ESourceAction::None;
	}
	inline bool NeedsCheckout(bool bUsesCheckout, bool bControlled, bool bAdded, bool bDeleted, bool bCheckedOut)
	{
		return bUsesCheckout && bControlled && !bAdded && !bDeleted && !bCheckedOut;
	}
	FString ReceiptPath(FName Map);
	bool Record(FName Map, const TArray<FString>& Files, FString& Error);
	bool Finish(FName Map, TFunctionRef<bool(const TArray<FString>&, const TArray<FString>&)> Finalize, FString& Error);
}
