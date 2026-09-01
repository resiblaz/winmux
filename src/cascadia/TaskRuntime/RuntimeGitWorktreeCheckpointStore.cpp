// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeCheckpointStore.h"

#include "RuntimeCrypto.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskCore;

        std::optional<std::string> _digest(
            const std::string_view domain,
            const TaskId& task,
            const TaskRunId& run,
            const GitCheckpointId& checkpoint,
            const uint64_t revision,
            const TaskGitCheckpointState state,
            std::initializer_list<std::string_view> values)
        {
            std::vector<uint8_t> input;
            const auto append = [&](const std::string_view value) {
                input.insert(input.end(), value.begin(), value.end());
                input.push_back(0);
            };
            append(domain);
            append(task.Value());
            append(run.Value());
            append(checkpoint.Value());
            append(std::to_string(revision));
            append(std::to_string(static_cast<uint8_t>(state)));
            for (const auto value : values)
            {
                append(value);
            }
            return RuntimeSha256Hex(input);
        }

        TaskStoreCommandResult _failure(std::string error)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::StorageError;
            result.Error = std::move(error);
            return result;
        }
    }

    TaskStoreCommandResult BeginRuntimeGitWorktreeCheckpoint(
        ITaskStore& store,
        const TaskAggregateSnapshot& aggregate,
        const TaskRunRecord& run,
        const RuntimeGitWorktreeCheckpointContext& context,
        std::string startCommit,
        std::string startSummaryJson)
    {
        const auto digest = _digest(
            "runtime-git-checkpoint-start-v1",
            aggregate.Task.Id,
            run.Id,
            context.Checkpoint,
            1,
            TaskGitCheckpointState::Pending,
            { startCommit, startSummaryJson });
        if (!digest)
        {
            return _failure("Runtime Git checkpoint start digest failed");
        }

        BeginTaskRunGitCheckpointCommand command;
        command.Metadata.Command = context.Command;
        command.Metadata.Event = context.Event;
        command.Metadata.ExpectedTaskVersion = aggregate.Task.Version;
        command.Metadata.RequestDigest = *digest;
        command.Metadata.EventPayloadJson = "{}";
        command.Metadata.ReceiptResultJson = "{}";
        command.Metadata.TimestampUnixMilliseconds = (std::max)(
            context.UnixMilliseconds,
            aggregate.Task.UpdatedAtUnixMilliseconds);
        command.Checkpoint.Id = context.Checkpoint;
        command.Checkpoint.Task = aggregate.Task.Id;
        command.Checkpoint.Run = run.Id;
        command.Checkpoint.Revision = 1;
        command.Checkpoint.State = TaskGitCheckpointState::Pending;
        command.Checkpoint.StartCommit = std::move(startCommit);
        command.Checkpoint.ChangedPathsJson = "[]";
        command.Checkpoint.DiffSummaryJson = std::move(startSummaryJson);
        command.Checkpoint.CreatedAtUnixMilliseconds =
            command.Metadata.TimestampUnixMilliseconds;
        command.Checkpoint.UpdatedAtUnixMilliseconds =
            command.Metadata.TimestampUnixMilliseconds;
        return store.BeginTaskRunGitCheckpoint(command);
    }

    TaskStoreCommandResult AdvanceRuntimeGitWorktreeCheckpoint(
        ITaskStore& store,
        const TaskAggregateSnapshot& aggregate,
        const TaskRunGitCheckpointRecord& checkpoint,
        const RuntimeGitWorktreeCheckpointContext& context,
        const TaskGitCheckpointState requestedState,
        std::string endCommit,
        std::string checkpointCommit,
        const bool autoCheckpointed,
        std::string changedPathsJson,
        std::string diffSummaryJson,
        const bool hasUndeliveredWork,
        std::string preservedPath,
        std::string reasonCode)
    {
        if (!aggregate.GitWorktree)
        {
            return _failure("Runtime Git checkpoint Worktree is unavailable");
        }
        const auto digest = _digest(
            "runtime-git-checkpoint-advance-v1",
            checkpoint.Task,
            checkpoint.Run,
            checkpoint.Id,
            checkpoint.Revision,
            requestedState,
            {
                endCommit,
                checkpointCommit,
                autoCheckpointed ? "1" : "0",
                changedPathsJson,
                diffSummaryJson,
                hasUndeliveredWork ? "1" : "0",
                preservedPath,
                reasonCode,
            });
        if (!digest)
        {
            return _failure("Runtime Git checkpoint advance digest failed");
        }

        AdvanceTaskRunGitCheckpointCommand command;
        command.Metadata.Command = context.Command;
        command.Metadata.Event = context.Event;
        command.Metadata.ExpectedTaskVersion = aggregate.Task.Version;
        command.Metadata.RequestDigest = *digest;
        command.Metadata.EventPayloadJson = "{}";
        command.Metadata.ReceiptResultJson = "{}";
        command.Metadata.TimestampUnixMilliseconds = (std::max)({
            context.UnixMilliseconds,
            aggregate.Task.UpdatedAtUnixMilliseconds,
            aggregate.GitWorktree->UpdatedAtUnixMilliseconds,
            checkpoint.UpdatedAtUnixMilliseconds,
        });
        command.Task = checkpoint.Task;
        command.Run = checkpoint.Run;
        command.Checkpoint = checkpoint.Id;
        command.ExpectedCheckpointRevision = checkpoint.Revision;
        command.ExpectedWorktreeRevision = aggregate.GitWorktree->Revision;
        command.RequestedState = requestedState;
        command.EndCommit = std::move(endCommit);
        command.CheckpointCommit = std::move(checkpointCommit);
        command.AutoCheckpointed = autoCheckpointed;
        command.ChangedPathsJson = std::move(changedPathsJson);
        command.DiffSummaryJson = std::move(diffSummaryJson);
        command.HasUndeliveredWork = hasUndeliveredWork;
        command.PreservedPath = std::move(preservedPath);
        command.ReasonCode = std::move(reasonCode);
        return store.AdvanceTaskRunGitCheckpoint(command);
    }
}
