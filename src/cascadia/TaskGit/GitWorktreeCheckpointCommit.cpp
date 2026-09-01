// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreeCheckpoint.h"

#include "GitSourceCommandOutput.h"
#include "GitTrackedSource.h"
#include "GitWorktreeBaselineInternal.h"
#include "GitWorktreeCheckpointInternal.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        GitWorktreeCheckpointCommitResult _failure(
            const GitWorktreeCheckpointStatus status,
            const DWORD systemError,
            std::string error,
            const GitWorktreeLayout* layout = nullptr)
        {
            GitWorktreeCheckpointCommitResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            if (layout)
            {
                result.Preserved = true;
                result.PreservedPath = layout->WorktreePathUtf8;
            }
            return result;
        }

        GitWorktreeCheckpointCommitResult _contextFailure(
            const details::GitWorktreeCheckpointContextResult& context,
            const GitWorktreeLayout& layout)
        {
            return _failure(
                context.Status,
                context.SystemError,
                context.Error,
                context.Status == GitWorktreeCheckpointStatus::InvalidRequest ?
                    nullptr : &layout);
        }

        GitWorktreeCheckpointCommitResult _commandFailure(
            const details::GitWorktreeCheckpointCommandContext& context,
            const GitWorktreeLayout& layout)
        {
            return _failure(
                context.FailureStatus,
                context.FailureSystemError,
                context.FailureError,
                &layout);
        }

        GitWorktreeCheckpointStatus _policyStatus(
            const GitWorktreeBaselineStatus status) noexcept
        {
            switch (status)
            {
            case GitWorktreeBaselineStatus::InvalidRequest:
                return GitWorktreeCheckpointStatus::InvalidRequest;
            case GitWorktreeBaselineStatus::IdentityFailure:
                return GitWorktreeCheckpointStatus::IdentityFailure;
            case GitWorktreeBaselineStatus::Cancelled:
                return GitWorktreeCheckpointStatus::Cancelled;
            case GitWorktreeBaselineStatus::TimedOut:
                return GitWorktreeCheckpointStatus::TimedOut;
            case GitWorktreeBaselineStatus::ResourceFailure:
                return GitWorktreeCheckpointStatus::ResourceFailure;
            default:
                return GitWorktreeCheckpointStatus::GitFailure;
            }
        }
    }

    GitWorktreeCheckpointCommitResult
    GitWorktreeCheckpointManager::CommitOrRecover(
        const GitWorktreeLayout& layout,
        const GitWorktreeMarker& marker,
        const GitWorktreeSourceJournal& journal,
        const Microsoft::Terminal::TaskCore::TaskRunId& run,
        const std::string_view agentEndCommit,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!Microsoft::Terminal::TaskCore::IsCanonicalUuid(run.Value()) ||
                !details::IsGitSourceObjectId(
                    agentEndCommit,
                    journal.ObjectFormat))
            {
                return _failure(
                    GitWorktreeCheckpointStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Task Worktree checkpoint commit identity is invalid");
            }
            auto opened = details::OpenGitWorktreeCheckpointContext(
                _gitExecutable,
                _executor,
                layout,
                marker,
                journal,
                cancellationEvent,
                timeoutMilliseconds);
            if (!opened.Context)
            {
                return _contextFailure(opened, layout);
            }
            auto& context = *opened.Context;
            auto checkpointCommit = context.Target.Head;
            bool created{};
            bool fallback{};
            bool signing{};

            if (checkpointCommit == agentEndCommit)
            {
                const auto staged = context.Run({
                    L"-c",
                    L"core.hooksPath=NUL",
                    L"add",
                    L"-A",
                    L"--",
                });
                const auto dirty = context.Run({
                    L"diff",
                    L"--cached",
                    L"--quiet",
                    L"--exit-code",
                    L"HEAD",
                    L"--",
                });
                if (!staged || !dirty)
                {
                    return _commandFailure(context, layout);
                }
                if (staged->ExitCode != 0 || dirty->ExitCode != 1)
                {
                    return _failure(
                        GitWorktreeCheckpointStatus::TargetChanged,
                        ERROR_REVISION_MISMATCH,
                        "Task Worktree checkpoint candidate is empty or changed",
                        &layout);
                }

                const details::GitSourceCommandInvoker invoke =
                    [&](std::vector<std::wstring> arguments) {
                        return context.Run(std::move(arguments));
                    };
                const auto policy = details::InspectGitWorktreeCommitPolicy(invoke);
                if (!policy.Succeeded)
                {
                    if (!context.FailureError.empty())
                    {
                        return _commandFailure(context, layout);
                    }
                    return _failure(
                        _policyStatus(policy.Failure.Status),
                        policy.Failure.SystemError,
                        policy.Failure.Error,
                        &layout);
                }
                fallback = policy.UseFallbackIdentity;
                signing = policy.SigningConfigured;

                std::vector<std::wstring> arguments{
                    L"-c",
                    L"core.hooksPath=NUL",
                };
                if (fallback)
                {
                    arguments.insert(
                        arguments.end(),
                        {
                            L"-c",
                            L"user.name=Richer Task Runtime",
                            L"-c",
                            L"user.email=task-runtime@localhost",
                        });
                }
                arguments.insert(
                    arguments.end(),
                    {
                        L"commit",
                        L"--no-verify",
                        L"-m",
                        details::WidenGitSourceAscii(
                            details::GitWorktreeCheckpointSubject(run)),
                    });
                const auto committed = context.Run(std::move(arguments));
                if (!committed)
                {
                    auto result = _commandFailure(context, layout);
                    result.UsedFallbackIdentity = fallback;
                    result.SigningConfigured = signing;
                    return result;
                }
                if (committed->ExitCode != 0)
                {
                    auto result = _failure(
                        signing ? GitWorktreeCheckpointStatus::SigningFailure :
                                  GitWorktreeCheckpointStatus::GitFailure,
                        ERROR_INVALID_DATA,
                        signing ?
                            "Configured Git signing failed for the TaskRun checkpoint" :
                            "Git could not create the TaskRun checkpoint commit",
                        &layout);
                    result.UsedFallbackIdentity = fallback;
                    result.SigningConfigured = signing;
                    return result;
                }
                created = true;
                const auto head = context.Run({
                    L"rev-parse",
                    L"--verify",
                    L"HEAD",
                });
                const auto parsed = head ? details::ParseGitSourceLine(*head) :
                                           std::optional<std::string>{};
                if (!head || head->ExitCode != 0 || !parsed ||
                    !details::IsGitSourceObjectId(
                        *parsed,
                        journal.ObjectFormat) ||
                    *parsed == agentEndCommit)
                {
                    auto result = !head ?
                                      _commandFailure(context, layout) :
                                      _failure(
                                          GitWorktreeCheckpointStatus::VerificationFailed,
                                          ERROR_INVALID_DATA,
                                          "TaskRun checkpoint commit ID is invalid",
                                          &layout);
                    result.UsedFallbackIdentity = fallback;
                    result.SigningConfigured = signing;
                    return result;
                }
                checkpointCommit = *parsed;
            }

            std::string verificationError;
            if (!details::VerifyGitWorktreeCheckpointCommit(
                    context,
                    layout,
                    run,
                    agentEndCommit,
                    checkpointCommit,
                    journal.ObjectFormat,
                    verificationError))
            {
                auto result = !context.FailureError.empty() ?
                                  _commandFailure(context, layout) :
                                  _failure(
                                      GitWorktreeCheckpointStatus::VerificationFailed,
                                      ERROR_REVISION_MISMATCH,
                                      std::move(verificationError),
                                      &layout);
                result.CheckpointCommit = checkpointCommit;
                result.CommitCreated = created;
                result.UsedFallbackIdentity = fallback;
                result.SigningConfigured = signing;
                return result;
            }

            GitWorktreeCheckpointCommitResult result;
            result.Status = created ?
                                GitWorktreeCheckpointStatus::Succeeded :
                                GitWorktreeCheckpointStatus::AlreadyCommitted;
            result.CheckpointCommit = std::move(checkpointCommit);
            result.CommitCreated = created;
            result.UsedFallbackIdentity = fallback;
            result.SigningConfigured = signing;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitWorktreeCheckpointStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Task Worktree checkpoint commit exhausted local resources");
        }
    }
}
