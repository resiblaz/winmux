// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeFileCopyInternal.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        std::optional<std::filesystem::path> _finalPath(
            const HANDLE handle,
            DWORD& systemError)
        {
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
    }

    GitWorktreeCopyHandle::~GitWorktreeCopyHandle()
    {
        Reset();
    }

    GitWorktreeCopyHandle::GitWorktreeCopyHandle(
        GitWorktreeCopyHandle&& other) noexcept :
        Value{ std::exchange(other.Value, nullptr) }
    {
    }

    GitWorktreeCopyHandle& GitWorktreeCopyHandle::operator=(
        GitWorktreeCopyHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(std::exchange(other.Value, nullptr));
        }
        return *this;
    }

    void GitWorktreeCopyHandle::Reset(const HANDLE value) noexcept
    {
        if (Value && Value != INVALID_HANDLE_VALUE)
        {
            CloseHandle(Value);
        }
        Value = value;
    }

    GitWorktreeFileCopyResult GitWorktreeFileCopyResultFor(
        const GitWorktreeFileCopyStatus status,
        const DWORD systemError,
        std::string error)
    {
        GitWorktreeFileCopyResult result;
        result.Status = status;
        result.SystemError = systemError;
        result.Error = std::move(error);
        return result;
    }

    std::filesystem::path ExtendedGitWorktreeCopyPath(
        const std::filesystem::path& path)
    {
        const auto& value = path.native();
        return value.starts_with(L"\\\\") ?
                   std::filesystem::path{ LR"(\\?\UNC\)" + value.substr(2) } :
                   std::filesystem::path{ LR"(\\?\)" + value };
    }

    bool SameGitWorktreeCopyPath(
        const std::filesystem::path& left,
        const std::filesystem::path& right) noexcept
    {
        try
        {
            return left.native().size() <= static_cast<size_t>(INT_MAX) &&
                   right.native().size() <= static_cast<size_t>(INT_MAX) &&
                   CompareStringOrdinal(
                       left.native().data(),
                       static_cast<int>(left.native().size()),
                       right.native().data(),
                       static_cast<int>(right.native().size()),
                       TRUE) == CSTR_EQUAL;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ReadGitWorktreeFileEvidence(
        const HANDLE handle,
        const std::filesystem::path& expectedPath,
        GitWorktreeFileEvidence& evidence,
        DWORD& systemError)
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        const auto finalPath = _finalPath(handle, systemError);
        if (!GetFileInformationByHandleEx(
                handle,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) ||
            !GetFileInformationByHandleEx(
                handle,
                FileStandardInfo,
                &evidence.Standard,
                sizeof(evidence.Standard)) ||
            !GetFileInformationByHandleEx(
                handle,
                FileBasicInfo,
                &evidence.Basic,
                sizeof(evidence.Basic)) ||
            !GetFileInformationByHandleEx(
                handle,
                FileIdInfo,
                &evidence.Identity,
                sizeof(evidence.Identity)) ||
            (attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DEVICE)) != 0 ||
            evidence.Standard.Directory || !finalPath ||
            !SameGitWorktreeCopyPath(*finalPath, expectedPath))
        {
            if (systemError == ERROR_SUCCESS)
            {
                systemError = GetLastError();
                if (systemError == ERROR_SUCCESS)
                {
                    systemError = ERROR_ACCESS_DENIED;
                }
            }
            return false;
        }
        return true;
    }

    bool SameGitWorktreeFileEvidence(
        const GitWorktreeFileEvidence& left,
        const GitWorktreeFileEvidence& right) noexcept
    {
        return left.Standard.EndOfFile.QuadPart ==
                   right.Standard.EndOfFile.QuadPart &&
               left.Basic.CreationTime.QuadPart ==
                   right.Basic.CreationTime.QuadPart &&
               left.Basic.LastWriteTime.QuadPart ==
                   right.Basic.LastWriteTime.QuadPart &&
               left.Basic.ChangeTime.QuadPart ==
                   right.Basic.ChangeTime.QuadPart &&
               left.Basic.FileAttributes == right.Basic.FileAttributes &&
               left.Identity.VolumeSerialNumber ==
                   right.Identity.VolumeSerialNumber &&
               std::equal(
                   std::begin(left.Identity.FileId.Identifier),
                   std::end(left.Identity.FileId.Identifier),
                   std::begin(right.Identity.FileId.Identifier));
    }

    bool ValidateGitWorktreeCopyRoot(
        const CanonicalDirectoryIdentity& root) noexcept
    {
        try
        {
            if (!root.Valid() ||
                root.Policy() != DirectoryIdentityPolicy::Worktree)
            {
                return false;
            }
            const auto current = ResolveCanonicalDirectoryIdentity(
                root.CanonicalPath(),
                DirectoryIdentityPolicy::Worktree);
            return current.Succeeded() &&
                   current.Identity->Digest() == root.Digest();
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<std::filesystem::path> DecodeGitWorktreeCopyRelativePath(
        const std::string_view value)
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
        const std::filesystem::path path{ decoded };
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

    bool RewindGitWorktreeCopyFile(
        const HANDLE file,
        DWORD& systemError) noexcept
    {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
        {
            systemError = GetLastError();
            return false;
        }
        return true;
    }

    GitWorktreeFileCopyResult EnsureGitWorktreeCopyParents(
        const CanonicalDirectoryIdentity& destinationRoot,
        const std::filesystem::path& relativeParent)
    {
        auto current = destinationRoot.CanonicalPath();
        for (const auto& component : relativeParent)
        {
            current /= component;
            if (CreateDirectoryW(
                    ExtendedGitWorktreeCopyPath(current).c_str(),
                    nullptr) == FALSE)
            {
                const auto error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS)
                {
                    return GitWorktreeFileCopyResultFor(
                        GitWorktreeFileCopyStatus::IoFailure,
                        error,
                        "Task Worktree destination parent could not be created");
                }
            }
            const auto attributes = GetFileAttributesW(
                ExtendedGitWorktreeCopyPath(current).c_str());
            const auto identity = ResolveCanonicalDirectoryIdentity(
                current,
                DirectoryIdentityPolicy::Worktree);
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes &
                 (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0 ||
                !identity.Succeeded() ||
                !SameGitWorktreeCopyPath(
                    identity.Identity->CanonicalPath(),
                    current))
            {
                return GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::UnsafeDestination,
                    attributes == INVALID_FILE_ATTRIBUTES ?
                        GetLastError() :
                        ERROR_ACCESS_DENIED,
                    "Task Worktree destination parent identity is unsafe");
            }
        }
        return GitWorktreeFileCopyResultFor(
            GitWorktreeFileCopyStatus::Succeeded,
            ERROR_SUCCESS,
            {});
    }

    GitWorktreeFileCopyResult CleanupGitWorktreeCopyStaging(
        const std::filesystem::path& stagingPath)
    {
        GitWorktreeCopyHandle staging;
        staging.Value = CreateFileW(
            ExtendedGitWorktreeCopyPath(stagingPath).c_str(),
            DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (!staging.Value || staging.Value == INVALID_HANDLE_VALUE)
        {
            staging.Value = nullptr;
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            {
                return GitWorktreeFileCopyResultFor(
                    GitWorktreeFileCopyStatus::Succeeded,
                    ERROR_SUCCESS,
                    {});
            }
            return GitWorktreeFileCopyResultFor(
                GitWorktreeFileCopyStatus::IoFailure,
                error,
                "Task Worktree copy staging file could not be opened");
        }
        GitWorktreeFileEvidence ignored;
        DWORD error{};
        if (!ReadGitWorktreeFileEvidence(
                staging.Value,
                stagingPath,
                ignored,
                error))
        {
            return GitWorktreeFileCopyResultFor(
                GitWorktreeFileCopyStatus::UnsafeDestination,
                error,
                "Task Worktree copy staging identity is unsafe");
        }
        FILE_DISPOSITION_INFO disposition{ TRUE };
        if (!SetFileInformationByHandle(
                staging.Value,
                FileDispositionInfo,
                &disposition,
                sizeof(disposition)))
        {
            return GitWorktreeFileCopyResultFor(
                GitWorktreeFileCopyStatus::IoFailure,
                GetLastError(),
                "Task Worktree copy staging file could not be discarded");
        }
        staging.Reset();
        return GitWorktreeFileCopyResultFor(
            GitWorktreeFileCopyStatus::Succeeded,
            ERROR_SUCCESS,
            {});
    }
}
