// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitCommandRunner.h"

#include "GitCommandLine.h"
#include "GitEnvironment.h"
#include "GitExecutable.h"
#include "GitProcessInternal.h"

namespace Microsoft::Terminal::TaskExecution
{
    namespace
    {
        GitCommandResult _failure(
            const GitCommandStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitCommandResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _validArguments(
            const std::vector<std::wstring>& arguments) noexcept
        {
            try
            {
                if (arguments.empty() || arguments.size() > MaximumGitArgumentCount)
                {
                    return false;
                }
                size_t total{};
                for (const auto& argument : arguments)
                {
                    if (argument.size() > MaximumGitArgumentCharacters ||
                        argument.find(L'\0') != std::wstring::npos ||
                        total > MaximumGitCommandLineCharacters -
                                    std::min(
                                        argument.size(),
                                        MaximumGitCommandLineCharacters))
                    {
                        return false;
                    }
                    total += argument.size();
                }
                return total <= MaximumGitCommandLineCharacters;
            }
            catch (...)
            {
                return false;
            }
        }

        GitCommandStatus _validateCancellation(
            const HANDLE cancellationEvent,
            DWORD& systemError) noexcept
        {
            if (!cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE)
            {
                systemError = ERROR_INVALID_HANDLE;
                return GitCommandStatus::InvalidRequest;
            }
            const auto waited = WaitForSingleObject(cancellationEvent, 0);
            if (waited == WAIT_OBJECT_0)
            {
                systemError = ERROR_OPERATION_ABORTED;
                return GitCommandStatus::Cancelled;
            }
            if (waited == WAIT_TIMEOUT)
            {
                return GitCommandStatus::Completed;
            }
            systemError = waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE;
            return GitCommandStatus::InvalidRequest;
        }
    }

    GitCommandResult GitCommandRunner::Run(
        const GitCommandRequest& request) const noexcept
    {
        try
        {
            if (!_validArguments(request.Arguments) ||
                request.TimeoutMilliseconds == 0 ||
                request.TimeoutMilliseconds > MaximumGitCommandTimeoutMilliseconds ||
                request.MaximumStdoutBytes == 0 ||
                request.MaximumStderrBytes == 0 ||
                request.MaximumStdoutBytes > MaximumGitOutputBytes ||
                request.MaximumStderrBytes > MaximumGitOutputBytes ||
                request.MaximumStdoutBytes >
                    MaximumGitOutputBytes - request.MaximumStderrBytes)
            {
                return _failure(
                    GitCommandStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git command request exceeds a structural bound");
            }
            const auto deadline = GetTickCount64() + request.TimeoutMilliseconds;

            DWORD error{};
            const auto cancellation = _validateCancellation(
                request.CancellationEvent,
                error);
            if (cancellation != GitCommandStatus::Completed)
            {
                return _failure(
                    cancellation,
                    error,
                    cancellation == GitCommandStatus::Cancelled ?
                        "Git command was cancelled before launch" :
                        "Git command cancellation handle is invalid");
            }

            const auto executable = ValidateGitExecutable(request.GitExecutable);
            if (!executable.Succeeded())
            {
                return _failure(
                    GitCommandStatus::ExecutableUnavailable,
                    executable.SystemError,
                    "Git executable could not be trusted");
            }
            if (!request.WorkingDirectory.Valid())
            {
                return _failure(
                    GitCommandStatus::InvalidRequest,
                    ERROR_BAD_PATHNAME,
                    "Git working-directory identity is invalid");
            }
            const auto workingDirectory = ResolveCanonicalDirectoryIdentity(
                request.WorkingDirectory.CanonicalPath(),
                request.WorkingDirectory.Policy());
            if (!workingDirectory.Succeeded() ||
                workingDirectory.Identity->Digest() !=
                    request.WorkingDirectory.Digest())
            {
                return _failure(
                    GitCommandStatus::InvalidRequest,
                    ERROR_INVALID_DATA,
                    "Git working-directory identity changed before launch");
            }

            auto environment = details::BuildSanitizedGitEnvironment(
                request.Environment);
            if (!environment.Succeeded)
            {
                return _failure(
                    GitCommandStatus::InvalidRequest,
                    ERROR_INVALID_ENVIRONMENT,
                    std::move(environment.Error));
            }

            std::vector<std::wstring> arguments{
                L"-c",
                L"core.quotepath=false",
                L"-c",
                L"i18n.logOutputEncoding=utf-8",
            };
            if (arguments.size() + request.Arguments.size() >
                MaximumGitArgumentCount)
            {
                return _failure(
                    GitCommandStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git command argument count leaves no room for safety options");
            }
            arguments.insert(
                arguments.end(),
                request.Arguments.begin(),
                request.Arguments.end());
            const auto commandLine = details::BuildGitCommandLine(
                *executable.Executable,
                arguments);
            if (!commandLine)
            {
                return _failure(
                    GitCommandStatus::InvalidRequest,
                    ERROR_FILENAME_EXCED_RANGE,
                    "Git command line exceeds its encoded bound");
            }

            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                return _failure(
                    GitCommandStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git command deadline expired before process launch");
            }
            const auto launchCancellation = _validateCancellation(
                request.CancellationEvent,
                error);
            if (launchCancellation != GitCommandStatus::Completed)
            {
                return _failure(
                    launchCancellation,
                    error,
                    launchCancellation == GitCommandStatus::Cancelled ?
                        "Git command was cancelled before process launch" :
                        "Git command cancellation handle changed before launch");
            }
            auto executionRequest = request;
            const auto executionNow = GetTickCount64();
            if (executionNow >= deadline)
            {
                return _failure(
                    GitCommandStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git command deadline expired while copying the request");
            }
            executionRequest.TimeoutMilliseconds = static_cast<uint32_t>(
                (std::min<uint64_t>)(deadline - executionNow, UINT32_MAX));
            return details::RunGitProcess(
                executionRequest,
                *executable.Executable,
                workingDirectory.Identity->CanonicalPath(),
                *commandLine,
                std::move(environment.Block));
        }
        catch (...)
        {
            return _failure(
                GitCommandStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git command preparation exhausted local resources");
        }
    }
}
