// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    bool IsCanonicalProtocolUuid(const std::string_view value) noexcept
    {
        if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
            value[18] != '-' || value[23] != '-')
        {
            return false;
        }
        bool nonzero{};
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (index == 8 || index == 13 || index == 18 || index == 23)
            {
                continue;
            }
            const auto character = value[index];
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f')))
            {
                return false;
            }
            nonzero = nonzero || character != '0';
        }
        return nonzero;
    }

    bool IsValidRuntimeIdentity(const std::string_view value) noexcept
    {
        if (value.empty() || value.size() > 128)
        {
            return false;
        }
        return std::ranges::all_of(value, [](const char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '.' || character == '_' || character == '-' || character == ':';
        });
    }

    bool IsValidUtf8(const std::span<const uint8_t> value) noexcept
    {
        size_t index{};
        while (index < value.size())
        {
            const auto first = value[index];
            if (first <= 0x7f)
            {
                ++index;
                continue;
            }

            size_t continuationCount{};
            uint32_t codepoint{};
            if ((first & 0xe0) == 0xc0)
            {
                continuationCount = 1;
                codepoint = first & 0x1f;
            }
            else if ((first & 0xf0) == 0xe0)
            {
                continuationCount = 2;
                codepoint = first & 0x0f;
            }
            else if ((first & 0xf8) == 0xf0)
            {
                continuationCount = 3;
                codepoint = first & 0x07;
            }
            else
            {
                return false;
            }
            if (index + continuationCount >= value.size())
            {
                return false;
            }
            for (size_t offset = 1; offset <= continuationCount; ++offset)
            {
                const auto next = value[index + offset];
                if ((next & 0xc0) != 0x80)
                {
                    return false;
                }
                codepoint = (codepoint << 6) | (next & 0x3f);
            }

            const auto minimum = continuationCount == 1 ? 0x80u :
                                 continuationCount == 2 ? 0x800u : 0x10000u;
            if (codepoint < minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff))
            {
                return false;
            }
            index += continuationCount + 1;
        }
        return true;
    }

    bool IsValidUtf8(const std::string_view value) noexcept
    {
        return IsValidUtf8(std::span{
            reinterpret_cast<const uint8_t*>(value.data()),
            value.size()
        });
    }
}
