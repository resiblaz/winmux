// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeFinalize.h"

#include "RuntimeGitWorktreeFinalizeStore.h"
#include "RuntimeGitWorktreeSetupEvidence.h"

#include "GitWorktreeFinalize.h"
#include "TaskGitWorktreeStatePolicy.h"
#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskCore;
        using namespace Microsoft::Terminal::TaskGit;

        RuntimeGitWorktreeFinalizeResult _result(
            const RuntimeGitWorktreeFinalizeStatus status,
            const TaskGitWorktreeState current,
            const TaskGitWorktreeState next,
            std::string error = {})
        {
            RuntimeGitWorktreeFinalizeResult result;
            result.Status = status;
            result.CurrentState = current;
            result.NextState = next;
            result.Error = std::move(error);
            return result;
        }

        RuntimeGitWorktreeFinalizeResult _stored(
            const TaskStoreCommandResult& stored,
            const TaskGitWorktreeState current,
            const TaskGitWorktreeState next)
        {
            auto result = _result(
                stored.Succeeded() ?
                    (next == TaskGitWorktreeState::Finalized ?
                         RuntimeGitWorktreeFinalizeStatus::Completed :
                         RuntimeGitWorktreeFinalizeStatus::Advanced) :
                    RuntimeGitWorktreeFinalizeStatus::StoreFailure,
                current,
                next,
                stored.Error);
            result.StoreStatus = stored.Status;
            return result;
        }

        std::string _reason(const GitWorktreeFinalizeStatus status)
        {
            switch (status)
            {
            case GitWorktreeFinalizeStatus::Cancelled:
                return "git-finalize-cancelled";
            case GitWorktreeFinalizeStatus::TimedOut:
                return "git-finalize-timeout";
            case GitWorktreeFinalizeStatus::IdentityFailure:
                return "git-finalize-identity-failed";
            case GitWorktreeFinalizeStatus::SigningFailure:
                return "git-finalize-signing-failed";
            case GitWorktreeFinalizeStatus::EvidenceMismatch:
                return "git-finalize-evidence-invalid";
            case GitWorktreeFinalizeStatus::TargetChanged:
                return "git-finalize-target-changed";
            case GitWorktreeFinalizeStatus::SidecarCleanupFailed:
                return "git-finalize-sidecar-cleanup-failed";
            case GitWorktreeFinalizeStatus::ResourceFailure:
                return "git-finalize-resource-failed";
            default:
                return "git-finalize-operation-failed";
            }
        }
    }

    RuntimeGitWorktreeFinalizeService::RuntimeGitWorktreeFinalizeService(
        std::shared_ptr<ITaskStore> store,
        std::filesystem::path gitExecutable,
        std::filesystem::path runtimeDataRoot) noexcept :
        _store{ std::move(store) },
        _gitExecutable{ std::move(gitExecutable) },
        _runtimeDataRoot{ std::move(runtimeDataRoot) }
    {
        _valid = _store && _store->IsReady() && !_gitExecutable.empty() &&
                 !_runtimeDataRoot.empty();
    }

    bool RuntimeGitWorktreeFinalizeService::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeGitWorktreeFinalizeResult RuntimeGitWorktreeFinalizeService::Step(
        const TaskId& task,
        const RuntimeGitWorktreeFinalizeContext& context,
        const HANDLE cancellationEvent)
    {
        try
        {
            if (!_valid || !IsCanonicalUuid(task.Value()) ||
                !IsCanonicalUuid(context.Command.Value()) ||
                !IsCanonicalUuid(context.Event.Value()) ||
                context.UnixMilliseconds < 0 || !cancellationEvent ||
                cancellationEvent == INVALID_HANDLE_VALUE)
            {
                return _result(
                    RuntimeGitWorktreeFinalizeStatus::InvalidRequest,
                    TaskGitWorktreeState::Ready,
                    TaskGitWorktreeState::Ready,
                    "Runtime Git finalization request is invalid");
            }
            const auto aggregate = _store->LoadTask(task);
            if (!aggregate || !aggregate->GitWorktree)
            {
                return _result(
                    RuntimeGitWorktreeFinalizeStatus::NotFound,
                    TaskGitWorktreeState::Ready,
                    TaskGitWorktreeState::Ready,
                    "Runtime Git finalization Task or Worktree was not found");
            }
            const auto& worktree = *aggregate->GitWorktree;
            const auto current = worktree.State;
            if (current == TaskGitWorktreeState::Finalized)
            {
                return _result(
                    RuntimeGitWorktreeFinalizeStatus::Completed,
                    current,
                    current);
            }
            if (current == TaskGitWorktreeState::Ready)
            {
                return _stored(
                    details::AdvanceRuntimeGitWorktreeFinalization(
                        *_store,
                        *aggregate,
                        context,
                        TaskGitWorktreeState::Finalizing,
                        worktree.HasUndeliveredWork,
                        false),
                    current,
                    TaskGitWorktreeState::Finalizing);
            }
            if (current == TaskGitWorktreeState::Preserved)
            {
                if (!context.RetryPreserved)
                {
                    return _result(
                        RuntimeGitWorktreeFinalizeStatus::UnsupportedState,
                        current,
                        current,
                        "Preserved Worktree finalization requires explicit retry");
                }
                return _stored(
                    details::AdvanceRuntimeGitWorktreeFinalization(
                        *_store,
                        *aggregate,
                        context,
                        TaskGitWorktreeState::Finalizing,
                        true,
                        false),
                    current,
                    TaskGitWorktreeState::Finalizing);
            }
            if (current != TaskGitWorktreeState::Finalizing)
            {
                return _result(
                    RuntimeGitWorktreeFinalizeStatus::UnsupportedState,
                    current,
                    current,
                    "Task Git Worktree is not in a finalization state");
            }

            const auto layout = details::PrepareRuntimeGitWorktreeLayoutEvidence(
                worktree,
                _runtimeDataRoot);
            if (!layout.Succeeded)
            {
                return _result(
                    RuntimeGitWorktreeFinalizeStatus::InvalidRequest,
                    current,
                    current,
                    layout.Error);
            }
            const auto evidence = details::LoadRuntimeGitWorktreeEvidence(
                worktree,
                *layout.Layout,
                *layout.ExpectedMarker);
            std::optional<GitWorktreeSourceJournal> journal;
            if (evidence.Succeeded)
            {
                journal = *evidence.Journal;
            }
            GitWorktreeFinalizer finalizer{ _gitExecutable };
            const auto finalized = finalizer.Finalize(
                *layout.Layout,
                *layout.ExpectedMarker,
                journal,
                *layout.SourcePath,
                worktree.HasUndeliveredWork,
                cancellationEvent);
            if (!finalized.Succeeded())
            {
                const auto reason = _reason(finalized.Status);
                if (finalized.Preserved)
                {
                    const auto stored =
                        details::AdvanceRuntimeGitWorktreeFinalization(
                            *_store,
                            *aggregate,
                            context,
                            TaskGitWorktreeState::Preserved,
                            true,
                            false,
                            worktree.WorktreePath,
                            reason);
                    auto result = _result(
                        stored.Succeeded() ?
                            RuntimeGitWorktreeFinalizeStatus::OperationFailed :
                            RuntimeGitWorktreeFinalizeStatus::StoreFailure,
                        current,
                        TaskGitWorktreeState::Preserved,
                        stored.Succeeded() ? finalized.Error : stored.Error);
                    result.StoreStatus = stored.Status;
                    result.FailurePersisted = stored.Succeeded();
                    result.PreservedPath = worktree.WorktreePath;
                    result.ReasonCode = reason;
                    return result;
                }
                auto result = _result(
                    RuntimeGitWorktreeFinalizeStatus::OperationFailed,
                    current,
                    current,
                    finalized.Error);
                result.ProducedWork = finalized.ProducedWork;
                result.BranchRetained = finalized.BranchRetained;
                result.BranchDeleted = finalized.BranchDeleted;
                result.WorktreeRemoved = finalized.WorktreeRemoved;
                result.FinalCommit = finalized.FinalCommit;
                result.PreservedPath = worktree.WorktreePath;
                result.ReasonCode = reason;
                return result;
            }

            const auto stored = details::AdvanceRuntimeGitWorktreeFinalization(
                *_store,
                *aggregate,
                context,
                TaskGitWorktreeState::Finalized,
                false,
                true);
            auto result = _stored(
                stored,
                current,
                TaskGitWorktreeState::Finalized);
            result.ProducedWork = finalized.ProducedWork;
            result.BranchRetained = finalized.BranchRetained;
            result.BranchDeleted = finalized.BranchDeleted;
            result.WorktreeRemoved = finalized.WorktreeRemoved;
            result.FinalCommit = finalized.FinalCommit;
            return result;
        }
        catch (...)
        {
            return _result(
                RuntimeGitWorktreeFinalizeStatus::ResourceFailure,
                TaskGitWorktreeState::Ready,
                TaskGitWorktreeState::Ready,
                "Runtime Git finalization exhausted local resources");
        }
    }
}
