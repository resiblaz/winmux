// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitExecutable.h"

#include "DirectoryIdentity.h"
#include "GitCommandTypes.h"

namespace Microsoft::Terminal::TaskExecution
{
    namespace
    {
        struct HandleOwner
        {
            HandleOwner() noexcept = default;
            HandleOwner(const HandleOwner&) = delete;
            HandleOwner& operator=(const HandleOwner&) = delete;

            HANDLE Value{};

            ~HandleOwner()
            {
                if (Value && Value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(Value);
                }
            }
        };

        GitExecutableResult _failure(
            const GitExecutableStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitExecutableResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _equalsIgnoreCase(
            const std::wstring_view left,
            const std::wstring_view right) noexcept
        {
            return left.size() == right.size() &&
                   CompareStringOrdinal(
                       left.data(),
                       static_cast<int>(left.size()),
                       right.data(),
                       static_cast<int>(right.size()),
                   TRUE) == CSTR_EQUAL;
        }

        bool _startsWithIgnoreCase(
            const std::wstring_view value,
            const std::wstring_view prefix) noexcept
        {
            return value.size() >= prefix.size() &&
                   CompareStringOrdinal(
                       value.data(),
                       static_cast<int>(prefix.size()),
                       prefix.data(),
                       static_cast<int>(prefix.size()),
                       TRUE) == CSTR_EQUAL;
        }

        bool _validName(const std::filesystem::path& path) noexcept
        {
            try
            {
                return _equalsIgnoreCase(path.filename().native(), L"git.exe");
            }
            catch (...)
            {
                return false;
            }
        }

        std::filesystem::path _extendedPath(const std::filesystem::path& path)
        {
            const auto& value = path.native();
            return value.starts_with(L"\\\\") ?
                       std::filesystem::path{ LR"(\\?\UNC\)" + value.substr(2) } :
                       std::filesystem::path{ LR"(\\?\)" + value };
        }

        std::optional<std::filesystem::path> _ordinaryFinalPath(
            const HANDLE handle,
            DWORD& systemError)
        {
            constexpr DWORD flags{ FILE_NAME_NORMALIZED | VOLUME_NAME_DOS };
            const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
            if (required == 0 || required > MaximumTaskExecutionPathCharacters + 8)
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
            if (_startsWithIgnoreCase(value, uncPrefix))
            {
                value = L"\\\\" + value.substr(uncPrefix.size());
            }
            else if (_startsWithIgnoreCase(value, prefix))
            {
                value.erase(0, prefix.size());
            }
            else
            {
                systemError = ERROR_BAD_PATHNAME;
                return std::nullopt;
            }
            if (value.empty() || value.size() > MaximumTaskExecutionPathCharacters ||
                value.find(L'\0') != std::wstring::npos)
            {
                systemError = ERROR_BAD_PATHNAME;
                return std::nullopt;
            }
            return std::filesystem::path{ value }.lexically_normal();
        }

        std::optional<FILE_ID_INFO> _fileIdentity(
            const HANDLE handle,
            DWORD& systemError)
        {
            FILE_ID_INFO identity{};
            if (!GetFileInformationByHandleEx(
                    handle,
                    FileIdInfo,
                    &identity,
                    sizeof(identity)))
            {
                systemError = GetLastError();
                return std::nullopt;
            }
            return identity;
        }

        bool _sameFileIdentity(
            const FILE_ID_INFO& left,
            const FILE_ID_INFO& right) noexcept
        {
            return left.VolumeSerialNumber == right.VolumeSerialNumber &&
                   std::equal(
                       std::begin(left.FileId.Identifier),
                       std::end(left.FileId.Identifier),
                       std::begin(right.FileId.Identifier));
        }
    }

    GitExecutableResult ValidateGitExecutable(
        const std::filesystem::path& executable) noexcept
    {
        try
        {
            const auto& value = executable.native();
            if (value.empty() || value.size() > MaximumTaskExecutionPathCharacters ||
                value.find(L'\0') != std::wstring::npos ||
                (value.size() >= 2 && value[0] == L'\\' && value[1] == L'\\') ||
                !executable.is_absolute() || !_validName(executable))
            {
                return _failure(
                    GitExecutableStatus::InvalidPath,
                    ERROR_BAD_PATHNAME,
                    "Git executable path is invalid");
            }
            const auto parent = ResolveCanonicalDirectoryIdentity(
                executable.parent_path(),
                DirectoryIdentityPolicy::Direct);
            if (!parent.Succeeded())
            {
                return _failure(
                    GitExecutableStatus::UnsafePath,
                    parent.SystemError,
                    "Git executable parent directory is unsafe");
            }

            const auto candidate = parent.Identity->CanonicalPath() /
                                   executable.filename();
            const auto apiCandidate = _extendedPath(candidate);
            const auto attributes = GetFileAttributesW(apiCandidate.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto error = GetLastError();
                return _failure(
                    error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                        GitExecutableStatus::Missing :
                        GitExecutableStatus::PathUnavailable,
                    error,
                    "Git executable attributes are unavailable");
            }
            if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_REPARSE_POINT |
                               FILE_ATTRIBUTE_DEVICE)) != 0)
            {
                return _failure(
                    GitExecutableStatus::UnsafePath,
                    ERROR_ACCESS_DENIED,
                    "Git executable has unsafe attributes");
            }

