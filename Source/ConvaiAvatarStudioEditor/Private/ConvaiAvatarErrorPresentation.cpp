// Copyright Convai Inc. All Rights Reserved.
#include "ConvaiAvatarErrorPresentation.h"

namespace ConvaiAvatarErrorPresentation
{
    FString RedactDiagnostic(FString Text, const FString& Credential)
    {
        for (const FString& Prefix : {FString(TEXT("https://")), FString(TEXT("http://"))})
        {
            int32 Start = Text.Find(Prefix, ESearchCase::IgnoreCase);
            while (Start != INDEX_NONE)
            {
                int32 End = Start;
                while (End < Text.Len() && !FChar::IsWhitespace(Text[End]) && Text[End] != TEXT('"') && Text[End] != TEXT('\'')) ++End;
                Text = Text.Left(Start) + TEXT("[redacted URL]") + Text.Mid(End);
                Start = Text.Find(Prefix, ESearchCase::IgnoreCase);
            }
        }
        if (!Credential.IsEmpty()) Text.ReplaceInline(*Credential, TEXT("[redacted credential]"), ESearchCase::CaseSensitive);
        return Text.Left(16384);
    }

    FString LocalRecoveryMessage(const FString& Error)
    {
        FString Summary = Error.TrimStartAndEnd();
        int32 End = INDEX_NONE;
        const bool bHasDetails = Summary.FindChar(TEXT('\n'), End);
        if (bHasDetails) Summary = Summary.Left(End).TrimEnd();
        const bool bTooLong = Summary.Len() > 1024;
        if (bTooLong) Summary = Summary.Left(1024).TrimEnd() + TEXT("...");
        if (Summary.IsEmpty()) Summary = TEXT("This operation could not finish.");
        if (bHasDetails || bTooLong) Summary += TEXT(" Open the latest log for the affected files and full details.");
        return Summary;
    }

    FString ForDisplay(const FString& Error, const FString& FriendlyError, const FString& Credential)
    {
        if (!FriendlyError.IsEmpty()) return RedactDiagnostic(FriendlyError, Credential);
        const FString SafeError = RedactDiagnostic(Error, Credential);
        if (SafeError.Contains(TEXT("The file transfer failed (HTTP 200)")))
            return TEXT("The file transfer was interrupted. Check your connection, then retry.");
        if (SafeError.Contains(TEXT(".ps1")) || SafeError.Contains(TEXT("Exception")) ||
            SafeError.Contains(TEXT("CategoryInfo")) || SafeError.Contains(TEXT("\n")))
            return TEXT("This operation could not finish. Open the latest log for details, then retry.");
        return SafeError;
    }
}
