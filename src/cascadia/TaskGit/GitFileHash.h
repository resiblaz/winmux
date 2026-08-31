// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace Microsoft::Terminal::TaskGit::details
{
    enum class GitFileHashStatus
    {
        Succeeded,
        Cancelled,
        ReadFailure,
        ResourceFailure,
    };

    struct GitFileHashResult
    {
        GitFileHashStatus Status{ GitFileHashStatus::ReadFailure };
        std::string Digest;
        DWORD SystemError{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == GitFileHashStatus::Succeeded &&
                   Digest.size() == 64;
        }
    };

    [[nodiscard]] GitFileHashResult HashOpenGitFileSha256(
        HANDLE file,
        uint64_t expectedBytes,
        HANDLE cancellationEvent) noexcept;
}
