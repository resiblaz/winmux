// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitRepositoryInspector.h"

#include "GitCommandRunner.h"
#include "GitExecutable.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskExecution
{
    namespace
    {
        constexpr size_t InspectorStdoutBytes{ 64 * 1024 };
        constexpr size_t InspectorStderrBytes{ 16 * 1024 };
        constexpr int64_t WindowsToUnixEpoch100ns{ 116444736000000000LL };

        GitRepositoryInspectionResult _failure(
            const GitRepositoryInspectionStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitRepositoryInspectionResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        std::optional<std::wstring> _strictWide(const std::string_view value)
        {
            if (value.empty())
            {
                return std::wstring{};
            }
            if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
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
                    required) != required)
            {
                return std::nullopt;
            }
            return result;
        }

        std::optional<std::wstring> _singleLine(
            const GitCommandResult& result,
            const bool allowEmpty = false)
        {
            if (result.Status != GitCommandStatus::Completed ||
                result.Stdout.size() > InspectorStdoutBytes ||
                result.Stderr.size() > InspectorStderrBytes ||
                !_strictWide(result.Stderr))
            {
                return std::nullopt;
            }
            auto value = _strictWide(result.Stdout);
            if (!value)
            {
                return std::nullopt;
            }
            if (!value->empty() && value->back() == L'\n')
            {
                value->pop_back();
                if (!value->empty() && value->back() == L'\r')
                {
                    value->pop_back();
                }
            }
            if ((!allowEmpty && value->empty()) ||
                value->size() > MaximumTaskExecutionPathCharacters ||
                std::ranges::any_of(*value, [](const wchar_t character) {
                    return character < L' ' || character == 0x7f;
                }))
            {
                return std::nullopt;
            }
            return value;
        }

        std::optional<std::string> _asciiLine(
            const GitCommandResult& result,
            const size_t maximumCharacters,
            const bool allowEmpty = false)
        {
            const auto line = _singleLine(result, allowEmpty);
            if (!line || line->size() > maximumCharacters ||
                std::ranges::any_of(*line, [](const wchar_t character) {
                    return character > 0x7f;
                }))
            {
                return std::nullopt;
            }
            std::string ascii;
            ascii.reserve(line->size());
            for (const auto character : *line)
            {
                ascii.push_back(static_cast<char>(character));
            }
            return ascii;
        }

        std::optional<std::string> _utf8Line(
            const GitCommandResult& result,
            const size_t maximumBytes,
            const bool allowEmpty = false)
        {
            if (result.Status != GitCommandStatus::Completed ||
                result.Stdout.size() > maximumBytes ||
                result.Stderr.size() > InspectorStderrBytes ||
                !_strictWide(result.Stdout) || !_strictWide(result.Stderr))
            {
                return std::nullopt;
            }
            auto value = result.Stdout;
            if (!value.empty() && value.back() == '\n')
            {
                value.pop_back();
                if (!value.empty() && value.back() == '\r')
                {
                    value.pop_back();
                }
            }
            if ((!allowEmpty && value.empty()) || value.size() > maximumBytes ||
                std::ranges::any_of(value, [](const unsigned char character) {
                    return character < 0x20 || character == 0x7f;
                }))
            {
                return std::nullopt;
            }
            return value;
        }

        std::optional<GitVersion> _version(const GitCommandResult& result)
        {
            const auto line = _asciiLine(result, 128);
            constexpr std::string_view prefix{ "git version " };
            if (!line || !std::string_view{ *line }.starts_with(prefix))
            {
                return std::nullopt;
            }
            auto value = std::string_view{ *line }.substr(prefix.size());
            GitVersion version;
            auto component = [&](uint32_t& output) {
                const auto separator = value.find('.');
                const auto text = value.substr(0, separator);
                if (text.empty())
                {
                    return false;
                }
                const auto parsed = std::from_chars(
                    text.data(),
                    text.data() + text.size(),
                    output);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != text.data() + text.size())
                {
                    return false;
                }
                value = separator == std::string_view::npos ?
                            std::string_view{} :
                            value.substr(separator + 1);
                return true;
            };
            if (!component(version.Major) || !component(version.Minor))
            {
                return std::nullopt;
            }
            const auto patchEnd = value.find_first_not_of("0123456789");
            const auto patch = value.substr(0, patchEnd);
            if (patch.empty())
            {
                return std::nullopt;
            }
            const auto parsed = std::from_chars(
                patch.data(),
                patch.data() + patch.size(),
                version.Patch);
            return parsed.ec == std::errc{} &&
                           parsed.ptr == patch.data() + patch.size() ?
                       std::optional<GitVersion>{ version } :
                       std::nullopt;
        }

        bool _supported(const GitVersion& version) noexcept
        {
            return version.Major > MinimumSupportedGitMajorVersion ||
                   (version.Major == MinimumSupportedGitMajorVersion &&
                    version.Minor >= MinimumSupportedGitMinorVersion);
        }

        std::optional<GitObjectFormat> _objectFormat(
            const GitCommandResult& result)
        {
            const auto value = _asciiLine(result, 16);
            if (value == std::optional<std::string>{ "sha1" })
            {
                return GitObjectFormat::Sha1;
            }
            if (value == std::optional<std::string>{ "sha256" })
            {
                return GitObjectFormat::Sha256;
            }
            return std::nullopt;
        }

        bool _hexObjectId(
            const std::string_view value,
            const GitObjectFormat format) noexcept
        {
            const auto length = format == GitObjectFormat::Sha1 ? 40 : 64;
            return value.size() == length &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        int64_t _unixMilliseconds() noexcept
        {
            FILETIME fileTime{};
            GetSystemTimePreciseAsFileTime(&fileTime);
            ULARGE_INTEGER value{};
            value.LowPart = fileTime.dwLowDateTime;
            value.HighPart = fileTime.dwHighDateTime;
            return value.QuadPart >= static_cast<uint64_t>(WindowsToUnixEpoch100ns) ?
                       static_cast<int64_t>(
                           (value.QuadPart - WindowsToUnixEpoch100ns) / 10000) :
                       0;
        }

        GitRepositoryInspectionStatus _commandFailureStatus(
            const GitCommandResult& result) noexcept
        {
            switch (result.Status)
            {
            case GitCommandStatus::Cancelled:
                return GitRepositoryInspectionStatus::Cancelled;
            case GitCommandStatus::TimedOut:
                return GitRepositoryInspectionStatus::TimedOut;
            case GitCommandStatus::InvalidUtf8:
            case GitCommandStatus::OutputLimitExceeded:
                return GitRepositoryInspectionStatus::InvalidOutput;
            case GitCommandStatus::ExecutableUnavailable:
                return GitRepositoryInspectionStatus::GitUnavailable;
            default:
                return GitRepositoryInspectionStatus::InspectionFailed;
            }
        }

        GitRepositoryInspectionStatus _lockFailureStatus(
            const TaskDirectoryLockStatus status) noexcept
        {
            switch (status)
            {
            case TaskDirectoryLockStatus::Cancelled:
                return GitRepositoryInspectionStatus::Cancelled;
            case TaskDirectoryLockStatus::TimedOut:
                return GitRepositoryInspectionStatus::TimedOut;
            default:
                return GitRepositoryInspectionStatus::LockFailed;
            }
        }
    }

    GitRepositoryInspector::GitRepositoryInspector(
        std::filesystem::path gitExecutable,
        GitCommandExecutor executor) :
        _gitExecutable{ std::move(gitExecutable) },
        _executor{ executor ?
                       std::move(executor) :
                       GitCommandExecutor{ [](const GitCommandRequest& request) {
                           return GitCommandRunner{}.Run(request);
                       } } }
    {
    }

    GitRepositoryInspectionResult GitRepositoryInspector::Inspect(
        const std::filesystem::path& path,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!_executor || _gitExecutable.empty() || !cancellationEvent ||
                cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumGitCommandTimeoutMilliseconds)
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git repository inspection request is invalid");
            }
            const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
            if (cancelled == WAIT_OBJECT_0)
            {
                return _failure(
                    GitRepositoryInspectionStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Git repository inspection was cancelled before start");
            }
            if (cancelled != WAIT_TIMEOUT)
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidRequest,
                    cancelled == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE,
                    "Git repository inspection cancellation handle is invalid");
            }

            const auto input = ResolveCanonicalDirectoryIdentity(
                path,
                DirectoryIdentityPolicy::Worktree);
            if (!input.Succeeded())
            {
                return _failure(
                    GitRepositoryInspectionStatus::DirectoryRejected,
                    input.SystemError,
                    "Git repository inspection directory was rejected");
            }
            const auto executable = ValidateGitExecutable(_gitExecutable);
            if (!executable.Succeeded())
            {
                return _failure(
                    GitRepositoryInspectionStatus::GitUnavailable,
                    executable.SystemError,
                    "Git repository inspection executable is unavailable");
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            GitRepositoryInspectionResult commandFailure;
            const auto invoke = [&](
                                    const CanonicalDirectoryIdentity& directory,
                                    std::vector<std::wstring> arguments) ->
                std::optional<GitCommandResult> {
                const auto now = GetTickCount64();
                if (now >= deadline)
                {
                    commandFailure = _failure(
                        GitRepositoryInspectionStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Git repository inspection deadline expired");
                    return std::nullopt;
                }
                GitCommandRequest request;
                request.GitExecutable = *executable.Executable;
                request.WorkingDirectory = directory;
                request.Arguments = std::move(arguments);
                request.CancellationEvent = cancellationEvent;
                request.TimeoutMilliseconds = static_cast<uint32_t>(
                    (std::min<uint64_t>)(deadline - now, UINT32_MAX));
                request.MaximumStdoutBytes = InspectorStdoutBytes;
                request.MaximumStderrBytes = InspectorStderrBytes;
                const auto result = _executor(request);
                if (GetTickCount64() >= deadline)
                {
                    commandFailure = _failure(
                        GitRepositoryInspectionStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Git repository inspection command exceeded the deadline");
                    return std::nullopt;
                }
                if (result.Status != GitCommandStatus::Completed)
                {
                    commandFailure = _failure(
                        _commandFailureStatus(result),
                        result.SystemError,
                        "Git repository inspection command failed");
                    return std::nullopt;
                }
                return result;
            };

            const auto versionResult = invoke(*input.Identity, { L"--version" });
            const auto version = versionResult ? _version(*versionResult) : std::nullopt;
            if (!versionResult)
            {
                return commandFailure;
            }
            if (!version)
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git version output is invalid");
            }
            if (!_supported(*version))
            {
                return _failure(
                    GitRepositoryInspectionStatus::UnsupportedGitVersion,
                    ERROR_OLD_WIN_VERSION,
                    "Git 2.38 or newer is required");
            }

            const auto commonResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--path-format=absolute", L"--git-common-dir" });
            if (!commonResult)
            {
                return commandFailure;
            }
            if (commonResult->ExitCode != 0)
            {
                return _failure(
                    GitRepositoryInspectionStatus::NotRepository,
                    commonResult->ExitCode,
                    "Directory is not a Git repository worktree");
            }
            const auto commonLine = _singleLine(*commonResult);
            const auto common = commonLine ?
                                    ResolveCanonicalDirectoryIdentity(
                                        *commonLine,
                                        DirectoryIdentityPolicy::Worktree) :
                                    DirectoryIdentityResult{};
            if (!commonLine || !common.Succeeded())
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git common-directory output is invalid");
            }

            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                return _failure(
                    GitRepositoryInspectionStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git repository inspection deadline expired before locking");
            }
            auto gitLock = AcquireGitRepositoryLock(
                *common.Identity,
                cancellationEvent,
                static_cast<uint32_t>((std::min<uint64_t>)(deadline - now, UINT32_MAX)));
            if (!gitLock.Acquired())
            {
                return _failure(
                    _lockFailureStatus(gitLock.Status),
                    gitLock.SystemError,
                    "Git common-directory lock acquisition failed");
            }

            const auto confirmedCommonResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--path-format=absolute", L"--git-common-dir" });
            const auto confirmedCommonLine = confirmedCommonResult ?
                                                 _singleLine(*confirmedCommonResult) :
                                                 std::nullopt;
            const auto confirmedCommon = confirmedCommonLine ?
                                             ResolveCanonicalDirectoryIdentity(
                                                 *confirmedCommonLine,
                                                 DirectoryIdentityPolicy::Worktree) :
                                             DirectoryIdentityResult{};
            if (!confirmedCommonResult)
            {
                return commandFailure;
            }
            if (!confirmedCommonLine || !confirmedCommon.Succeeded() ||
                confirmedCommon.Identity->Digest() != common.Identity->Digest())
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git common-directory identity changed while locking");
            }

            const auto bareResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--is-bare-repository" });
            const auto bare = bareResult ? _asciiLine(*bareResult, 8) : std::nullopt;
            if (!bareResult)
            {
                return commandFailure;
            }
            if (bare == std::optional<std::string>{ "true" })
            {
                return _failure(
                    GitRepositoryInspectionStatus::BareRepository,
                    ERROR_NOT_SUPPORTED,
                    "Bare repositories are not supported for Task Worktrees");
            }
            if (bare != std::optional<std::string>{ "false" })
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git bare-repository output is invalid");
            }

            const auto insideResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--is-inside-work-tree" });
            const auto inside = insideResult ? _asciiLine(*insideResult, 8) : std::nullopt;
            if (!insideResult)
            {
                return commandFailure;
            }
            if (inside != std::optional<std::string>{ "true" })
            {
                return _failure(
                    inside ? GitRepositoryInspectionStatus::NotRepository :
                             GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Directory is not inside a Git worktree");
            }

            const auto worktreeResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--path-format=absolute", L"--show-toplevel" });
            const auto gitDirectoryResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--path-format=absolute", L"--git-dir" });
            const auto objectFormatResult = invoke(
                *input.Identity,
                { L"rev-parse", L"--show-object-format" });
            if (!worktreeResult || !gitDirectoryResult || !objectFormatResult)
            {
                return commandFailure;
            }
            const auto worktreeLine = _singleLine(*worktreeResult);
            const auto gitDirectoryLine = _singleLine(*gitDirectoryResult);
            const auto objectFormat = _objectFormat(*objectFormatResult);
            const auto worktree = worktreeLine ?
                                      ResolveCanonicalDirectoryIdentity(
                                          *worktreeLine,
                                          DirectoryIdentityPolicy::Worktree) :
                                      DirectoryIdentityResult{};
            const auto gitDirectory = gitDirectoryLine ?
                                          ResolveCanonicalDirectoryIdentity(
                                              *gitDirectoryLine,
                                              DirectoryIdentityPolicy::Worktree) :
                                          DirectoryIdentityResult{};
            if (!worktreeLine || !gitDirectoryLine || !objectFormat ||
                !worktree.Succeeded() || !gitDirectory.Succeeded())
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git repository path or object-format output is invalid");
            }

            const auto headResult = invoke(
                *worktree.Identity,
                { L"rev-parse", L"--verify", L"HEAD" });
            if (!headResult)
            {
                return commandFailure;
            }
            if (headResult->ExitCode != 0)
            {
                return _failure(
                    GitRepositoryInspectionStatus::UnbornRepository,
                    headResult->ExitCode,
                    "Git repository has no committed HEAD");
            }
            const auto head = _asciiLine(*headResult, 64);
            if (!head || !_hexObjectId(*head, *objectFormat))
            {
                return _failure(
                    GitRepositoryInspectionStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git HEAD output does not match its object format");
            }

            const auto branchResult = invoke(
                *worktree.Identity,
                { L"symbolic-ref", L"--quiet", L"HEAD" });
            if (!branchResult)
            {
                return commandFailure;
            }
            std::string branch;
            bool detached{};
            if (branchResult->ExitCode == 0)
            {
                const auto branchLine = _utf8Line(*branchResult, 4096);
                if (!branchLine || !std::string_view{ *branchLine }.starts_with("refs/heads/"))
                {
                    return _failure(
                        GitRepositoryInspectionStatus::InvalidOutput,
                        ERROR_INVALID_DATA,
                        "Git symbolic HEAD output is invalid");
                }
                branch = *branchLine;
            }
            else if (branchResult->ExitCode == 1)
            {
                detached = true;
                if (!branchResult->Stdout.empty())
                {
                    return _failure(
                        GitRepositoryInspectionStatus::InvalidOutput,
                        ERROR_INVALID_DATA,
                        "Detached Git symbolic HEAD returned unexpected output");
                }
            }
            else
            {
                return _failure(
                    GitRepositoryInspectionStatus::InspectionFailed,
                    branchResult->ExitCode,
                    "Git symbolic HEAD query failed");
            }

            GitRepositoryInspectionResult result;
            result.Status = GitRepositoryInspectionStatus::Succeeded;
            result.Inspection.emplace();
            result.Inspection->GitExecutable = *executable.Executable;
            result.Inspection->WorktreeIdentity = *worktree.Identity;
            result.Inspection->CommonDirectoryIdentity = *common.Identity;
            result.Inspection->GitDirectoryIdentity = *gitDirectory.Identity;
            result.Inspection->Version = *version;
            result.Inspection->ObjectFormat = *objectFormat;
            result.Inspection->Head = *head;
            result.Inspection->BranchRef = std::move(branch);
            result.Inspection->LinkedWorktree =
                gitDirectory.Identity->Digest() != common.Identity->Digest();
            result.Inspection->DetachedHead = detached;
            result.Inspection->CapturedAtUnixMilliseconds = _unixMilliseconds();
            return result;
        }
        catch (...)
        {
            return _failure(
                GitRepositoryInspectionStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git repository inspection exhausted local resources");
        }
    }
}
