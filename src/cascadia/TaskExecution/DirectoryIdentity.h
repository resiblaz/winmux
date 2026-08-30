// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskExecution
{
    inline constexpr size_t DirectoryIdentityDigestCharacters{ 64 };
    // Leaves room for the longest \\?\UNC\ API prefix and a terminator.
    inline constexpr size_t MaximumTaskExecutionPathCharacters{ 32759 };

    enum class DirectoryIdentityPolicy : uint8_t
    {
        Direct,
        Worktree,
    };

    enum class DirectoryIdentityStatus
    {
        Succeeded,
        InvalidPath,
        MissingPath,
        NotDirectory,
        RootRejected,
        SystemRootRejected,
        UncRejected,
        ReparsePointRejected,
        DeviceRejected,
        OpenFailed,
        FinalPathUnavailable,
        IdentityUnavailable,
        AliasVerificationFailed,
        HashFailure,
        ResourceFailure,
    };

    class CanonicalDirectoryIdentity final
    {
    public:
        [[nodiscard]] const std::filesystem::path& CanonicalPath() const noexcept;
        [[nodiscard]] uint64_t VolumeSerialNumber() const noexcept;
        [[nodiscard]] const std::array<uint8_t, 16>& FileId() const noexcept;
        [[nodiscard]] const std::string& Digest() const noexcept;
        [[nodiscard]] DirectoryIdentityPolicy Policy() const noexcept;
        [[nodiscard]] bool IsUnc() const noexcept;
        [[nodiscard]] bool Valid() const noexcept;

    private:
        friend struct DirectoryIdentityResult;
        friend DirectoryIdentityResult ResolveCanonicalDirectoryIdentity(
            const std::filesystem::path&,
            DirectoryIdentityPolicy) noexcept;

        std::filesystem::path _canonicalPath;
        uint64_t _volumeSerialNumber{};
        std::array<uint8_t, 16> _fileId{};
        std::string _digest;
        DirectoryIdentityPolicy _policy{ DirectoryIdentityPolicy::Direct };
        bool _isUnc{};
    };

    struct DirectoryIdentityResult
    {
        DirectoryIdentityStatus Status{ DirectoryIdentityStatus::ResourceFailure };
        std::optional<CanonicalDirectoryIdentity> Identity;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == DirectoryIdentityStatus::Succeeded && Identity.has_value();
        }
    };

    [[nodiscard]] DirectoryIdentityResult ResolveCanonicalDirectoryIdentity(
        const std::filesystem::path& path,
        DirectoryIdentityPolicy policy) noexcept;
}
