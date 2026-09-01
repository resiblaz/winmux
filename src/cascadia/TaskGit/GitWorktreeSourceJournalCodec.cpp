// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeSourceJournal.h"

#include "TaskExecutionCrypto.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using Microsoft::Terminal::TaskExecution::GitObjectFormat;

        constexpr std::array<uint8_t, 16> JournalMagic{
            'W', 'T', 'A', 'S', 'K', '-', 'S', 'O', 'U', 'R', 'C', 'E', '-', 'V', '1', 1
        };
        constexpr size_t JournalChecksumBytes{ 32 };

        bool _lowerHex(const std::string_view value, const size_t length) noexcept
        {
            return value.size() == length &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        bool _objectId(
            const std::string_view value,
            const GitObjectFormat format,
            const bool allowEmpty = false) noexcept
        {
            return (allowEmpty && value.empty()) ||
                   _lowerHex(
                       value,
                       format == GitObjectFormat::Sha1 ? 40 : 64);
        }

        std::optional<std::wstring> _strictWide(const std::string_view value)
        {
            if (value.empty() || value.size() > MaximumGitSnapshotPathBytes ||
                value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::nullopt;
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
                return std::nullopt;
            }
            std::wstring decoded(static_cast<size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    decoded.data(),
                    required) != required ||
                std::ranges::any_of(decoded, [](const wchar_t character) {
                    return character < L' ' || character == 0x7f;
                }))
            {
                return std::nullopt;
            }
            return decoded;
        }

        bool _absolutePath(const std::string_view value)
        {
            const auto decoded = _strictWide(value);
            if (!decoded)
            {
                return false;
            }
            const std::filesystem::path path{ *decoded };
            return path.is_absolute() && path.has_root_path();
        }

        bool _relativePath(const std::string_view value)
        {
            const auto decoded = _strictWide(value);
            if (!decoded)
            {
                return false;
            }
            const std::filesystem::path path{ *decoded };
            return !path.empty() && !path.is_absolute() && !path.has_root_path() &&
                   std::ranges::none_of(path, [](const auto& component) {
                       return component.empty() || component == L"." ||
                              component == L"..";
                   });
        }

        bool _validJournalShape(const GitWorktreeSourceJournal& journal)
        {
            if (journal.FormatVersion != GitWorktreeSourceJournalFormatVersion ||
                !Microsoft::Terminal::TaskCore::IsCanonicalUuid(
                    journal.Task.Value()) ||
                !Microsoft::Terminal::TaskCore::IsCanonicalUuid(
                    journal.OwnershipNonce) ||
                !_lowerHex(journal.RepositoryIdentityDigest, 64) ||
                !_absolutePath(journal.SourceWorktreePath) ||
                (journal.ObjectFormat != GitObjectFormat::Sha1 &&
                 journal.ObjectFormat != GitObjectFormat::Sha256) ||
                !_objectId(journal.SourceHead, journal.ObjectFormat) ||
                journal.SourceReference.size() > 4096 ||
                (!journal.SourceReference.empty() &&
                 (!std::string_view{ journal.SourceReference }.starts_with(
                      "refs/heads/") ||
                  !_strictWide(journal.SourceReference))) ||
                !_objectId(
                    journal.TrackedSnapshotCommit,
                    journal.ObjectFormat,
                    true) ||
                !_objectId(journal.WorktreeTree, journal.ObjectFormat) ||
                !_objectId(journal.IndexTree, journal.ObjectFormat) ||
                !_objectId(
                    journal.StashReference,
                    journal.ObjectFormat,
                    true) ||
                !_lowerHex(journal.StatusDigest, 64) ||
                !_lowerHex(journal.SnapshotDigest, 64) ||
                journal.UntrackedFiles.size() > MaximumGitSnapshotUntrackedFiles ||
                journal.UntrackedBytes > MaximumGitSnapshotUntrackedBytes)
            {
                return false;
            }

            uint64_t total{};
            std::vector<std::string_view> paths;
            paths.reserve(journal.UntrackedFiles.size());
            for (const auto& file : journal.UntrackedFiles)
            {
                if (file.Type != GitSourceFileType::RegularFile ||
                    !_relativePath(file.RelativePath) ||
                    !_lowerHex(file.Sha256, 64) ||
                    file.Size > MaximumGitSnapshotUntrackedBytes - total ||
                    std::ranges::find(paths, file.RelativePath) != paths.end())
                {
                    return false;
                }
                total += file.Size;
                paths.emplace_back(file.RelativePath);
            }
            return total == journal.UntrackedBytes;
        }

        struct Writer
        {
            std::vector<uint8_t> Bytes;

            void Uint32(const uint32_t value)
            {
                for (size_t index = 0; index < sizeof(value); ++index)
                {
                    Bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
                }
            }

            void Uint64(const uint64_t value)
            {
                for (size_t index = 0; index < sizeof(value); ++index)
                {
                    Bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
                }
            }

            void Text(const std::string_view value)
            {
                Uint32(static_cast<uint32_t>(value.size()));
                Bytes.insert(Bytes.end(), value.begin(), value.end());
            }
        };

        struct Reader
        {
            std::span<const uint8_t> Bytes;
            size_t Offset{};

            std::optional<uint32_t> Uint32()
            {
                if (Offset > Bytes.size() ||
                    sizeof(uint32_t) > Bytes.size() - Offset)
                {
                    return std::nullopt;
                }
                uint32_t value{};
                for (size_t index = 0; index < sizeof(value); ++index)
                {
                    value |= static_cast<uint32_t>(Bytes[Offset++]) <<
                             (index * 8);
                }
                return value;
            }

            std::optional<uint64_t> Uint64()
            {
                if (Offset > Bytes.size() ||
                    sizeof(uint64_t) > Bytes.size() - Offset)
                {
                    return std::nullopt;
                }
                uint64_t value{};
                for (size_t index = 0; index < sizeof(value); ++index)
                {
                    value |= static_cast<uint64_t>(Bytes[Offset++]) <<
                             (index * 8);
                }
                return value;
            }

            std::optional<std::string> Text(const size_t maximumBytes)
            {
                const auto length = Uint32();
                if (!length || *length > maximumBytes || Offset > Bytes.size() ||
                    *length > Bytes.size() - Offset)
                {
                    return std::nullopt;
                }
                std::string value{
                    reinterpret_cast<const char*>(Bytes.data() + Offset),
                    *length
                };
                Offset += *length;
                return value;
            }
        };

        GitWorktreeSourceJournalResult _malformed(std::string error)
        {
            GitWorktreeSourceJournalResult result;
            result.Status = GitWorktreeSourceJournalStatus::Malformed;
            result.SystemError = ERROR_INVALID_DATA;
            result.Error = std::move(error);
            return result;
        }
    }
}

