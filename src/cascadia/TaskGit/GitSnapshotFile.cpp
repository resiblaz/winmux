// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSnapshotFile.h"
#include "GitFileHash.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        struct HandleOwner
        {
            HANDLE Value{};
            ~HandleOwner()
            {
                if (Value && Value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(Value);
                }
            }
        };

        GitSnapshotFileResult _failure(
            const GitSnapshotFileStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitSnapshotFileResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
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
            std::wstring result(static_cast<size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required) != required ||
                std::ranges::any_of(result, [](const wchar_t character) {
                    return character < L' ' || character == 0x7f;
                }))
            {
                return std::nullopt;
            }
            return result;
        }

        std::optional<std::filesystem::path> _relativePath(
            const std::string_view value)
        {
            const auto decoded = _strictWide(value);
            if (!decoded)
            {
                return std::nullopt;
            }
            const std::filesystem::path path{ *decoded };
            if (path.empty() || path.is_absolute() || path.has_root_path() ||
                std::ranges::any_of(path, [](const auto& component) {
                    return component.empty() || component == L"." ||
                           component == L"..";
                }))
            {
                return std::nullopt;
            }
            return path.lexically_normal();
        }

        std::filesystem::path _extendedPath(const std::filesystem::path& path)
        {
            const auto& value = path.native();
            return value.starts_with(L"\\\\") ?
                       std::filesystem::path{ LR"(\\?\UNC\)" + value.substr(2) } :
                       std::filesystem::path{ LR"(\\?\)" + value };
        }

        bool _samePath(
            const std::filesystem::path& left,
            const std::filesystem::path& right) noexcept
        {
            try
            {
                return CompareStringOrdinal(
                           left.native().c_str(),
                           static_cast<int>(left.native().size()),
                           right.native().c_str(),
                           static_cast<int>(right.native().size()),
                           TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        std::optional<std::filesystem::path> _finalPath(
            const HANDLE handle,
            DWORD& systemError)
        {
            constexpr DWORD flags{ FILE_NAME_NORMALIZED | VOLUME_NAME_DOS };
            const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
            if (required == 0 ||
                required > Microsoft::Terminal::TaskExecution::MaximumTaskExecutionPathCharacters + 8)
            {
                systemError = required == 0 ?
                                  GetLastError() :
                                  ERROR_FILENAME_EXCED_RANGE;
                return std::nullopt;
            }
            std::wstring value(static_cast<size_t>(required) + 1, L'\0');
            const auto written = GetFinalPathNameByHandleW(
                handle,
                value.data(),
                static_cast<DWORD>(value.size()),
                flags);
            if (written == 0 || written >= value.size())
            {
                systemError = GetLastError();
                return std::nullopt;
            }
            value.resize(written);
            constexpr std::wstring_view uncPrefix{ LR"(\\?\UNC\)" };
            constexpr std::wstring_view prefix{ LR"(\\?\)" };
            if (value.starts_with(uncPrefix))
            {
                value = L"\\\\" + value.substr(uncPrefix.size());
            }
            else if (value.starts_with(prefix))
            {
                value.erase(0, prefix.size());
            }
            else
            {
                systemError = ERROR_BAD_PATHNAME;
                return std::nullopt;
            }
            return std::filesystem::path{ value }.lexically_normal();
        }

        bool _sameIdentity(
            const FILE_ID_INFO& left,
            const FILE_ID_INFO& right) noexcept
        {
            return left.VolumeSerialNumber == right.VolumeSerialNumber &&
                   std::equal(
                       std::begin(left.FileId.Identifier),
                       std::end(left.FileId.Identifier),
                       std::begin(right.FileId.Identifier));
        }

        bool _sameBasic(
            const FILE_BASIC_INFO& left,
            const FILE_BASIC_INFO& right) noexcept
        {
            return left.CreationTime.QuadPart == right.CreationTime.QuadPart &&
                   left.LastWriteTime.QuadPart == right.LastWriteTime.QuadPart &&
                   left.ChangeTime.QuadPart == right.ChangeTime.QuadPart &&
                   left.FileAttributes == right.FileAttributes;
        }

    }

    namespace details
    {
        GitSnapshotFileStatus ClassifyGitSnapshotFileAttributes(
            const DWORD attributes) noexcept
        {
            return (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE)) == 0 ?
                       GitSnapshotFileStatus::Succeeded :
                       GitSnapshotFileStatus::UnsafeAttributes;
        }
    }

    GitSnapshotFileResult HashGitSnapshotFile(
        const Microsoft::Terminal::TaskExecution::CanonicalDirectoryIdentity& root,
        const std::string_view relativePath,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            if (!root.Valid() || !cancellationEvent ||
                cancellationEvent == INVALID_HANDLE_VALUE ||
                root.Policy() != Microsoft::Terminal::TaskExecution::DirectoryIdentityPolicy::Worktree)
            {
                return _failure(
                    GitSnapshotFileStatus::InvalidPath,
                    ERROR_INVALID_PARAMETER,
                    "Git snapshot root identity is invalid");
            }
            const auto relative = _relativePath(relativePath);
            if (!relative)
            {
                return _failure(
                    GitSnapshotFileStatus::InvalidPath,
                    ERROR_BAD_PATHNAME,
                    "Git snapshot relative path is invalid");
            }

            auto current = root.CanonicalPath();
            const auto parent = relative->parent_path();
            for (const auto& component : parent)
            {
                current /= component;
                const auto attributes = GetFileAttributesW(
                    _extendedPath(current).c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    return _failure(
                        GitSnapshotFileStatus::Missing,
                        GetLastError(),
                        "Git snapshot parent component disappeared");
                }
                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                    (attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                   FILE_ATTRIBUTE_DEVICE)) != 0)
                {
                    return _failure(
                        GitSnapshotFileStatus::UnsafeAttributes,
                        ERROR_ACCESS_DENIED,
                        "Git snapshot parent component is unsafe");
                }
            }

            const auto candidate = (root.CanonicalPath() / *relative).lexically_normal();
            const auto attributes = GetFileAttributesW(
                _extendedPath(candidate).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                return _failure(
                    GitSnapshotFileStatus::Missing,
                    GetLastError(),
                    "Git snapshot file disappeared");
            }
            if (details::ClassifyGitSnapshotFileAttributes(attributes) !=
                GitSnapshotFileStatus::Succeeded)
            {
                return _failure(
                    GitSnapshotFileStatus::UnsafeAttributes,
                    ERROR_ACCESS_DENIED,
                    "Git snapshot file is not a direct regular file");
            }

            HandleOwner file;
            file.Value = CreateFileW(
                _extendedPath(candidate).c_str(),
                GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (!file.Value || file.Value == INVALID_HANDLE_VALUE)
            {
                file.Value = nullptr;
                return _failure(
                    GitSnapshotFileStatus::Missing,
                    GetLastError(),
                    "Git snapshot file could not be opened exclusively for reading");
            }

            FILE_ATTRIBUTE_TAG_INFO tag{};
            FILE_STANDARD_INFO beforeSize{};
            FILE_BASIC_INFO beforeBasic{};
            FILE_ID_INFO beforeIdentity{};
            if (!GetFileInformationByHandleEx(
                    file.Value,
                    FileAttributeTagInfo,
                    &tag,
                    sizeof(tag)) ||
                details::ClassifyGitSnapshotFileAttributes(tag.FileAttributes) !=
                    GitSnapshotFileStatus::Succeeded ||
                !GetFileInformationByHandleEx(
                    file.Value,
                    FileStandardInfo,
                    &beforeSize,
                    sizeof(beforeSize)) ||
                !GetFileInformationByHandleEx(
                    file.Value,
                    FileBasicInfo,
                    &beforeBasic,
                    sizeof(beforeBasic)) ||
                !GetFileInformationByHandleEx(
                    file.Value,
                    FileIdInfo,
                    &beforeIdentity,
                    sizeof(beforeIdentity)))
            {
                return _failure(
                    GitSnapshotFileStatus::UnsafeAttributes,
                    GetLastError(),
                    "Git snapshot file identity is unavailable");
            }
            if (beforeSize.Directory || beforeSize.EndOfFile.QuadPart < 0)
            {
                return _failure(
                    GitSnapshotFileStatus::UnsafeAttributes,
                    ERROR_FILE_INVALID,
                    "Git snapshot entry is not a regular file");
            }
            const auto size = static_cast<uint64_t>(beforeSize.EndOfFile.QuadPart);
            if (size > MaximumGitSnapshotUntrackedBytes)
            {
                return _failure(
                    GitSnapshotFileStatus::TooLarge,
                    ERROR_FILE_TOO_LARGE,
                    "Git snapshot file exceeds the untracked-byte limit");
            }

            DWORD error{};
            const auto finalPath = _finalPath(file.Value, error);
            if (!finalPath || !_samePath(*finalPath, candidate))
            {
                return _failure(
                    GitSnapshotFileStatus::IdentityMismatch,
                    error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error,
                    "Git snapshot file final path escaped its manifest identity");
            }
            const auto digest = details::HashOpenGitFileSha256(
                file.Value,
                size,
                cancellationEvent);
            if (!digest.Succeeded())
            {
                return _failure(
                    digest.Status == details::GitFileHashStatus::Cancelled ?
                        GitSnapshotFileStatus::Cancelled :
                    digest.Status == details::GitFileHashStatus::ResourceFailure ?
                        GitSnapshotFileStatus::ResourceFailure :
                        GitSnapshotFileStatus::ReadFailure,
                    digest.SystemError,
                    "Git snapshot file could not be hashed completely");
            }

            FILE_STANDARD_INFO afterSize{};
            FILE_BASIC_INFO afterBasic{};
            FILE_ID_INFO afterIdentity{};
            if (!GetFileInformationByHandleEx(
                    file.Value,
                    FileStandardInfo,
                    &afterSize,
                    sizeof(afterSize)) ||
                !GetFileInformationByHandleEx(
                    file.Value,
                    FileBasicInfo,
                    &afterBasic,
                    sizeof(afterBasic)) ||
                !GetFileInformationByHandleEx(
                    file.Value,
                    FileIdInfo,
                    &afterIdentity,
                    sizeof(afterIdentity)) ||
                beforeSize.EndOfFile.QuadPart != afterSize.EndOfFile.QuadPart ||
                !_sameBasic(beforeBasic, afterBasic) ||
                !_sameIdentity(beforeIdentity, afterIdentity))
            {
                return _failure(
                    GitSnapshotFileStatus::SourceChanged,
                    ERROR_FILE_INVALID,
                    "Git snapshot file changed while hashing");
            }

            GitSnapshotFileResult result;
            result.Status = GitSnapshotFileStatus::Succeeded;
            result.File = GitSourceFileRecord{
                std::string{ relativePath },
                GitSourceFileType::RegularFile,
                size,
                digest.Digest,
            };
            return result;
        }
        catch (...)
        {
            return _failure(
                GitSnapshotFileStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git snapshot file hashing exhausted local resources");
        }
    }
}
