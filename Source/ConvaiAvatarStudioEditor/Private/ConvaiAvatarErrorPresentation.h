// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

namespace ConvaiAvatarErrorPresentation
{
    /** Safe diagnostic text; retains multiline recovery details but removes capability URLs and credentials. */
    FString RedactDiagnostic(FString Text, const FString& Credential);
    /** For already-redacted errors authored by local workspace code only. Full file lists remain in the diagnostic log. */
    FString LocalRecoveryMessage(const FString& Error);
    /** External/helper errors keep conservative presentation unless the caller supplies a trusted summary. */
    FString ForDisplay(const FString& Error, const FString& FriendlyError, const FString& Credential);
}
