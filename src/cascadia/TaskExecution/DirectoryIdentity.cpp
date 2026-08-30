// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "DirectoryIdentity.h"
#include "DirectoryIdentityInternal.h"

#include "TaskExecutionCrypto.h"

namespace Microsoft::Terminal::TaskExecution
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

        struct DirectoryFileIdentity
        {
            uint64_t VolumeSerialNumber{};
            std::array<uint8_t, 16> FileId{};
        };

        DirectoryIdentityResult _failure(
            const DirectoryIdentityStatus status,
            const DWORD systemError,
            std::string error)
        {
            DirectoryIdentityResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _slash(const wchar_t value) noexcept
        {
            return value == L'\\' || value == L'/';
        }

        bool _driveLetter(const wchar_t value) noexcept
        {
            return (value >= L'a' && value <= L'z') ||
                   (value >= L'A' && value <= L'Z');
        }

        bool _startsWithOrdinal(
            const std::wstring_view value,
            const std::wstring_view prefix,
            const bool ignoreCase = false) noexcept
        {
            return value.size() >= prefix.size() &&
                   CompareStringOrdinal(
                       value.data(),
                       static_cast<int>(prefix.size()),
                       prefix.data(),
                       static_cast<int>(prefix.size()),
                       ignoreCase) == CSTR_EQUAL;
        }

        std::optional<std::filesystem::path> _ordinaryPath(
            const std::filesystem::path& input)
        {
            auto value = input.native();
            if (value.empty() ||
                value.size() > MaximumTaskExecutionPathCharacters + 8 ||
                value.find(L'\0') != std::wstring::npos)
            {
                return std::nullopt;
            }
            std::ranges::replace(value, L'/', L'\\');

            constexpr std::wstring_view extendedUnc{ LR"(\\?\UNC\)" };
            constexpr std::wstring_view extendedPrefix{ LR"(\\?\)" };
            constexpr std::wstring_view devicePrefix{ LR"(\\.\)" };
            if (_startsWithOrdinal(value, devicePrefix, true))
            {
                return std::nullopt;
            }
            if (_startsWithOrdinal(value, extendedUnc, true))
            {
                value = L"\\\\" + value.substr(extendedUnc.size());
            }
            else if (_startsWithOrdinal(value, extendedPrefix, true))
            {
                value.erase(0, extendedPrefix.size());
            }

            const std::filesystem::path path{ value };
            if (!path.is_absolute() ||
                std::ranges::any_of(path, [](const auto& component) {
                    return component == L"." || component == L"..";
                }))
            {
                return std::nullopt;
            }
            auto normalized = path.lexically_normal();
            value = normalized.native();
            std::ranges::replace(value, L'/', L'\\');
            const auto rootLength = normalized.root_path().native().size();
            while (value.size() > rootLength && !value.empty() && _slash(value.back()))
            {
                value.pop_back();
            }
            if (value.empty() || value.size() > MaximumTaskExecutionPathCharacters)
            {
                return std::nullopt;
            }
            return std::filesystem::path{ value };
        }

        bool _isUnc(const std::filesystem::path& path) noexcept
        {
            try
            {
                const auto& value = path.native();
                return value.size() >= 2 && _slash(value[0]) && _slash(value[1]);
            }
            catch (...)
            {
                return false;
            }
        }

        std::filesystem::path _extendedApiPath(
            const std::filesystem::path& path)
        {
            const auto& value = path.native();
            return _isUnc(path) ?
                       std::filesystem::path{ LR"(\\?\UNC\)" + value.substr(2) } :
                       std::filesystem::path{ LR"(\\?\)" + value };
        }

        bool _samePath(
            const std::filesystem::path& left,
            const std::filesystem::path& right) noexcept
        {
            try
            {
                const auto& leftValue = left.native();
                const auto& rightValue = right.native();
                return CompareStringOrdinal(
                           leftValue.c_str(),
                           static_cast<int>(leftValue.size()),
                           rightValue.c_str(),
                           static_cast<int>(rightValue.size()),
                           TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _atOrBelow(
            const std::filesystem::path& candidate,
            const std::filesystem::path& root) noexcept
        {
            try
            {
                const auto& candidateValue = candidate.native();
                const auto& rootValue = root.native();
                if (candidateValue.size() < rootValue.size() ||
                    CompareStringOrdinal(
                        candidateValue.data(),
                        static_cast<int>(rootValue.size()),
                        rootValue.data(),
                        static_cast<int>(rootValue.size()),
                        TRUE) != CSTR_EQUAL)
                {
                    return false;
                }
                return candidateValue.size() == rootValue.size() ||
                       _slash(candidateValue[rootValue.size()]);
            }
            catch (...)
            {
                return false;
            }
        }

        std::optional<std::filesystem::path> _systemDirectory(
            const bool windowsDirectory)
        {
            std::wstring value(32768, L'\0');
            const auto written = windowsDirectory ?
                                     GetWindowsDirectoryW(
                                         value.data(),
                                         static_cast<UINT>(value.size())) :
                                     GetSystemDirectoryW(
                                         value.data(),
                                         static_cast<UINT>(value.size()));
            if (written == 0 || written >= value.size())
            {
                return std::nullopt;
            }
            value.resize(written);
            return _ordinaryPath(value);
        }

        std::optional<DirectoryIdentityStatus> _classifyProtectedRoot(
            const std::filesystem::path& path)
        {
            if (_samePath(path, path.root_path()))
            {
                return DirectoryIdentityStatus::RootRejected;
            }
            const auto windows = _systemDirectory(true);
            const auto system = _systemDirectory(false);
            if (!windows || !system)
            {
                return std::nullopt;
            }
            if (_atOrBelow(path, *windows) || _atOrBelow(path, *system))
            {
                return DirectoryIdentityStatus::SystemRootRejected;
            }
            return DirectoryIdentityStatus::Succeeded;
        }

        DirectoryIdentityResult _validateComponents(
            const std::filesystem::path& path)
        {
            auto current = path.root_path();
            for (const auto& component : path.relative_path())
            {
                current /= component;
                SetLastError(ERROR_SUCCESS);
                const auto apiPath = _extendedApiPath(current);
                const auto attributes = GetFileAttributesW(apiPath.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const auto error = GetLastError();
                    return _failure(
                        error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                            DirectoryIdentityStatus::MissingPath :
                            DirectoryIdentityStatus::OpenFailed,
                        error,
                        "Task directory component attributes are unavailable");
                }
                const auto status = details::ClassifyDirectoryAttributes(attributes);
                if (status != DirectoryIdentityStatus::Succeeded)
                {
                    return _failure(
                        status,
                        ERROR_ACCESS_DENIED,
                        "Task directory component has unsafe attributes");
                }
            }
            DirectoryIdentityResult result;
            result.Status = DirectoryIdentityStatus::Succeeded;
            return result;
        }

        DirectoryIdentityResult _openDirectory(
            const std::filesystem::path& path,
            HandleOwner& handle)
        {
            SetLastError(ERROR_SUCCESS);
            const auto apiPath = _extendedApiPath(path);
            handle.Value = CreateFileW(
                apiPath.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr);
            if (handle.Value == INVALID_HANDLE_VALUE)
            {
                const auto error = GetLastError();
                handle.Value = nullptr;
                return _failure(
                    error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                        DirectoryIdentityStatus::MissingPath :
                        DirectoryIdentityStatus::OpenFailed,
                    error,
                    "Task directory could not be opened for identity resolution");
            }

            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!GetFileInformationByHandleEx(
                    handle.Value,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes)))
            {
                return _failure(
                    DirectoryIdentityStatus::IdentityUnavailable,
                    GetLastError(),
                    "Task directory handle attributes are unavailable");
            }
            const auto status = details::ClassifyDirectoryAttributes(attributes.FileAttributes);
            if (status != DirectoryIdentityStatus::Succeeded)
            {
                return _failure(
                    status,
                    ERROR_ACCESS_DENIED,
                    "Task directory handle has unsafe attributes");
            }

            DirectoryIdentityResult result;
            result.Status = DirectoryIdentityStatus::Succeeded;
            return result;
        }

        std::optional<std::filesystem::path> _finalPath(
            const HANDLE handle,
            DWORD& systemError)
        {
            constexpr DWORD flags{ FILE_NAME_NORMALIZED | VOLUME_NAME_DOS };
            DWORD capacity = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
            if (capacity == 0)
            {
                systemError = GetLastError();
                return std::nullopt;
            }
            if (capacity > MaximumTaskExecutionPathCharacters + 8)
            {
                systemError = ERROR_FILENAME_EXCED_RANGE;
                return std::nullopt;
            }
            std::wstring value(static_cast<size_t>(capacity) + 1, L'\0');
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
            const auto ordinary = _ordinaryPath(value);
            if (!ordinary)
            {
                systemError = ERROR_BAD_PATHNAME;
            }
            return ordinary;
        }

        std::optional<DirectoryFileIdentity> _fileIdentity(
            const HANDLE handle,
            DWORD& systemError)
        {
            FILE_ID_INFO info{};
            if (!GetFileInformationByHandleEx(
                    handle,
                    FileIdInfo,
                    &info,
                    sizeof(info)))
            {
                systemError = GetLastError();
                return std::nullopt;
            }
            DirectoryFileIdentity identity;
            identity.VolumeSerialNumber = info.VolumeSerialNumber;
            std::copy_n(
                info.FileId.Identifier,
                identity.FileId.size(),
                identity.FileId.begin());
            if (identity.VolumeSerialNumber == 0 ||
                std::ranges::all_of(identity.FileId, [](const uint8_t byte) {
                    return byte == 0;
                }))
            {
                systemError = ERROR_INVALID_DATA;
                return std::nullopt;
            }
            return identity;
        }

        bool _sameIdentity(
            const DirectoryFileIdentity& left,
            const DirectoryFileIdentity& right) noexcept
        {
            return left.VolumeSerialNumber == right.VolumeSerialNumber &&
                   left.FileId == right.FileId;
        }

        std::optional<std::string> _identityDigest(
            const DirectoryFileIdentity& identity)
        {
            constexpr std::string_view domain{ "wt-task-directory-identity-v1" };
            std::vector<uint8_t> payload;
            payload.reserve(domain.size() + 1 + sizeof(uint64_t) + identity.FileId.size());
            payload.insert(payload.end(), domain.begin(), domain.end());
            payload.push_back(0);
            for (size_t index = 0; index < sizeof(uint64_t); ++index)
            {
                payload.push_back(static_cast<uint8_t>(
                    identity.VolumeSerialNumber >> (index * 8)));
            }
            payload.insert(payload.end(), identity.FileId.begin(), identity.FileId.end());
            const auto digest = details::TaskExecutionSha256(payload);
            return digest ?
                       std::optional<std::string>{ details::LowerHex(*digest) } :
                       std::nullopt;
        }
    }

    namespace details
    {
        DirectoryIdentityStatus ClassifyDirectoryAttributes(
            const DWORD attributes) noexcept
        {
            if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0)
            {
                return DirectoryIdentityStatus::DeviceRejected;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return DirectoryIdentityStatus::ReparsePointRejected;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return DirectoryIdentityStatus::NotDirectory;
            }
            return DirectoryIdentityStatus::Succeeded;
        }
    }

    const std::filesystem::path& CanonicalDirectoryIdentity::CanonicalPath() const noexcept
    {
        return _canonicalPath;
    }

    uint64_t CanonicalDirectoryIdentity::VolumeSerialNumber() const noexcept
    {
        return _volumeSerialNumber;
    }

    const std::array<uint8_t, 16>& CanonicalDirectoryIdentity::FileId() const noexcept
    {
        return _fileId;
    }

    const std::string& CanonicalDirectoryIdentity::Digest() const noexcept
    {
        return _digest;
    }

    DirectoryIdentityPolicy CanonicalDirectoryIdentity::Policy() const noexcept
    {
        return _policy;
    }

    bool CanonicalDirectoryIdentity::IsUnc() const noexcept
    {
        return _isUnc;
    }

    bool CanonicalDirectoryIdentity::Valid() const noexcept
    {
        return !_canonicalPath.empty() && _volumeSerialNumber != 0 &&
               _digest.size() == DirectoryIdentityDigestCharacters &&
               std::ranges::all_of(_digest, [](const char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               }) &&
               std::ranges::any_of(_fileId, [](const uint8_t byte) {
                   return byte != 0;
               });
    }

    DirectoryIdentityResult ResolveCanonicalDirectoryIdentity(
        const std::filesystem::path& path,
        const DirectoryIdentityPolicy policy) noexcept
    {
        try
        {
            if (policy != DirectoryIdentityPolicy::Direct &&
                policy != DirectoryIdentityPolicy::Worktree)
            {
                return _failure(
                    DirectoryIdentityStatus::InvalidPath,
                    ERROR_INVALID_PARAMETER,
                    "Task directory policy is invalid");
            }
            const auto input = _ordinaryPath(path);
            if (!input)
            {
                return _failure(
                    DirectoryIdentityStatus::InvalidPath,
                    ERROR_BAD_PATHNAME,
                    "Task directory path must be a normalized absolute Windows path");
            }
            if (policy == DirectoryIdentityPolicy::Worktree && _isUnc(*input))
            {
                return _failure(
                    DirectoryIdentityStatus::UncRejected,
                    ERROR_BAD_NETPATH,
                    "Worktree directory identity does not accept UNC paths");
            }
            const auto inputRoot = _classifyProtectedRoot(*input);
            if (!inputRoot)
            {
                return _failure(
                    DirectoryIdentityStatus::IdentityUnavailable,
                    ERROR_INVALID_DATA,
                    "Windows protected-directory identities are unavailable");
            }
            if (*inputRoot != DirectoryIdentityStatus::Succeeded)
            {
                return _failure(
                    *inputRoot,
                    ERROR_ACCESS_DENIED,
                    "Task execution does not accept drive or system roots");
            }
            if (auto checked = _validateComponents(*input);
                checked.Status != DirectoryIdentityStatus::Succeeded)
            {
                return checked;
            }

            HandleOwner original;
            if (auto opened = _openDirectory(*input, original);
                opened.Status != DirectoryIdentityStatus::Succeeded)
            {
                return opened;
            }
            DWORD error{};
            const auto canonicalPath = _finalPath(original.Value, error);
            if (!canonicalPath)
            {
                return _failure(
                    DirectoryIdentityStatus::FinalPathUnavailable,
                    error,
                    "Task directory final path is unavailable");
            }
            if (policy == DirectoryIdentityPolicy::Worktree && _isUnc(*canonicalPath))
            {
                return _failure(
                    DirectoryIdentityStatus::UncRejected,
                    ERROR_BAD_NETPATH,
                    "Resolved Worktree directory identity is UNC");
            }
            const auto canonicalRoot = _classifyProtectedRoot(*canonicalPath);
            if (!canonicalRoot)
            {
                return _failure(
                    DirectoryIdentityStatus::IdentityUnavailable,
                    ERROR_INVALID_DATA,
                    "Resolved Windows protected-directory identities are unavailable");
            }
            if (*canonicalRoot != DirectoryIdentityStatus::Succeeded)
            {
                return _failure(
                    *canonicalRoot,
                    ERROR_ACCESS_DENIED,
                    "Resolved Task directory is a drive or system root");
            }
            if (auto checked = _validateComponents(*canonicalPath);
                checked.Status != DirectoryIdentityStatus::Succeeded)
            {
                return checked;
            }

            const auto originalIdentity = _fileIdentity(original.Value, error);
            if (!originalIdentity)
            {
                return _failure(
                    DirectoryIdentityStatus::IdentityUnavailable,
                    error,
                    "Task directory volume/file identity is unavailable");
            }

            HandleOwner verified;
            if (auto opened = _openDirectory(*canonicalPath, verified);
                opened.Status != DirectoryIdentityStatus::Succeeded)
            {
                opened.Status = DirectoryIdentityStatus::AliasVerificationFailed;
                opened.Error = "Resolved Task directory could not be reopened";
                return opened;
            }
            const auto verifiedIdentity = _fileIdentity(verified.Value, error);
            if (!verifiedIdentity || !_sameIdentity(*originalIdentity, *verifiedIdentity))
            {
                return _failure(
                    DirectoryIdentityStatus::AliasVerificationFailed,
                    verifiedIdentity ? ERROR_INVALID_DATA : error,
                    "Resolved Task directory alias does not retain exact identity");
            }

            const auto digest = _identityDigest(*originalIdentity);
            if (!digest)
            {
                return _failure(
                    DirectoryIdentityStatus::HashFailure,
                    ERROR_INVALID_DATA,
                    "Task directory identity digest could not be derived");
            }

            DirectoryIdentityResult result;
            result.Status = DirectoryIdentityStatus::Succeeded;
            result.Identity.emplace();
            result.Identity->_canonicalPath = *canonicalPath;
            result.Identity->_volumeSerialNumber = originalIdentity->VolumeSerialNumber;
            result.Identity->_fileId = originalIdentity->FileId;
            result.Identity->_digest = *digest;
            result.Identity->_policy = policy;
            result.Identity->_isUnc = _isUnc(*canonicalPath);
            return result;
        }
        catch (const std::exception&)
        {
            return _failure(
                DirectoryIdentityStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task directory identity exhausted local resources");
        }
        catch (...)
        {
            return _failure(
                DirectoryIdentityStatus::ResourceFailure,
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Task directory identity failure");
        }
    }
}
