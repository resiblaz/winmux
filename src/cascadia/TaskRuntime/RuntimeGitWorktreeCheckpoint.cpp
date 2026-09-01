// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeCheckpoint.h"

#include "RuntimeGitWorktreeCheckpointStore.h"
#include "RuntimeGitWorktreeSetupEvidence.h"

#include "GitWorktreeCheckpoint.h"
#include "TaskRecordValidation.h"
#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskCore;
        using namespace Microsoft::Terminal::TaskGit;

        RuntimeGitWorktreeCheckpointResult _result(
            const RuntimeGitWorktreeCheckpointStatus status,
            const TaskGitCheckpointState current,
            const TaskGitCheckpointState next,
            std::string error = {})
        {
            RuntimeGitWorktreeCheckpointResult result;
            result.Status = status;
            result.CurrentState = current;
            result.NextState = next;
            result.Error = std::move(error);
            return result;
        }

        RuntimeGitWorktreeCheckpointResult _stored(
            const TaskStoreCommandResult& stored,
            const TaskGitCheckpointState current,
            const TaskGitCheckpointState next)
        {
            auto result = _result(
                stored.Succeeded() ?
                    (next == TaskGitCheckpointState::Succeeded ||
                             next == TaskGitCheckpointState::NotNeeded ?
                         RuntimeGitWorktreeCheckpointStatus::Completed :
                         RuntimeGitWorktreeCheckpointStatus::Advanced) :
                    RuntimeGitWorktreeCheckpointStatus::StoreFailure,
                current,
                next,
                stored.Error);
            result.StoreStatus = stored.Status;
            return result;
        }

        const TaskRunRecord* _findRun(
            const TaskAggregateSnapshot& aggregate,
            const TaskRunId& run)
        {
            const auto found = std::ranges::find_if(
                aggregate.Runs,
                [&](const auto& value) { return value.Id == run; });
            return found == aggregate.Runs.end() ? nullptr : &*found;
        }

        const TaskRunGitCheckpointRecord* _findCheckpoint(
            const TaskAggregateSnapshot& aggregate,
            const TaskRunId& run)
        {
            const auto found = std::ranges::find_if(
                aggregate.GitCheckpoints,
                [&](const auto& value) { return value.Run == run; });
            if (found == aggregate.GitCheckpoints.end())
            {
                return nullptr;
            }
            const auto duplicates = std::ranges::count_if(
                aggregate.GitCheckpoints,
                [&](const auto& value) { return value.Run == run; });
            return duplicates == 1 ? &*found : nullptr;
        }

        std::string _reason(const GitWorktreeCheckpointStatus status)
        {
            switch (status)
            {
            case GitWorktreeCheckpointStatus::Cancelled:
                return "git-checkpoint-cancelled";
            case GitWorktreeCheckpointStatus::TimedOut:
                return "git-checkpoint-timeout";
            case GitWorktreeCheckpointStatus::IdentityFailure:
                return "git-checkpoint-identity-failed";
            case GitWorktreeCheckpointStatus::SigningFailure:
                return "git-checkpoint-signing-failed";
            case GitWorktreeCheckpointStatus::OutputLimitExceeded:
                return "git-checkpoint-output-limit";
            case GitWorktreeCheckpointStatus::EvidenceMismatch:
                return "git-checkpoint-evidence-invalid";
            case GitWorktreeCheckpointStatus::TargetChanged:
                return "git-checkpoint-target-changed";
            case GitWorktreeCheckpointStatus::ResourceFailure:
                return "git-checkpoint-resource-failed";
            default:
                return "git-checkpoint-operation-failed";
            }
        }

        bool _validContext(
            const RuntimeGitWorktreeCheckpointContext& context,
            const bool requireCheckpoint) noexcept
        {
            return IsCanonicalUuid(context.Command.Value()) &&
                   IsCanonicalUuid(context.Event.Value()) &&
                   (!requireCheckpoint ||
                    IsCanonicalUuid(context.Checkpoint.Value())) &&
                   context.UnixMilliseconds >= 0;
        }
    }

    RuntimeGitWorktreeCheckpointService::RuntimeGitWorktreeCheckpointService(
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

    bool RuntimeGitWorktreeCheckpointService::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeGitWorktreeCheckpointResult
    RuntimeGitWorktreeCheckpointService::Start(
        const TaskId& task,
        const TaskRunId& run,
        const RuntimeGitWorktreeCheckpointContext& context,
        const HANDLE cancellationEvent)
    {
        try
        {
            if (!_valid || !IsCanonicalUuid(task.Value()) ||
                !IsCanonicalUuid(run.Value()) || !_validContext(context, true) ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::InvalidRequest,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "Runtime Git checkpoint start request is invalid");
            }
            const auto aggregate = _store->LoadTask(task);
            if (!aggregate || !aggregate->GitWorktree)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::NotFound,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "Runtime Git checkpoint Task or Worktree was not found");
            }
            const auto taskRun = _findRun(*aggregate, run);
            if (!taskRun)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::NotFound,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "Runtime Git checkpoint TaskRun was not found");
            }
            if (const auto existing = _findCheckpoint(*aggregate, run))
            {
                if (existing->Id != context.Checkpoint)
                {
                    return _result(
                        RuntimeGitWorktreeCheckpointStatus::InvalidRequest,
                        existing->State,
                        existing->State,
                        "Runtime Git checkpoint identity differs from the existing record");
                }
                auto result = _result(
                    RuntimeGitWorktreeCheckpointStatus::Started,
                    existing->State,
                    existing->State);
                result.StartCommit = existing->StartCommit;
                return result;
            }
            if (IsTaskRunTerminal(taskRun->State) ||
                aggregate->GitWorktree->State != TaskGitWorktreeState::Ready ||
                taskRun->ExecutionContext !=
                    aggregate->GitWorktree->ExecutionContext)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::UnsupportedState,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "TaskRun or Worktree is not ready for checkpoint start capture");
            }

            const auto layout = details::PrepareRuntimeGitWorktreeLayoutEvidence(
                *aggregate->GitWorktree,
                _runtimeDataRoot);
            if (!layout.Succeeded)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::InvalidRequest,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    layout.Error);
            }
            const auto evidence = details::LoadRuntimeGitWorktreeEvidence(
                *aggregate->GitWorktree,
                *layout.Layout,
                *layout.ExpectedMarker);
            if (!evidence.Succeeded)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::OperationFailed,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    evidence.Error);
            }
            GitWorktreeCheckpointManager manager{ _gitExecutable };
            const auto captured = manager.CaptureStart(
                *layout.Layout,
                *evidence.Marker,
                *evidence.Journal,
                cancellationEvent);
            if (!captured.Succeeded())
            {
                auto result = _result(
                    RuntimeGitWorktreeCheckpointStatus::OperationFailed,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    captured.Error);
                result.ReasonCode = _reason(captured.Status);
                return result;
            }
            const auto stored = details::BeginRuntimeGitWorktreeCheckpoint(
                *_store,
                *aggregate,
                *taskRun,
                context,
                captured.StartCommit,
                captured.DiffSummaryJson);
            auto result = _result(
                stored.Succeeded() ?
                    RuntimeGitWorktreeCheckpointStatus::Started :
                    RuntimeGitWorktreeCheckpointStatus::StoreFailure,
                TaskGitCheckpointState::Pending,
                TaskGitCheckpointState::Pending,
                stored.Error);
            result.StoreStatus = stored.Status;
            result.StartCommit = captured.StartCommit;
            return result;
        }
        catch (...)
        {
            return _result(
                RuntimeGitWorktreeCheckpointStatus::ResourceFailure,
                TaskGitCheckpointState::Pending,
                TaskGitCheckpointState::Pending,
                "Runtime Git checkpoint start exhausted local resources");
        }
    }

    RuntimeGitWorktreeCheckpointResult
    RuntimeGitWorktreeCheckpointService::Step(
        const TaskId& task,
        const TaskRunId& run,
        const RuntimeGitWorktreeCheckpointContext& context,
        const HANDLE cancellationEvent)
    {
        try
        {
            if (!_valid || !IsCanonicalUuid(task.Value()) ||
                !IsCanonicalUuid(run.Value()) || !_validContext(context, false) ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::InvalidRequest,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "Runtime Git checkpoint step request is invalid");
            }
            const auto aggregate = _store->LoadTask(task);
            if (!aggregate || !aggregate->GitWorktree)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::NotFound,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "Runtime Git checkpoint Task or Worktree was not found");
            }
            const auto taskRun = _findRun(*aggregate, run);
            const auto checkpoint = _findCheckpoint(*aggregate, run);
            if (!taskRun || !checkpoint)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::NotFound,
                    TaskGitCheckpointState::Pending,
                    TaskGitCheckpointState::Pending,
                    "Runtime Git checkpoint Run record was not found or is ambiguous");
            }
            const auto current = checkpoint->State;
            if (current == TaskGitCheckpointState::Succeeded ||
                current == TaskGitCheckpointState::NotNeeded)
            {
                auto result = _result(
                    RuntimeGitWorktreeCheckpointStatus::Completed,
                    current,
                    current);
                result.StartCommit = checkpoint->StartCommit;
                result.EndCommit = checkpoint->EndCommit;
                result.CheckpointCommit = checkpoint->CheckpointCommit;
                return result;
            }
            if (!IsTaskRunTerminal(taskRun->State) ||
                aggregate->GitWorktree->State != TaskGitWorktreeState::Ready)
            {
                return _result(
                    RuntimeGitWorktreeCheckpointStatus::UnsupportedState,
                    current,
                    current,
                    "Runtime Git checkpoint requires a terminal TaskRun and ready Worktree");
            }

            const auto advance = [&](const TaskGitCheckpointState next,
                                     std::string endCommit,
                                     std::string checkpointCommit,
                                     const bool autoCheckpointed,
                                     std::string changedPaths,
                                     std::string diffSummary,
                                     const bool hasUndelivered,
                                     std::string preserved = {},
                                     std::string reason = {}) {
                return details::AdvanceRuntimeGitWorktreeCheckpoint(
                    *_store,
                    *aggregate,
                    *checkpoint,
                    context,
                    next,
                    std::move(endCommit),
                    std::move(checkpointCommit),
                    autoCheckpointed,
                    std::move(changedPaths),
                    std::move(diffSummary),
                    hasUndelivered,
                    std::move(preserved),
                    std::move(reason));
            };

            if (current == TaskGitCheckpointState::Pending)
            {
                return _stored(
                    advance(
                        TaskGitCheckpointState::Inspecting,
                        {},
                        {},
                        false,
                        "[]",
                        checkpoint->DiffSummaryJson,
                        aggregate->GitWorktree->HasUndeliveredWork),
                    current,
                    TaskGitCheckpointState::Inspecting);
            }
            if (current == TaskGitCheckpointState::Failed)
            {
                if (!context.RetryFailed)
                {
                    return _result(
                        RuntimeGitWorktreeCheckpointStatus::UnsupportedState,
                        current,
                        current,
                        "Failed Git checkpoint requires an explicit retry");
                }
                const auto next = checkpoint->EndCommit.empty() ?
                                      TaskGitCheckpointState::Inspecting :
                                      TaskGitCheckpointState::Committing;
                return _stored(
                    advance(
                        next,
                        next == TaskGitCheckpointState::Committing ?
                            checkpoint->EndCommit : std::string{},
                        {},
                        false,
                        next == TaskGitCheckpointState::Committing ?
                            checkpoint->ChangedPathsJson : std::string{ "[]" },
                        checkpoint->DiffSummaryJson,
                        true),
                    current,
                    next);
            }

            const auto layout = details::PrepareRuntimeGitWorktreeLayoutEvidence(
                *aggregate->GitWorktree,
                _runtimeDataRoot);
            const auto evidence = layout.Succeeded ?
                                      details::LoadRuntimeGitWorktreeEvidence(
                                          *aggregate->GitWorktree,
                                          *layout.Layout,
                                          *layout.ExpectedMarker) :
                                      details::RuntimeGitWorktreeEvidenceResult{};
            const auto persistFailure = [&](const GitWorktreeCheckpointStatus status,
                                            std::string operationError) {
                const auto reason = _reason(status);
                const auto stored = advance(
                    TaskGitCheckpointState::Failed,
                    checkpoint->EndCommit,
                    {},
                    false,
                    checkpoint->ChangedPathsJson,
                    checkpoint->DiffSummaryJson,
                    true,
                    aggregate->GitWorktree->WorktreePath,
                    reason);
                auto result = _result(
                    stored.Succeeded() ?
                        RuntimeGitWorktreeCheckpointStatus::OperationFailed :
                        RuntimeGitWorktreeCheckpointStatus::StoreFailure,
                    current,
                    TaskGitCheckpointState::Failed,
                    stored.Succeeded() ?
                        std::move(operationError) : stored.Error);
                result.StoreStatus = stored.Status;
                result.FailurePersisted = stored.Succeeded();
                result.ReasonCode = reason;
                return result;
            };
            if (!layout.Succeeded || !evidence.Succeeded)
            {
                return persistFailure(
                    GitWorktreeCheckpointStatus::EvidenceMismatch,
                    layout.Succeeded ? evidence.Error : layout.Error);
            }

            GitWorktreeCheckpointManager manager{ _gitExecutable };
            if (current == TaskGitCheckpointState::Inspecting)
            {
                const auto startDigest = ParseGitWorktreeCheckpointStartDigest(
                    checkpoint->DiffSummaryJson);
                if (!startDigest)
                {
                    return persistFailure(
                        GitWorktreeCheckpointStatus::EvidenceMismatch,
                        "Runtime Git checkpoint start digest is invalid");
                }
                const auto inspected = manager.InspectAndStage(
                    *layout.Layout,
                    *evidence.Marker,
                    *evidence.Journal,
                    checkpoint->StartCommit,
                    *startDigest,
                    cancellationEvent);
                if (!inspected.Succeeded())
                {
                    return persistFailure(inspected.Status, inspected.Error);
                }
                const auto next = inspected.NeedsCommit ?
                                      TaskGitCheckpointState::Committing :
                                      TaskGitCheckpointState::NotNeeded;
                const auto hasUndelivered =
                    aggregate->GitWorktree->HasUndeliveredWork ||
                    inspected.AgentEndCommit !=
                        aggregate->GitWorktree->BaselineCommit;
                auto result = _stored(
                    advance(
                        next,
                        inspected.AgentEndCommit,
                        {},
                        false,
                        inspected.ChangedPathsJson,
                        inspected.DiffSummaryJson,
                        inspected.NeedsCommit || hasUndelivered),
                    current,
                    next);
                result.EndCommit = inspected.AgentEndCommit;
                return result;
            }
            if (current == TaskGitCheckpointState::Committing)
            {
                const auto committed = manager.CommitOrRecover(
                    *layout.Layout,
                    *evidence.Marker,
                    *evidence.Journal,
                    run,
                    checkpoint->EndCommit,
                    cancellationEvent);
                if (!committed.Succeeded())
                {
                    return persistFailure(committed.Status, committed.Error);
                }
                auto result = _stored(
                    advance(
                        TaskGitCheckpointState::Succeeded,
                        committed.CheckpointCommit,
                        committed.CheckpointCommit,
                        true,
                        checkpoint->ChangedPathsJson,
                        checkpoint->DiffSummaryJson,
                        true),
                    current,
                    TaskGitCheckpointState::Succeeded);
                result.EndCommit = committed.CheckpointCommit;
                result.CheckpointCommit = committed.CheckpointCommit;
                return result;
            }
            return _result(
                RuntimeGitWorktreeCheckpointStatus::UnsupportedState,
                current,
                current,
                "Runtime Git checkpoint phase is unsupported");
        }
        catch (...)
        {
            return _result(
                RuntimeGitWorktreeCheckpointStatus::ResourceFailure,
                TaskGitCheckpointState::Pending,
                TaskGitCheckpointState::Pending,
                "Runtime Git checkpoint step exhausted local resources");
        }
    }
}