namespace Microsoft::Terminal::TaskGit::details
{
    bool ValidateGitWorktreeSourceJournalShape(
        const GitWorktreeSourceJournal& journal) noexcept
    {
        try
        {
            return _validJournalShape(journal);
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<std::vector<uint8_t>> EncodeGitWorktreeSourceJournal(
        const GitWorktreeSourceJournal& journal) noexcept
    {
        try
        {
            if (!_validJournalShape(journal))
            {
                return std::nullopt;
            }
            Writer writer;
            writer.Bytes.reserve(1024 + journal.UntrackedFiles.size() * 128);
            writer.Bytes.insert(
                writer.Bytes.end(),
                JournalMagic.begin(),
                JournalMagic.end());
            writer.Uint32(journal.FormatVersion);
            writer.Uint32(
                journal.ObjectFormat == GitObjectFormat::Sha1 ? 1 : 2);
            writer.Uint64(journal.UntrackedBytes);
            writer.Text(journal.Task.Value());
            writer.Text(journal.OwnershipNonce);
            writer.Text(journal.RepositoryIdentityDigest);
            writer.Text(journal.SourceWorktreePath);
            writer.Text(journal.SourceHead);
            writer.Text(journal.SourceReference);
            writer.Text(journal.TrackedSnapshotCommit);
            writer.Text(journal.WorktreeTree);
            writer.Text(journal.IndexTree);
            writer.Text(journal.StashReference);
            writer.Text(journal.StatusDigest);
            writer.Text(journal.SnapshotDigest);
            writer.Uint32(static_cast<uint32_t>(journal.UntrackedFiles.size()));
            for (const auto& file : journal.UntrackedFiles)
            {
                writer.Uint32(static_cast<uint32_t>(file.Type));
                writer.Uint64(file.Size);
                writer.Text(file.RelativePath);
                writer.Text(file.Sha256);
            }
            if (writer.Bytes.size() >
                MaximumGitWorktreeSourceJournalBytes - JournalChecksumBytes)
            {
                return std::nullopt;
            }
            const auto checksum =
                Microsoft::Terminal::TaskExecution::details::TaskExecutionSha256(
                    writer.Bytes);
            if (!checksum)
            {
                return std::nullopt;
            }
            writer.Bytes.insert(
                writer.Bytes.end(),
                checksum->begin(),
                checksum->end());
            return std::move(writer.Bytes);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    GitWorktreeSourceJournalResult DecodeGitWorktreeSourceJournal(
        const std::span<const uint8_t> bytes) noexcept
    {
        try
        {
            if (bytes.size() > MaximumGitWorktreeSourceJournalBytes ||
                bytes.size() < JournalMagic.size() + 2 * sizeof(uint32_t) +
                                   sizeof(uint64_t) + 13 * sizeof(uint32_t) +
                                   JournalChecksumBytes ||
                !std::equal(
                    JournalMagic.begin(),
                    JournalMagic.end(),
                    bytes.begin()))
            {
                return _malformed("Git Worktree source journal header is invalid");
            }
            const auto payload = bytes.first(bytes.size() - JournalChecksumBytes);
            const auto checksumBytes = bytes.last(JournalChecksumBytes);
            const auto checksum =
                Microsoft::Terminal::TaskExecution::details::TaskExecutionSha256(
                    payload);
            if (!checksum || !std::equal(
                                 checksum->begin(),
                                 checksum->end(),
                                 checksumBytes.begin()))
            {
                return _malformed("Git Worktree source journal checksum is invalid");
            }

            Reader reader{ payload, JournalMagic.size() };
            const auto version = reader.Uint32();
            const auto objectFormat = reader.Uint32();
            const auto untrackedBytes = reader.Uint64();
            const auto task = reader.Text(36);
            const auto nonce = reader.Text(36);
            const auto repository = reader.Text(64);
            const auto sourcePath = reader.Text(MaximumGitSnapshotPathBytes);
            const auto head = reader.Text(64);
            const auto sourceReference = reader.Text(4096);
            const auto trackedCommit = reader.Text(64);
            const auto worktreeTree = reader.Text(64);
            const auto indexTree = reader.Text(64);
            const auto stash = reader.Text(64);
            const auto statusDigest = reader.Text(64);
            const auto snapshotDigest = reader.Text(64);
            const auto fileCount = reader.Uint32();
            if (!version || !objectFormat || !untrackedBytes || !task || !nonce ||
                !repository || !sourcePath || !head || !sourceReference ||
                !trackedCommit || !worktreeTree || !indexTree || !stash ||
                !statusDigest || !snapshotDigest || !fileCount ||
                (*objectFormat != 1 && *objectFormat != 2) ||
                *fileCount > MaximumGitSnapshotUntrackedFiles)
            {
                return _malformed("Git Worktree source journal payload is invalid");
            }

            GitWorktreeSourceJournal journal;
            journal.FormatVersion = *version;
            journal.ObjectFormat = *objectFormat == 1 ?
                                       GitObjectFormat::Sha1 :
                                       GitObjectFormat::Sha256;
            journal.UntrackedBytes = *untrackedBytes;
            journal.Task = Microsoft::Terminal::TaskCore::TaskId{ *task };
            journal.OwnershipNonce = *nonce;
            journal.RepositoryIdentityDigest = *repository;
            journal.SourceWorktreePath = *sourcePath;
            journal.SourceHead = *head;
            journal.SourceReference = *sourceReference;
            journal.TrackedSnapshotCommit = *trackedCommit;
            journal.WorktreeTree = *worktreeTree;
            journal.IndexTree = *indexTree;
            journal.StashReference = *stash;
            journal.StatusDigest = *statusDigest;
            journal.SnapshotDigest = *snapshotDigest;
            journal.UntrackedFiles.reserve(*fileCount);
            for (uint32_t index = 0; index < *fileCount; ++index)
            {
                const auto type = reader.Uint32();
                const auto size = reader.Uint64();
                const auto path = reader.Text(MaximumGitSnapshotPathBytes);
                const auto digest = reader.Text(64);
                if (!type || !size || !path || !digest)
                {
                    return _malformed(
                        "Git Worktree source journal manifest is invalid");
                }
                journal.UntrackedFiles.push_back(GitSourceFileRecord{
                    *path,
                    static_cast<GitSourceFileType>(*type),
                    *size,
                    *digest,
                });
            }
            if (reader.Offset != payload.size() || !_validJournalShape(journal))
            {
                return _malformed("Git Worktree source journal fields are invalid");
            }

            GitWorktreeSourceJournalResult result;
            result.Status = GitWorktreeSourceJournalStatus::Succeeded;
            result.Journal = std::move(journal);
            return result;
        }
        catch (...)
        {
            GitWorktreeSourceJournalResult result;
            result.Status = GitWorktreeSourceJournalStatus::ResourceFailure;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error =
                "Git Worktree source journal decoding exhausted local resources";
            return result;
        }
    }
}
