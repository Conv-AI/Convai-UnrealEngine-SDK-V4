// Copyright Convai. All Rights Reserved.
#pragma once

#include "Misc/EngineVersionComparison.h"
#include "Misc/MessageDialog.h"

inline EAppReturnType::Type ConvaiOpenMessageDialog(EAppMsgType::Type MessageType,
	const FText& Message, const FText& Title)
{
#if UE_VERSION_OLDER_THAN(5, 3, 0)
	return FMessageDialog::Open(MessageType, Message, &Title);
#else
	return FMessageDialog::Open(MessageType, Message, Title);
#endif
}
