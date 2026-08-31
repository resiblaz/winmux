// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitFileHash.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace Microsoft::Terminal::TaskGit::details
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

        GitFileHashResult _failure(
            const GitFileHashStatus status,
            const DWORD systemError)
        {
            GitFileHashResult result;
            result.Status = status;
            result.SystemError = systemError;
            return result;
        }
    }

    GitFileHashResult HashOpenGitFileSha256(
        const HANDLE file,
        const uint64_t expectedBytes,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            AlgorithmOwner algorithm;
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                    &algorithm.Value,
                    BCRYPT_SHA256_ALGORITHM,
                    nullptr,
                    0)))
            {
                return _failure(
                    GitFileHashStatus::ReadFailure,
                    ERROR_INVALID_DATA);
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
                return _failure(
                    GitFileHashStatus::ReadFailure,
                    ERROR_INVALID_DATA);
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
                    0)))
            {
                return _failure(
                    GitFileHashStatus::ReadFailure,
                    ERROR_INVALID_DATA);
            }

            uint64_t bytesReadTotal{};
            std::array<uint8_t, 64 * 1024> buffer{};
            for (;;)
            {
                const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
                if (cancelled == WAIT_OBJECT_0)
                {
                    return _failure(
                        GitFileHashStatus::Cancelled,
                        ERROR_OPERATION_ABORTED);
                }
                if (cancelled != WAIT_TIMEOUT)
                {
                    return _failure(
                        GitFileHashStatus::ReadFailure,
                        cancelled == WAIT_FAILED ?
                            GetLastError() :
                            ERROR_INVALID_HANDLE);
                }
                DWORD bytesRead{};
                if (!ReadFile(
                        file,
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &bytesRead,
                        nullptr))
                {
                    return _failure(
                        GitFileHashStatus::ReadFailure,
                        GetLastError());
                }
                if (bytesRead == 0)
                {
                    break;
                }
                bytesReadTotal += bytesRead;
                if (bytesReadTotal > expectedBytes ||
                    !BCRYPT_SUCCESS(BCryptHashData(
                        hash.Value,
                        buffer.data(),
                        bytesRead,
                        0)))
                {
                    return _failure(
                        GitFileHashStatus::ReadFailure,
                        ERROR_INVALID_DATA);
                }
            }
            if (bytesReadTotal != expectedBytes)
            {
                return _failure(
                    GitFileHashStatus::ReadFailure,
                    ERROR_FILE_INVALID);
            }
            std::array<uint8_t, 32> digest{};
            if (!BCRYPT_SUCCESS(BCryptFinishHash(
                    hash.Value,
                    digest.data(),
                    static_cast<ULONG>(digest.size()),
                    0)))
            {
                return _failure(
                    GitFileHashStatus::ReadFailure,
                    ERROR_INVALID_DATA);
            }

            constexpr std::string_view digits{ "0123456789abcdef" };
            GitFileHashResult result;
            result.Status = GitFileHashStatus::Succeeded;
            result.Digest.reserve(digest.size() * 2);
            for (const auto byte : digest)
            {
                result.Digest.push_back(digits[byte >> 4]);
                result.Digest.push_back(digits[byte & 0x0f]);
            }
            return result;
        }
        catch (...)
        {
            return _failure(
                GitFileHashStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY);
        }
    }
}
