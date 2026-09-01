// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeMarker.h"

#include "GitSourceSnapshot.h"
#include "TaskExecutionCrypto.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        constexpr std::array<uint8_t, 16> MarkerMagic{
            'W', 'T', 'A', 'S', 'K', '-', 'O', 'W', 'N', 'E', 'R', '-', 'V', '1', 0, 1
        };
        constexpr size_t MarkerChecksumBytes{ 32 };

        bool _lowerHex(const std::string_view value, const size_t length) noexcept
        {
            return value.size() == length &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        bool _objectId(const std::string_view value) noexcept
        {
            return _lowerHex(value, 40) || _lowerHex(value, 64);
        }

        bool _strictUtf8Path(const std::string_view value) noexcept
        {
            if (value.empty() || value.size() > MaximumGitSnapshotPathBytes ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return false;
            }
            const auto required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (required <= 0)
            {
                return false;
            }
            std::wstring decoded(static_cast<size_t>(required), L'\0');
            return MultiByteToWideChar(
                       CP_UTF8,
                       MB_ERR_INVALID_CHARS,
                       value.data(),
                       static_cast<int>(value.size()),
                       decoded.data(),
                       required) == required &&
                   std::ranges::none_of(decoded, [](const wchar_t character) {
                       return character < L' ' || character == 0x7f;
                   });
        }

        bool _validMarkerShape(const GitWorktreeMarker& marker) noexcept
        {
            return marker.FormatVersion == GitWorktreeMarkerFormatVersion &&
                   Microsoft::Terminal::TaskCore::IsCanonicalUuid(
                       marker.Task.Value()) &&
                   _lowerHex(marker.RepositoryIdentityDigest, 64) &&
                   marker.BranchRef ==
                       "refs/heads/wt-task/" + marker.Task.Value() &&
                   (marker.SnapshotDigest.empty() ||
                    _lowerHex(marker.SnapshotDigest, 64)) &&
                   (marker.BaselineCommit.empty() ||
                    _objectId(marker.BaselineCommit)) &&
                   Microsoft::Terminal::TaskCore::IsCanonicalUuid(
                       marker.OwnershipNonce) &&
                   _strictUtf8Path(marker.WorktreePath);
        }

        void _appendUint32(std::vector<uint8_t>& bytes, const uint32_t value)
        {
            for (size_t index = 0; index < sizeof(value); ++index)
            {
                bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
            }
        }

        void _appendText(
            std::vector<uint8_t>& bytes,
            const std::string_view value)
        {
            _appendUint32(bytes, static_cast<uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        }

        GitWorktreeMarkerResult _decodeFailure(std::string error)
        {
            GitWorktreeMarkerResult result;
            result.Status = GitWorktreeMarkerStatus::Malformed;
            result.SystemError = ERROR_INVALID_DATA;
            result.Error = std::move(error);
            return result;
        }
    }

    bool ValidateGitWorktreeMarker(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker) noexcept
    {
        try
        {
            const auto expectedMarkerPath = layout.StateRoot.CanonicalPath() /
                                            (std::wstring{
                                                 layout.Task.Value().begin(),
                                                 layout.Task.Value().end() } +
                                             L".owner-v1.bin");
            return layout.RuntimeRoot.Valid() && layout.ManagedRoot.Valid() &&
                   layout.StateRoot.Valid() && _validMarkerShape(marker) &&
                   marker.Task == layout.Task &&
                   marker.BranchRef == layout.BranchRef &&
                   marker.WorktreePath == layout.WorktreePathUtf8 &&
                   layout.MarkerPath == expectedMarkerPath;
        }
        catch (...)
        {
            return false;
        }
    }
}

