// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskIdentifiers.h"

namespace Microsoft::Terminal::TaskCore
{
    bool IsCanonicalUuid(const std::string_view value) noexcept
    {
        if (value.size() != 36 ||
            value[8] != '-' ||
            value[13] != '-' ||
            value[18] != '-' ||
            value[23] != '-')
        {
            return false;
        }

        bool hasNonZeroDigit{};
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (index == 8 || index == 13 || index == 18 || index == 23)
            {
                continue;
            }

            const auto character = value[index];
            const auto isDigit = character >= '0' && character <= '9';
            const auto isLowerHex = character >= 'a' && character <= 'f';
            if (!isDigit && !isLowerHex)
            {
                return false;
            }
            hasNonZeroDigit = hasNonZeroDigit || character != '0';
        }
        return hasNonZeroDigit;
    }
}
