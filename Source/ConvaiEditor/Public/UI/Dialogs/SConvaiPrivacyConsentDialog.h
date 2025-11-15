/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SConvaiPrivacyConsentDialog.h
 *
 * Privacy consent dialog for data collection.
 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Privacy consent dialog for data collection.
 */
class SConvaiPrivacyConsentDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SConvaiPrivacyConsentDialog) {}
    SLATE_END_ARGS()

    /** Constructs the widget */
    void Construct(const FArguments &InArgs);

    /**
     * Shows the privacy consent dialog
     * @param bIsForExport True if for log export, false if for support request
     * @return True if user accepts, false if declines
     */
    static bool ShowConsentDialog(bool bIsForExport);
};
