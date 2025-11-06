/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * SlateBinding.cpp
 *
 * Implementation of Slate binding utilities.
 */

#include "MVVM/SlateBinding.h"

namespace ConvaiEditor
{
    namespace Binding
    {
        namespace Internal
        {
            FText ConvertValueToText(const FString &Value)
            {
                return FText::FromString(Value);
            }

            bool TryParseTextToValue(const FText &Text, FString &OutValue)
            {
                OutValue = Text.ToString();
                return true;
            }

            bool TryParseTextToValue(const FText &Text, int32 &OutValue)
            {
                return LexTryParseString(OutValue, *Text.ToString());
            }

            bool TryParseTextToValue(const FText &Text, float &OutValue)
            {
                return LexTryParseString(OutValue, *Text.ToString());
            }

            bool TryParseTextToValue(const FText &Text, double &OutValue)
            {
                return LexTryParseString(OutValue, *Text.ToString());
            }

            bool TryParseTextToValue(const FText &Text, bool &OutValue)
            {
                const FString TextStr = Text.ToString().ToLower();
                if (TextStr == TEXT("true") || TextStr == TEXT("1") || TextStr == TEXT("yes"))
                {
                    OutValue = true;
                    return true;
                }
                else if (TextStr == TEXT("false") || TextStr == TEXT("0") || TextStr == TEXT("no"))
                {
                    OutValue = false;
                    return true;
                }

                return LexTryParseString(OutValue, *Text.ToString());
            }
        }
    }
}
