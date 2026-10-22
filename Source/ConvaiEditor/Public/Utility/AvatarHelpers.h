/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * AvatarHelpers.h
 *
 * Utility functions for generating avatar initials and colors.
 */

#pragma once

#include "CoreMinimal.h"

namespace ConvaiEditor
{
    /** Avatar generation helpers */
    namespace AvatarHelpers
    {
        /** Extracts initials from a username for avatar display */
        inline FString ExtractInitials(const FString &Username)
        {
            FString TrimmedName = Username.TrimStartAndEnd();

            if (TrimmedName.IsEmpty())
            {
                return TEXT("??");
            }

            TrimmedName = TrimmedName.ToUpper();

            TArray<FString> Words;
            TrimmedName.ParseIntoArray(Words, TEXT(" "), true);

            if (Words.Num() == 1)
            {
                TrimmedName.ParseIntoArray(Words, TEXT("_"), true);
            }
            if (Words.Num() == 1)
            {
                TrimmedName.ParseIntoArray(Words, TEXT("-"), true);
            }
            if (Words.Num() == 1)
            {
                TrimmedName.ParseIntoArray(Words, TEXT("."), true);
            }

            Words.RemoveAll([](const FString &Word)
                            { return Word.IsEmpty(); });

            if (Words.Num() == 0)
            {
                return TEXT("??");
            }

            if (Words.Num() >= 2)
            {
                FString First = Words[0].Left(1);
                FString Second = Words[1].Left(1);
                return First + Second;
            }

            FString SingleWord = Words[0];

            if (SingleWord.Len() <= 2)
            {
                return SingleWord.ToUpper();
            }

            FString First = SingleWord.Left(1);
            FString Last = SingleWord.Right(1);
            return First + Last;
        }

        /** Generates a deterministic color for an avatar */
        inline FLinearColor GenerateAvatarColor(const FString &Username)
        {
            uint32 Hash = GetTypeHash(Username);

            static const TArray<FLinearColor> ColorPalette = {
                FLinearColor(0.91f, 0.26f, 0.21f, 1.0f),
                FLinearColor(0.91f, 0.44f, 0.24f, 1.0f),
                FLinearColor(0.95f, 0.61f, 0.07f, 1.0f),
                FLinearColor(0.27f, 0.80f, 0.46f, 1.0f),
                FLinearColor(0.11f, 0.69f, 0.67f, 1.0f),
                FLinearColor(0.25f, 0.59f, 0.96f, 1.0f),
                FLinearColor(0.40f, 0.45f, 0.98f, 1.0f),
                FLinearColor(0.61f, 0.35f, 0.95f, 1.0f),
                FLinearColor(0.91f, 0.28f, 0.62f, 1.0f),
                FLinearColor(0.38f, 0.74f, 0.33f, 1.0f),
                FLinearColor(0.00f, 0.74f, 0.83f, 1.0f),
                FLinearColor(0.91f, 0.12f, 0.39f, 1.0f),
            };

            int32 ColorIndex = Hash % ColorPalette.Num();
            return ColorPalette[ColorIndex];
        }

        /** Validates if a username is suitable for avatar generation */
        inline bool IsValidUsername(const FString &Username)
        {
            return !Username.TrimStartAndEnd().IsEmpty();
        }

        /** Gets a fallback color for when username is invalid */
        inline FLinearColor GetFallbackColor()
        {
            return FLinearColor(0.4f, 0.4f, 0.4f, 1.0f);
        }
    } // namespace AvatarHelpers
} // namespace ConvaiEditor
