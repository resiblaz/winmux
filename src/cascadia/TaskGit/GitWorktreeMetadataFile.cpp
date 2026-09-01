// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeMetadataFile.h"

#include "DirectoryIdentity.h"

namespace Microsoft::Terminal::TaskGit::details
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

        GitWorktreeMetadataFileResult _failure(
            const GitWorktreeMetadataFileStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreeMetadataFileResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
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
            using namespace Microsoft::Terminal::TaskExecution;
            constexpr DWORD flags{ FILE_NAME_NORMALIZED | VOLUME_NAME_DOS };
            const auto required = GetFinalPathNameByHandleW(
                handle,
                nullptr,
                0,
                flags);
            if (required == 0 ||
                required > MaximumTaskExecutionPathCharacters + 8)
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

        bool _safeRegularFile(
            const HANDLE handle,
            const std::filesystem::path& expectedPath,
            std::optional<FILE_STANDARD_INFO>& size,
            DWORD& systemError)
        {
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            FILE_STANDARD_INFO standard{};
            const auto finalPath = _finalPath(handle, systemError);
            if (!GetFileInformationByHandleEx(
                    handle,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes)) ||
                !GetFileInformationByHandleEx(
                    handle,
                    FileStandardInfo,
                    &standard,
                    sizeof(standard)) ||
                (attributes.FileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                  FILE_ATTRIBUTE_DEVICE)) != 0 ||
                standard.Directory || !finalPath ||
                !_samePath(*finalPath, expectedPath))
            {
                return false;
            }
            size = standard;
            return true;
        }
    }

    GitWorktreeMetadataFileResult ReadGitWorktreeMetadataFile(
        const std::filesystem::path& path,
        const size_t maximumBytes) noexcept
    {
        try
        {
            if (path.empty() || maximumBytes == 0 || maximumBytes > UINT32_MAX)
            {
                return _failure(
                    GitWorktreeMetadataFileStatus::IoFailure,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree metadata read request is invalid");
            }
            HandleOwner file;
            file.Value = CreateFileW(
                _extendedPath(path).c_str(),
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
                const auto error = GetLastError();
                return _failure(
                    error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                        GitWorktreeMetadataFileStatus::Missing :
                        GitWorktreeMetadataFileStatus::IoFailure,
                    error,
                    "Git Worktree metadata file could not be opened");
            }

            DWORD error{};
            std::optional<FILE_STANDARD_INFO> size;
            if (!_safeRegularFile(file.Value, path, size, error) ||
                size->EndOfFile.QuadPart <= 0 ||
                size->EndOfFile.QuadPart > static_cast<LONGLONG>(maximumBytes))
            {
                return _failure(
                    GitWorktreeMetadataFileStatus::UnsafePath,
                    error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error,
                    "Git Worktree metadata file identity or size is unsafe");
            }

            GitWorktreeMetadataFileResult result;
            result.Status = GitWorktreeMetadataFileStatus::Succeeded;
            result.Bytes.resize(static_cast<size_t>(size->EndOfFile.QuadPart));
            size_t offset{};
            while (offset < result.Bytes.size())
            {
                DWORD read{};
                if (!ReadFile(
                        file.Value,
                        result.Bytes.data() + offset,
                        static_cast<DWORD>(result.Bytes.size() - offset),
                        &read,
                        nullptr) ||
                    read == 0)
                {
                    return _failure(
                        GitWorktreeMetadataFileStatus::IoFailure,
                        GetLastError(),
                        "Git Worktree metadata file could not be read");
                }
                offset += read;
            }
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeMetadataFileStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree metadata reading exhausted local resources");
        }
    }

    GitWorktreeMetadataFileResult WriteNewGitWorktreeMetadataFile(
        const std::filesystem::path& path,
        const std::span<const uint8_t> bytes) noexcept
    {
        try
        {
            if (path.empty() || bytes.empty() || bytes.size() > UINT32_MAX)
            {
                return _failure(
                    GitWorktreeMetadataFileStatus::IoFailure,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree metadata write request is invalid");
            }
            HandleOwner file;
            file.Value = CreateFileW(
                _extendedPath(path).c_str(),
                GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (!file.Value || file.Value == INVALID_HANDLE_VALUE)
            {
                file.Value = nullptr;
                const auto error = GetLastError();
                return _failure(
                    error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
                        GitWorktreeMetadataFileStatus::Collision :
                        GitWorktreeMetadataFileStatus::IoFailure,
                    error,
                    "Git Worktree metadata file could not be created");
            }

            DWORD error{};
            std::optional<FILE_STANDARD_INFO> ignoredSize;
            if (!_safeRegularFile(file.Value, path, ignoredSize, error))
            {
                return _failure(
                    GitWorktreeMetadataFileStatus::UnsafePath,
                    error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error,
                    "Git Worktree metadata file identity is unsafe");
            }
            const auto cleanupFailure = [&](const DWORD systemError,
                                            std::string errorMessage) {
                CloseHandle(file.Value);
                file.Value = nullptr;
                DeleteFileW(_extendedPath(path).c_str());
                return _failure(
                    GitWorktreeMetadataFileStatus::IoFailure,
                    systemError,
                    std::move(errorMessage));
            };

            size_t offset{};
            while (offset < bytes.size())
            {
                DWORD written{};
                if (!WriteFile(
                        file.Value,
                        bytes.data() + offset,
                        static_cast<DWORD>(bytes.size() - offset),
                        &written,
                        nullptr) ||
                    written == 0)
                {
                    return cleanupFailure(
                        GetLastError(),
                        "Git Worktree metadata file could not be written");
                }
                offset += written;
            }
            if (!FlushFileBuffers(file.Value))
            {
                return cleanupFailure(
                    GetLastError(),
                    "Git Worktree metadata file could not be flushed");
            }

            GitWorktreeMetadataFileResult result;
            result.Status = GitWorktreeMetadataFileStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeMetadataFileStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree metadata writing exhausted local resources");
        }
    }

    GitWorktreeMetadataFileResult ReplaceGitWorktreeMetadataFile(
        const std::filesystem::path& stagingPath,
        const std::filesystem::path& destinationPath) noexcept
    {
        try
        {
            if (stagingPath.empty() || destinationPath.empty())
            {
                return _failure(
                    GitWorktreeMetadataFileStatus::IoFailure,
                    ERROR_INVALID_PARAMETER,
                    "Git Worktree metadata replacement request is invalid");
            }
            if (!MoveFileExW(
                    _extendedPath(stagingPath).c_str(),
                    _extendedPath(destinationPath).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const auto error = GetLastError();
                DeleteFileW(_extendedPath(stagingPath).c_str());
                return _failure(
                    GitWorktreeMetadataFileStatus::IoFailure,
                    error,
                    "Git Worktree metadata file could not be replaced atomically");
            }
            GitWorktreeMetadataFileResult result;
            result.Status = GitWorktreeMetadataFileStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeMetadataFileStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree metadata replacement exhausted local resources");
        }
    }

    void DeleteGitWorktreeMetadataFile(
        const std::filesystem::path& path) noexcept
    {
        try
        {
            if (!path.empty())
            {
                DeleteFileW(_extendedPath(path).c_str());
            }
        }
        catch (...)
        {
        }
    }
}
