// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientInterfaces.h"

#include <objbase.h>

#include <iterator>

#pragma comment(lib, "ole32.lib")

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        constexpr uint32_t InitialBackoffMilliseconds{ 100 };
        constexpr uint32_t MaximumBackoffMilliseconds{ 5000 };
        constexpr uint32_t MaximumDoublingFailure{ 6 };

        void _writeHex(char*& destination, const uint64_t value, const size_t digits) noexcept
        {
            constexpr char alphabet[]{ "0123456789abcdef" };
            for (size_t index = 0; index < digits; ++index)
            {
                const auto shift = (digits - index - 1) * 4;
                const auto nibble = static_cast<size_t>((value >> shift) & 0xf);
                *destination++ = alphabet[nibble];
            }
        }

        class RuntimeClientRequestIdSource final : public IRuntimeClientRequestIdSource
        {
        public:
            [[nodiscard]] std::optional<std::string> Next() noexcept override
            {
                try
                {
                    GUID value{};
                    if (FAILED(CoCreateGuid(&value)) || InlineIsEqualGUID(value, GUID_NULL))
                    {
                        return std::nullopt;
                    }

                    char text[36]{};
                    auto output = text;
                    _writeHex(output, value.Data1, 8);
                    *output++ = '-';
                    _writeHex(output, value.Data2, 4);
                    *output++ = '-';
                    _writeHex(output, value.Data3, 4);
                    *output++ = '-';
                    _writeHex(output, value.Data4[0], 2);
                    _writeHex(output, value.Data4[1], 2);
                    *output++ = '-';
                    for (size_t index = 2; index < std::size(value.Data4); ++index)
                    {
                        _writeHex(output, value.Data4[index], 2);
                    }
                    if (output != text + std::size(text))
                    {
                        return std::nullopt;
                    }
                    return std::string{ text, std::size(text) };
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }
        };

        class RuntimeClientBackoffPolicy final : public IRuntimeClientBackoffPolicy
        {
        public:
            [[nodiscard]] uint32_t DelayForFailure(const uint32_t consecutiveFailures) noexcept override
            {
                if (consecutiveFailures == 0)
                {
                    return 0;
                }
                if (consecutiveFailures > MaximumDoublingFailure)
                {
                    return MaximumBackoffMilliseconds;
                }
                return InitialBackoffMilliseconds << (consecutiveFailures - 1);
            }
        };
    }

    std::shared_ptr<IRuntimeClientRequestIdSource> MakeRuntimeClientRequestIdSource()
    {
        try
        {
            return std::make_shared<RuntimeClientRequestIdSource>();
        }
        catch (...)
        {
            return {};
        }
    }

    std::shared_ptr<IRuntimeClientBackoffPolicy> MakeRuntimeClientBackoffPolicy()
    {
        try
        {
            return std::make_shared<RuntimeClientBackoffPolicy>();
        }
        catch (...)
        {
            return {};
        }
    }
}
