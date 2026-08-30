// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeCrypto.h"

#include "ProtocolValidation.h"

#include <bcrypt.h>

#include <limits>
#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        struct BcryptAlgorithmOwner
        {
            BCRYPT_ALG_HANDLE Value{};

            ~BcryptAlgorithmOwner()
            {
                if (Value)
                {
                    BCryptCloseAlgorithmProvider(Value, 0);
                }
            }
        };

        struct BcryptHashOwner
        {
            BCRYPT_HASH_HANDLE Value{};

            ~BcryptHashOwner()
            {
                if (Value)
                {
                    BCryptDestroyHash(Value);
                }
            }
        };

        void _writeHexByte(char*& destination, const uint8_t value) noexcept
        {
            constexpr char alphabet[]{ "0123456789abcdef" };
            *destination++ = alphabet[value >> 4];
            *destination++ = alphabet[value & 0x0f];
        }
    }

    std::optional<std::string> NewRuntimeUuid()
    {
        try
        {
            std::array<uint8_t, 16> bytes{};
            if (BCryptGenRandom(
                    nullptr,
                    bytes.data(),
                    static_cast<ULONG>(bytes.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            {
                return std::nullopt;
            }

            bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
            bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
            std::array<char, 36> text{};
            auto output = text.data();
            for (size_t index = 0; index < bytes.size(); ++index)
            {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                {
                    *output++ = '-';
                }
                _writeHexByte(output, bytes[index]);
            }
            if (output != text.data() + text.size())
            {
                return std::nullopt;
            }
            auto result = std::string{ text.data(), text.size() };
            return Microsoft::Terminal::TaskRuntimeProtocol::IsCanonicalProtocolUuid(
                       result) ?
                       std::optional{ std::move(result) } :
                       std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::array<uint8_t, 32>> RuntimeSha256(
        const std::span<const uint8_t> input)
    {
        try
        {
            if (input.size() > (std::numeric_limits<ULONG>::max)())
            {
                return std::nullopt;
            }
            BcryptAlgorithmOwner algorithm;
            // BCrypt retains the hash-object buffer until the hash handle is
            // destroyed, so the buffer must outlive BcryptHashOwner.
            std::vector<uint8_t> hashObject;
            BcryptHashOwner hash;
            std::array<uint8_t, 32> digest{};
            DWORD objectBytes{};
            DWORD copied{};
            auto status = BCryptOpenAlgorithmProvider(
                &algorithm.Value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0);
            if (status >= 0)
            {
                status = BCryptGetProperty(
                    algorithm.Value,
                    BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectBytes),
                    sizeof(objectBytes),
                    &copied,
                    0);
            }
            if (status >= 0)
            {
                hashObject.resize(objectBytes);
                status = BCryptCreateHash(
                    algorithm.Value,
                    &hash.Value,
                    hashObject.data(),
                    static_cast<ULONG>(hashObject.size()),
                    nullptr,
                    0,
                    0);
            }
            if (status >= 0 && !input.empty())
            {
                status = BCryptHashData(
                    hash.Value,
                    const_cast<PUCHAR>(input.data()),
                    static_cast<ULONG>(input.size()),
                    0);
            }
            if (status >= 0)
            {
                status = BCryptFinishHash(
                    hash.Value,
                    digest.data(),
                    static_cast<ULONG>(digest.size()),
                    0);
            }
            return status >= 0 ? std::optional{ digest } : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::string> RuntimeSha256Hex(
        const std::span<const uint8_t> input)
    {
        try
        {
            const auto digest = RuntimeSha256(input);
            if (!digest)
            {
                return std::nullopt;
            }
            std::array<char, 64> text{};
            auto output = text.data();
            for (const auto byte : *digest)
            {
                _writeHexByte(output, byte);
            }
            return std::string{ text.data(), text.size() };
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