namespace Microsoft::Terminal::TaskGit::details
{
    std::optional<std::vector<uint8_t>> EncodeGitWorktreeMarker(
        const GitWorktreeMarker& marker) noexcept
    {
        try
        {
            if (!_validMarkerShape(marker))
            {
                return std::nullopt;
            }
            std::vector<uint8_t> bytes;
            bytes.reserve(512 + marker.WorktreePath.size());
            bytes.insert(bytes.end(), MarkerMagic.begin(), MarkerMagic.end());
            _appendUint32(bytes, marker.FormatVersion);
            _appendText(bytes, marker.Task.Value());
            _appendText(bytes, marker.RepositoryIdentityDigest);
            _appendText(bytes, marker.BranchRef);
            _appendText(bytes, marker.SnapshotDigest);
            _appendText(bytes, marker.BaselineCommit);
            _appendText(bytes, marker.OwnershipNonce);
            _appendText(bytes, marker.WorktreePath);
            if (bytes.size() > MaximumGitWorktreeMarkerBytes - MarkerChecksumBytes)
            {
                return std::nullopt;
            }
            const auto checksum =
                Microsoft::Terminal::TaskExecution::details::TaskExecutionSha256(
                    bytes);
            if (!checksum)
            {
                return std::nullopt;
            }
            bytes.insert(bytes.end(), checksum->begin(), checksum->end());
            return bytes;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    GitWorktreeMarkerResult DecodeGitWorktreeMarker(
        const std::span<const uint8_t> bytes) noexcept
    {
        try
        {
            if (bytes.size() > MaximumGitWorktreeMarkerBytes ||
                bytes.size() < MarkerMagic.size() + sizeof(uint32_t) +
                                   7 * sizeof(uint32_t) + MarkerChecksumBytes ||
                !std::equal(
                    MarkerMagic.begin(),
                    MarkerMagic.end(),
                    bytes.begin()))
            {
                return _decodeFailure("Git Worktree marker header is invalid");
            }
            const auto payload = bytes.first(bytes.size() - MarkerChecksumBytes);
            const auto expected = bytes.last(MarkerChecksumBytes);
            const auto checksum =
                Microsoft::Terminal::TaskExecution::details::TaskExecutionSha256(
                    payload);
            if (!checksum || !std::equal(
                                 checksum->begin(),
                                 checksum->end(),
                                 expected.begin()))
            {
                return _decodeFailure("Git Worktree marker checksum is invalid");
            }

            size_t offset = MarkerMagic.size();
            const auto readUint32 = [&]() -> std::optional<uint32_t> {
                if (offset > payload.size() - sizeof(uint32_t))
                {
                    return std::nullopt;
                }
                uint32_t value{};
                for (size_t index = 0; index < sizeof(value); ++index)
                {
                    value |= static_cast<uint32_t>(payload[offset++]) <<
                             (index * 8);
                }
                return value;
            };
            const auto readText = [&]() -> std::optional<std::string> {
                const auto length = readUint32();
                if (!length || *length > MaximumGitSnapshotPathBytes ||
                    offset > payload.size() ||
                    *length > payload.size() - offset)
                {
                    return std::nullopt;
                }
                std::string value{
                    reinterpret_cast<const char*>(payload.data() + offset),
                    *length
                };
                offset += *length;
                return value;
            };

            const auto version = readUint32();
            const auto task = readText();
            const auto repository = readText();
            const auto branch = readText();
            const auto snapshot = readText();
            const auto baseline = readText();
            const auto nonce = readText();
            const auto worktreePath = readText();
            if (!version || !task || !repository || !branch || !snapshot ||
                !baseline || !nonce || !worktreePath || offset != payload.size())
            {
                return _decodeFailure("Git Worktree marker payload is invalid");
            }

            GitWorktreeMarker marker;
            marker.FormatVersion = *version;
            marker.Task = Microsoft::Terminal::TaskCore::TaskId{ *task };
            marker.RepositoryIdentityDigest = *repository;
            marker.BranchRef = *branch;
            marker.SnapshotDigest = *snapshot;
            marker.BaselineCommit = *baseline;
            marker.OwnershipNonce = *nonce;
            marker.WorktreePath = *worktreePath;
            if (!_validMarkerShape(marker))
            {
                return _decodeFailure("Git Worktree marker fields are invalid");
            }

            GitWorktreeMarkerResult result;
            result.Status = GitWorktreeMarkerStatus::Succeeded;
            result.Marker = std::move(marker);
            return result;
        }
        catch (...)
        {
            GitWorktreeMarkerResult result;
            result.Status = GitWorktreeMarkerStatus::ResourceFailure;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Git Worktree marker decoding exhausted local resources";
            return result;
        }
    }
}
