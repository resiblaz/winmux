// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskExecutionCrypto.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace Microsoft::Terminal::TaskExecution::details
{
    namespace
    {
        struct AlgorithmOwner
        {
            BCRYPT_ALG_HANDLE Value{};

            ~AlgorithmOwner()
            {
                if (Value)
                {
                    BCryptCloseAlgorithmProvider(Value, 0);
                }
            }
        };

        struct HashOwner
        {
            BCRYPT_HASH_HANDLE Value{};

            ~HashOwner()
            {
                if (Value)
                {
                    BCryptDestroyHash(Value);
                }
            }
        };
    }

    std::optional<std::array<uint8_t, 32>> TaskExecutionSha256(
        const std::span<const uint8_t> payload) noexcept
    {
        try
        {
            if (payload.size() > (std::numeric_limits<ULONG>::max)())
            {
                return std::nullopt;
            }
            AlgorithmOwner algorithm;
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                    &algorithm.Value,
                    BCRYPT_SHA256_ALGORITHM,
                    nullptr,
                    0)))
            {
                return std::nullopt;
            }

            DWORD objectBytes{};
            DWORD returned{};
            if (!BCRYPT_SUCCESS(BCryptGetProperty(
                    algorithm.Value,
                    BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectBytes),
                    sizeof(objectBytes),
                    &returned,
                    0)) ||
                returned != sizeof(objectBytes) || objectBytes == 0)
            {
                return std::nullopt;
            }

            std::vector<uint8_t> object(objectBytes);
            HashOwner hash;
            if (!BCRYPT_SUCCESS(BCryptCreateHash(
                    algorithm.Value,
                    &hash.Value,
                    object.data(),
                    static_cast<ULONG>(object.size()),
                    nullptr,
                    0,
                    0)) ||
                (!payload.empty() &&
                 !BCRYPT_SUCCESS(BCryptHashData(
                     hash.Value,
                     const_cast<PUCHAR>(payload.data()),
                     static_cast<ULONG>(payload.size()),
                     0))))
            {
                return std::nullopt;
            }

            std::array<uint8_t, 32> digest{};
            if (!BCRYPT_SUCCESS(BCryptFinishHash(
                    hash.Value,
                    digest.data(),
                    static_cast<ULONG>(digest.size()),
                    0)))
            {
                return std::nullopt;
            }
            return digest;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::string LowerHex(const std::span<const uint8_t> payload)
    {
        constexpr std::string_view digits{ "0123456789abcdef" };
        std::string result;
        result.reserve(payload.size() * 2);
        for (const auto byte : payload)
        {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    }
}