            HandleOwner original;
            original.Value = CreateFileW(
                apiCandidate.c_str(),
                FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (!original.Value || original.Value == INVALID_HANDLE_VALUE)
            {
                original.Value = nullptr;
                return _failure(
                    GitExecutableStatus::PathUnavailable,
                    GetLastError(),
                    "Git executable could not be opened for verification");
            }

            DWORD error{};
            const auto finalPath = _ordinaryFinalPath(original.Value, error);
            if (!finalPath || !_validName(*finalPath))
            {
                return _failure(
                    GitExecutableStatus::PathUnavailable,
                    error == ERROR_SUCCESS ? ERROR_BAD_PATHNAME : error,
                    "Git executable final path is unavailable");
            }
            const auto finalParent = ResolveCanonicalDirectoryIdentity(
                finalPath->parent_path(),
                DirectoryIdentityPolicy::Direct);
            if (!finalParent.Succeeded() ||
                finalParent.Identity->Digest() != parent.Identity->Digest())
            {
                return _failure(
                    GitExecutableStatus::IdentityMismatch,
                    ERROR_INVALID_DATA,
                    "Git executable parent identity changed during verification");
            }

            const auto originalIdentity = _fileIdentity(original.Value, error);
            HandleOwner verified;
            const auto apiFinal = _extendedPath(*finalPath);
            verified.Value = CreateFileW(
                apiFinal.c_str(),
                FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            std::optional<FILE_ID_INFO> verifiedIdentity;
            if (verified.Value && verified.Value != INVALID_HANDLE_VALUE)
            {
                verifiedIdentity = _fileIdentity(verified.Value, error);
            }
            else
            {
                error = GetLastError();
                verified.Value = nullptr;
            }
            if (!originalIdentity || !verifiedIdentity ||
                !_sameFileIdentity(*originalIdentity, *verifiedIdentity))
            {
                return _failure(
                    GitExecutableStatus::IdentityMismatch,
                    error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error,
                    "Git executable identity changed during verification");
            }

            GitExecutableResult result;
            result.Status = GitExecutableStatus::Succeeded;
            result.Executable = *finalPath;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitExecutableStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git executable verification exhausted local resources");
        }
    }

    GitExecutableResult FindGitExecutable() noexcept
    {
        try
        {
            const auto required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (required == 0 || required > MaximumGitEnvironmentCharacters)
            {
                return _failure(
                    GitExecutableStatus::InvalidPath,
                    ERROR_ENVVAR_NOT_FOUND,
                    "PATH is unavailable or exceeds its bound");
            }
            std::wstring pathValue(required, L'\0');
            const auto written = GetEnvironmentVariableW(
                L"PATH",
                pathValue.data(),
                required);
            if (written == 0 || written >= required)
            {
                return _failure(
                    GitExecutableStatus::InvalidPath,
                    GetLastError(),
                    "PATH could not be read");
            }
            pathValue.resize(written);

            size_t offset{};
            while (offset <= pathValue.size())
            {
                const auto separator = pathValue.find(L';', offset);
                auto entry = pathValue.substr(
                    offset,
                    separator == std::wstring::npos ?
                        std::wstring::npos :
                        separator - offset);
                if (entry.size() >= 2 && entry.front() == L'\"' &&
                    entry.back() == L'\"')
                {
                    entry = entry.substr(1, entry.size() - 2);
                }
                if (!entry.empty())
                {
                    const std::filesystem::path directory{ entry };
                    if (directory.is_absolute())
                    {
                        const auto candidate = ValidateGitExecutable(
                            directory / L"git.exe");
                        if (candidate.Succeeded())
                        {
                            return candidate;
                        }
                    }
                }
                if (separator == std::wstring::npos)
                {
                    break;
                }
                offset = separator + 1;
            }
            return _failure(
                GitExecutableStatus::Missing,
                ERROR_FILE_NOT_FOUND,
                "A trusted git.exe was not found on PATH");
        }
        catch (...)
        {
            return _failure(
                GitExecutableStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git executable discovery exhausted local resources");
        }
    }
}
