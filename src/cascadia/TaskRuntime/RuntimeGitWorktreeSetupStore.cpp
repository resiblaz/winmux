// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeSetupStore.h"

#include "RuntimeCrypto.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    Microsoft::Terminal::TaskCore::TaskStoreCommandResult
    AdvanceRuntimeGitWorktreeSetup(
        Microsoft::Terminal::TaskCore::ITaskStore& store,
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const RuntimeGitWorktreeSetupContext& context,
        const Microsoft::Terminal::TaskCore::TaskGitWorktreeState requestedState,
        std::string snapshotDigest,
        std::string baselineCommit,
        const bool hasUndeliveredWork,
        std::string preservedPath,
        std::string failureReasonCode)
    {
        using namespace Microsoft::Terminal::TaskCore;
        if (!aggregate.GitWorktree)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::NotFound;
            result.Error = "Runtime Git Worktree record is unavailable";
            return result;
        }
        const auto& current = *aggregate.GitWorktree;
        std::vector<uint8_t> digestInput;
        const auto append = [&](const std::string_view value) {
            digestInput.insert(digestInput.end(), value.begin(), value.end());
            digestInput.push_back(0);
        };
        append("runtime-git-worktree-setup-v1");
        append(current.Task.Value());
        append(std::to_string(current.Revision));
        append(std::to_string(static_cast<uint8_t>(requestedState)));
        append(snapshotDigest);
        append(baselineCommit);
        append(hasUndeliveredWork ? "1" : "0");
        append(preservedPath);
        append(failureReasonCode);
        const auto digest = RuntimeSha256Hex(digestInput);
        if (!digest)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::StorageError;
            result.Error = "Runtime Git Worktree command digest failed";
            return result;
        }

        AdvanceTaskGitWorktreeCommand command;
        command.Metadata.Command = context.Command;
        command.Metadata.Event = context.Event;
        command.Metadata.ExpectedTaskVersion = aggregate.Task.Version;
        command.Metadata.RequestDigest = *digest;
        command.Metadata.EventPayloadJson = "{}";
        command.Metadata.ReceiptResultJson = "{}";
        command.Metadata.TimestampUnixMilliseconds = (std::max)({
            context.UnixMilliseconds,
            aggregate.Task.UpdatedAtUnixMilliseconds,
            current.UpdatedAtUnixMilliseconds,
        });
        command.Task = current.Task;
        command.ExecutionContext = current.ExecutionContext;
        command.ExpectedWorktreeRevision = current.Revision;
        command.RequestedState = requestedState;
        command.SnapshotDigest = std::move(snapshotDigest);
        command.BaselineCommit = std::move(baselineCommit);
        command.HasUndeliveredWork = hasUndeliveredWork;
        command.PreservedPath = std::move(preservedPath);
        command.FailureReasonCode = std::move(failureReasonCode);
        return store.AdvanceTaskGitWorktree(command);
    }
}
