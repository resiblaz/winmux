// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeCheckpointInternal.h"

#include "GitSourceCommandOutput.h"
#include "GitWorktreeReplayInternal.h"

#include <charconv>

namespace Microsoft::Terminal::TaskGit
{
    std::optional<std::string> ParseGitWorktreeCheckpointStartDigest(
        const std::string_view summaryJson) noexcept
    {
        constexpr std::string_view prefix{ "{\"startStateDigest\":\"" };
        constexpr std::string_view suffix{ "\"}" };
        if (!summaryJson.starts_with(prefix) || !summaryJson.ends_with(suffix) ||
            summaryJson.size() != prefix.size() + 64 + suffix.size())
        {
            return std::nullopt;
        }
        const auto digest = summaryJson.substr(prefix.size(), 64);
        if (!std::ranges::all_of(digest, [](const char value) {
                return (value >= '0' && value <= '9') ||
                       (value >= 'a' && value <= 'f');
            }))
        {
            return std::nullopt;
        }
        return std::string{ digest };
    }
}

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        bool _validRequest(
            const std::filesystem::path& gitExecutable,
            const GitCommandExecutor& executor,
            const HANDLE cancellationEvent,
            const uint32_t timeoutMilliseconds) noexcept
        {
            return !gitExecutable.empty() && executor && cancellationEvent &&
                   cancellationEvent != INVALID_HANDLE_VALUE &&
                   timeoutMilliseconds > 0 &&
                   timeoutMilliseconds <=
                       MaximumGitWorktreeCheckpointTimeoutMilliseconds;
        }

        std::optional<uint32_t> _remaining(const uint64_t deadline) noexcept
        {
            const auto now = GetTickCount64();
            return now < deadline ?
                       std::optional<uint32_t>{ static_cast<uint32_t>(
                           (std::min<uint64_t>)(
                               deadline - now,
                               UINT32_MAX)) } :
                       std::nullopt;
        }

        bool _sameTarget(
            const GitRepositoryInspection& first,
            const GitRepositoryInspection& second) noexcept
        {
            return first.WorktreeIdentity.Digest() ==
                       second.WorktreeIdentity.Digest() &&
                   first.CommonDirectoryIdentity.Digest() ==
                       second.CommonDirectoryIdentity.Digest() &&
                   first.GitDirectoryIdentity.Digest() ==
                       second.GitDirectoryIdentity.Digest() &&
                   first.ObjectFormat == second.ObjectFormat &&
                   first.Head == second.Head &&
                   first.BranchRef == second.BranchRef &&
                   first.LinkedWorktree == second.LinkedWorktree &&
                   !second.DetachedHead;
        }

        std::optional<uint64_t> _decimal(const std::string_view value) noexcept
        {
            if (value.empty())
            {
                return std::nullopt;
            }
            uint64_t parsed{};
            const auto [end, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                parsed);
            return error == std::errc{} && end == value.data() + value.size() ?
                       std::optional<uint64_t>{ parsed } :
                       std::nullopt;
        }

        bool _add(uint64_t& target, const uint64_t value) noexcept
        {
            if (target > (std::numeric_limits<uint64_t>::max)() - value)
            {
                return false;
            }
            target += value;
            return true;
        }

        void _appendJsonString(std::string& output, const std::string_view value)
        {
            constexpr char hex[]{ "0123456789abcdef" };
            output.push_back('"');
            for (const auto raw : value)
            {
                const auto character = static_cast<unsigned char>(raw);
                switch (character)
                {
                case '"':
                    output.append("\\\"");
                    break;
                case '\\':
                    output.append("\\\\");
                    break;
                case '\b':
                    output.append("\\b");
                    break;
                case '\f':
                    output.append("\\f");
                    break;
                case '\n':
                    output.append("\\n");
                    break;
                case '\r':
                    output.append("\\r");
                    break;
                case '\t':
                    output.append("\\t");
                    break;
                default:
                    if (character < 0x20)
                    {
                        output.append("\\u00");
                        output.push_back(hex[character >> 4]);
                        output.push_back(hex[character & 0x0f]);
                    }
                    else
                    {
                        output.push_back(static_cast<char>(character));
                    }
                    break;
                }
            }
            output.push_back('"');
        }

        bool _nextNulToken(
            const std::string_view output,
            size_t& offset,
            std::string_view& token) noexcept
        {
            if (offset >= output.size())
            {
                return false;
            }
            const auto end = output.find('\0', offset);
            if (end == std::string_view::npos)
            {
                return false;
            }
            token = output.substr(offset, end - offset);
            offset = end + 1;
            return true;
        }

        bool _completedLine(
            GitWorktreeCheckpointCommandContext& context,
            std::vector<std::wstring> arguments,
            std::string& line,
            std::string& error)
        {
            const auto command = context.Run(std::move(arguments));
            if (!command)
            {
                error = context.FailureError;
                return false;
            }
            const auto parsed = ParseGitSourceLine(*command);
            if (command->ExitCode != 0 || !parsed)
            {
                error = "Task Worktree checkpoint verification output is invalid";
                return false;
            }
            line = *parsed;
            return true;
        }
    }

    std::optional<Microsoft::Terminal::TaskExecution::GitCommandResult>
    GitWorktreeCheckpointCommandContext::Run(std::vector<std::wstring> arguments)
    {
        using namespace Microsoft::Terminal::TaskExecution;
        const auto remaining = _remaining(Deadline);
        if (!remaining)
        {
            FailureStatus = GitWorktreeCheckpointStatus::TimedOut;
            FailureSystemError = WAIT_TIMEOUT;
            FailureError = "Task Worktree checkpoint command deadline expired";
            return std::nullopt;
        }
        GitCommandRequest request;
        request.GitExecutable = Target.GitExecutable;
        request.WorkingDirectory = Target.WorktreeIdentity;
        request.Arguments = std::move(arguments);
        request.CancellationEvent = CancellationEvent;
        request.TimeoutMilliseconds = (std::min)(
            *remaining,
            MaximumGitCommandTimeoutMilliseconds);
        request.MaximumStdoutBytes = GitWorktreeCheckpointStdoutBytes;
        request.MaximumStderrBytes = GitWorktreeCheckpointStderrBytes;
        auto result = Executor(request);
        if (result.Status != GitCommandStatus::Completed)
        {
            FailureStatus = MapGitWorktreeCheckpointCommandStatus(result.Status);
            FailureSystemError = result.SystemError;
            FailureError = result.Error.empty() ?
                               "Task Worktree checkpoint Git command failed" :
                               result.Error;
            return std::nullopt;
        }
        return result;
    }

    GitWorktreeCheckpointContextResult OpenGitWorktreeCheckpointContext(
        const std::filesystem::path& gitExecutable,
        const Microsoft::Terminal::TaskExecution::GitCommandExecutor& executor,
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) noexcept
    {
        using namespace Microsoft::Terminal::TaskExecution;
        GitWorktreeCheckpointContextResult result;
        try
        {
            if (!_validRequest(
                    gitExecutable,
                    executor,
                    cancellationEvent,
                    timeoutMilliseconds))
            {
                result.Status = GitWorktreeCheckpointStatus::InvalidRequest;
                result.SystemError = ERROR_INVALID_PARAMETER;
                result.Error = "Task Worktree checkpoint request is invalid";
                return result;
            }
            const auto evidence = ValidateGitWorktreeReplayEvidence(
                layout,
                marker,
                journal);
            if (!evidence.Succeeded() || marker.BaselineCommit.empty() ||
                !IsGitSourceObjectId(marker.BaselineCommit, journal.ObjectFormat))
            {
                result.Status = evidence.Status ==
                                        GitWorktreeReplayStatus::ResourceFailure ?
                                    GitWorktreeCheckpointStatus::ResourceFailure :
                                evidence.Status ==
                                        GitWorktreeReplayStatus::InvalidRequest ?
                                    GitWorktreeCheckpointStatus::InvalidRequest :
                                    GitWorktreeCheckpointStatus::EvidenceMismatch;
                result.SystemError = evidence.SystemError == ERROR_SUCCESS ?
                                         ERROR_REVISION_MISMATCH :
                                         evidence.SystemError;
                result.Error = evidence.Error.empty() ?
                                   "Task Worktree checkpoint evidence is incomplete" :
                                   evidence.Error;
                return result;
            }

            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            GitRepositoryInspector inspector{ gitExecutable, executor };
            const auto firstTime = _remaining(deadline);
            if (!firstTime)
            {
                result.Status = GitWorktreeCheckpointStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                result.Error = "Task Worktree checkpoint expired before inspection";
                return result;
            }
            const auto first = inspector.Inspect(
                layout.WorktreePath,
                cancellationEvent,
                (std::min)(*firstTime, MaximumGitCommandTimeoutMilliseconds));
            if (!first.Succeeded())
            {
                result.Status = MapGitWorktreeCheckpointInspectionStatus(first.Status);
                result.SystemError = first.SystemError;
                result.Error = "Task Worktree checkpoint target inspection failed";
                return result;
            }
            if (first.Inspection->CommonDirectoryIdentity.Digest() !=
                    marker.RepositoryIdentityDigest ||
                first.Inspection->ObjectFormat != journal.ObjectFormat ||
                first.Inspection->BranchRef != marker.BranchRef ||
                first.Inspection->DetachedHead ||
                !IsGitSourceObjectId(
                    first.Inspection->Head,
                    first.Inspection->ObjectFormat))
            {
                result.Status = GitWorktreeCheckpointStatus::TargetChanged;
                result.SystemError = ERROR_REVISION_MISMATCH;
                result.Error = "Task Worktree checkpoint target identity changed";
                return result;
            }

            const auto lockTime = _remaining(deadline);
            if (!lockTime)
            {
                result.Status = GitWorktreeCheckpointStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                result.Error = "Task Worktree checkpoint expired before repository locking";
                return result;
            }
            auto lock = AcquireGitRepositoryLock(
                first.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(*lockTime, GitRepositoryLockTimeoutMilliseconds));
            if (!lock.Acquired())
            {
                result.Status = MapGitWorktreeCheckpointLockStatus(lock.Status);
                result.SystemError = lock.SystemError;
                result.Error = "Task Worktree checkpoint repository lock failed";
                return result;
            }

            const auto secondTime = _remaining(deadline);
            if (!secondTime)
            {
                result.Status = GitWorktreeCheckpointStatus::TimedOut;
                result.SystemError = WAIT_TIMEOUT;
                result.Error = "Task Worktree checkpoint expired while locked";
                return result;
            }
            const auto second = inspector.Inspect(
                layout.WorktreePath,
                cancellationEvent,
                (std::min)(*secondTime, MaximumGitCommandTimeoutMilliseconds));
            if (!second.Succeeded() ||
                !_sameTarget(*first.Inspection, *second.Inspection))
            {
                result.Status = second.Succeeded() ?
                                    GitWorktreeCheckpointStatus::TargetChanged :
                                    MapGitWorktreeCheckpointInspectionStatus(second.Status);
                result.SystemError = second.SystemError == ERROR_SUCCESS ?
                                         ERROR_REVISION_MISMATCH :
                                         second.SystemError;
                result.Error = "Task Worktree checkpoint target changed while locking";
                return result;
            }

            GitWorktreeCheckpointCommandContext context;
            context.Target = *second.Inspection;
            context.Lock = std::move(lock.Lock);
            context.Executor = executor;
            context.CancellationEvent = cancellationEvent;
            context.Deadline = deadline;
            result.Context.emplace(std::move(context));
            result.Status = GitWorktreeCheckpointStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            result.Status = GitWorktreeCheckpointStatus::ResourceFailure;
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Task Worktree checkpoint context exhausted local resources";
            return result;
        }
    }

    GitWorktreeCheckpointStatus MapGitWorktreeCheckpointCommandStatus(
        const Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitCommandStatus;
        switch (status)
        {
        case GitCommandStatus::Cancelled:
            return GitWorktreeCheckpointStatus::Cancelled;
        case GitCommandStatus::TimedOut:
            return GitWorktreeCheckpointStatus::TimedOut;
        case GitCommandStatus::OutputLimitExceeded:
            return GitWorktreeCheckpointStatus::OutputLimitExceeded;
        case GitCommandStatus::InvalidRequest:
            return GitWorktreeCheckpointStatus::InvalidRequest;
        case GitCommandStatus::ResourceFailure:
            return GitWorktreeCheckpointStatus::ResourceFailure;
        default:
            return GitWorktreeCheckpointStatus::GitFailure;
        }
    }

    GitWorktreeCheckpointStatus MapGitWorktreeCheckpointInspectionStatus(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus;
        switch (status)
        {
        case GitRepositoryInspectionStatus::Cancelled:
            return GitWorktreeCheckpointStatus::Cancelled;
        case GitRepositoryInspectionStatus::TimedOut:
            return GitWorktreeCheckpointStatus::TimedOut;
        case GitRepositoryInspectionStatus::LockFailed:
            return GitWorktreeCheckpointStatus::LockFailed;
        case GitRepositoryInspectionStatus::ResourceFailure:
            return GitWorktreeCheckpointStatus::ResourceFailure;
        default:
            return GitWorktreeCheckpointStatus::InspectionFailed;
        }
    }

    GitWorktreeCheckpointStatus MapGitWorktreeCheckpointLockStatus(
        const Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::TaskDirectoryLockStatus;
        switch (status)
        {
        case TaskDirectoryLockStatus::Cancelled:
            return GitWorktreeCheckpointStatus::Cancelled;
        case TaskDirectoryLockStatus::TimedOut:
            return GitWorktreeCheckpointStatus::TimedOut;
        case TaskDirectoryLockStatus::ResourceFailure:
            return GitWorktreeCheckpointStatus::ResourceFailure;
        default:
            return GitWorktreeCheckpointStatus::LockFailed;
        }
    }

    std::optional<GitWorktreeCheckpointDiffSummary>
    ParseGitWorktreeCheckpointNumstat(const std::string_view output) noexcept
    {
        try
        {
            GitWorktreeCheckpointDiffSummary summary;
            if (output.empty())
            {
                return summary;
            }
            if (output.back() != '\0')
            {
                return std::nullopt;
            }
            size_t offset{};
            while (offset < output.size())
            {
                std::string_view record;
                if (!_nextNulToken(output, offset, record))
                {
                    return std::nullopt;
                }
                const auto firstTab = record.find('\t');
                const auto secondTab = firstTab == std::string_view::npos ?
                                           std::string_view::npos :
                                           record.find('\t', firstTab + 1);
                if (firstTab == std::string_view::npos ||
                    secondTab == std::string_view::npos)
                {
                    return std::nullopt;
                }
                const auto additions = record.substr(0, firstTab);
                const auto deletions = record.substr(
                    firstTab + 1,
                    secondTab - firstTab - 1);
                const auto path = record.substr(secondTab + 1);
                if (path.empty())
                {
                    std::string_view oldPath;
                    std::string_view newPath;
                    if (!_nextNulToken(output, offset, oldPath) ||
                        !_nextNulToken(output, offset, newPath) ||
                        oldPath.empty() || newPath.empty())
                    {
                        return std::nullopt;
                    }
                }
                if (additions == "-" && deletions == "-")
                {
                    if (!_add(summary.BinaryFiles, 1))
                    {
                        return std::nullopt;
                    }
                }
                else
                {
                    const auto added = _decimal(additions);
                    const auto deleted = _decimal(deletions);
                    if (!added || !deleted ||
                        !_add(summary.Additions, *added) ||
                        !_add(summary.Deletions, *deleted))
                    {
                        return std::nullopt;
                    }
                }
                if (!_add(summary.Files, 1))
                {
                    return std::nullopt;
                }
            }
            return summary;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::string> EncodeGitWorktreeCheckpointPaths(
        const std::span<const std::string> paths) noexcept
    {
        try
        {
            std::string result{ "[" };
            for (size_t index = 0; index < paths.size(); ++index)
            {
                if (index != 0)
                {
                    result.push_back(',');
                }
                _appendJsonString(result, paths[index]);
                if (result.size() > MaximumGitWorktreeCheckpointJsonBytes)
                {
                    return std::nullopt;
                }
            }
            result.push_back(']');
            return result.size() <= MaximumGitWorktreeCheckpointJsonBytes ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::string> EncodeGitWorktreeCheckpointSummary(
        const std::string_view startDigest,
        const std::string_view endDigest,
        const GitWorktreeCheckpointDiffSummary& summary) noexcept
    {
        try
        {
            const auto validDigest = [](const std::string_view value) {
                return value.size() == 64 &&
                       std::ranges::all_of(value, [](const char character) {
                           return (character >= '0' && character <= '9') ||
                                  (character >= 'a' && character <= 'f');
                       });
            };
            if (!validDigest(startDigest) || !validDigest(endDigest))
            {
                return std::nullopt;
            }
            auto result = "{\"startStateDigest\":\"" +
                          std::string{ startDigest } +
                          "\",\"endStateDigest\":\"" +
                          std::string{ endDigest } +
                          "\",\"files\":" + std::to_string(summary.Files) +
                          ",\"additions\":" + std::to_string(summary.Additions) +
                          ",\"deletions\":" + std::to_string(summary.Deletions) +
                          ",\"binaryFiles\":" +
                          std::to_string(summary.BinaryFiles) + "}";
            return result.size() <= MaximumGitWorktreeCheckpointJsonBytes ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::string GitWorktreeCheckpointSubject(
        const Microsoft::Terminal::TaskCore::TaskRunId& run)
    {
        return "Richer Task checkpoint " + run.Value();
    }

    bool VerifyGitWorktreeCheckpointCommit(
        GitWorktreeCheckpointCommandContext& context,
        const GitWorktreeLayout& layout,
        const Microsoft::Terminal::TaskCore::TaskRunId& run,
        const std::string_view agentEndCommit,
        const std::string_view checkpointCommit,
        const Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat,
        std::string& error) noexcept
    {
        try
        {
            if (!Microsoft::Terminal::TaskCore::IsCanonicalUuid(run.Value()) ||
                !IsGitSourceObjectId(agentEndCommit, objectFormat) ||
                !IsGitSourceObjectId(checkpointCommit, objectFormat) ||
                agentEndCommit == checkpointCommit)
            {
                error = "Task Worktree checkpoint commit proof is invalid";
                return false;
            }
            std::string subject;
            std::string parents;
            std::string head;
            std::string branch;
            if (!_completedLine(
                    context,
                    { L"show", L"-s", L"--format=%s", L"HEAD" },
                    subject,
                    error) ||
                !_completedLine(
                    context,
                    { L"rev-list", L"--parents", L"-n", L"1", L"HEAD" },
                    parents,
                    error) ||
                !_completedLine(
                    context,
                    { L"rev-parse", L"--verify", L"HEAD" },
                    head,
                    error) ||
                !_completedLine(
                    context,
                    {
                        L"rev-parse",
                        L"--verify",
                        WidenGitSourceAscii(layout.BranchRef),
                    },
                    branch,
                    error))
            {
                return false;
            }
            const auto expectedParents = std::string{ checkpointCommit } + " " +
                                         std::string{ agentEndCommit };
            if (subject != GitWorktreeCheckpointSubject(run) ||
                parents != expectedParents || head != checkpointCommit ||
                branch != checkpointCommit)
            {
                error = "Task Worktree checkpoint ownership proof does not match";
                return false;
            }

            const auto nonempty = context.Run({
                L"diff-tree",
                L"--quiet",
                L"--exit-code",
                WidenGitSourceAscii(std::string{ agentEndCommit }),
                WidenGitSourceAscii(std::string{ checkpointCommit }),
                L"--",
            });
            const auto status = context.Run({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
            });
            if (!nonempty || !status)
            {
                error = context.FailureError;
                return false;
            }
            if (nonempty->ExitCode != 1 || status->ExitCode != 0 ||
                !status->Stdout.empty())
            {
                error = "Task Worktree checkpoint commit is empty or not clean";
                return false;
            }
            return true;
        }
        catch (...)
        {
            error = "Task Worktree checkpoint verification exhausted local resources";
            return false;
        }
    }
}
