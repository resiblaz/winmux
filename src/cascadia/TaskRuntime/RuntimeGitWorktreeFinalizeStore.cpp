// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeFinalizeStore.h"

#include "RuntimeCrypto.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    Microsoft::Terminal::TaskCore::TaskStoreCommandResult
    AdvanceRuntimeGitWorktreeFinalization(
        Microsoft::Terminal::TaskCore::ITaskStore& store,
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const RuntimeGitWorktreeFinalizeContext& context,
        const Microsoft::Terminal::TaskCore::TaskGitWorktreeState requestedState,
        const bool hasUndeliveredWork,
        const bool cleanupEligible,
        std::string preservedPath,
        std::string failureReasonCode)
    {
        using namespace Microsoft::Terminal::TaskCore;
        if (!aggregate.GitWorktree)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::NotFound;
            result.Error = "Runtime Git finalization Worktree is unavailable";
            return result;
        }
        const auto& worktree = *aggregate.GitWorktree;
        std::vector<uint8_t> input;
        const auto append = [&](const std::string_view value) {
            input.insert(input.end(), value.begin(), value.end());
            input.push_back(0);
        };
        append("runtime-git-worktree-finalize-v1");
        append(worktree.Task.Value());
        append(std::to_string(worktree.Revision));
        append(std::to_string(static_cast<uint8_t>(requestedState)));
        append(hasUndeliveredWork ? "1" : "0");
        append(cleanupEligible ? "1" : "0");
        append(preservedPath);
        append(failureReasonCode);
        const auto digest = RuntimeSha256Hex(input);
        if (!digest)
        {
            TaskStoreCommandResult result;
            result.Status = TaskStoreCommandStatus::StorageError;
            result.Error = "Runtime Git finalization command digest failed";
            return result;
        }

        AdvanceTaskGitWorktreeFinalizationCommand command;
        command.Metadata.Command = context.Command;
        command.Metadata.Event = context.Event;
        command.Metadata.ExpectedTaskVersion = aggregate.Task.Version;
        command.Metadata.RequestDigest = *digest;
        command.Metadata.EventPayloadJson = "{}";
        command.Metadata.ReceiptResultJson = "{}";
        command.Metadata.TimestampUnixMilliseconds = (std::max)({
            context.UnixMilliseconds,
            aggregate.Task.UpdatedAtUnixMilliseconds,
            worktree.UpdatedAtUnixMilliseconds,
        });
        command.Task = worktree.Task;
        command.ExecutionContext = worktree.ExecutionContext;
        command.ExpectedWorktreeRevision = worktree.Revision;
        command.RequestedState = requestedState;
        command.HasUndeliveredWork = hasUndeliveredWork;
        command.CleanupEligible = cleanupEligible;
        command.PreservedPath = std::move(preservedPath);
        command.FailureReasonCode = std::move(failureReasonCode);
        for (size_t attempt = 0;; ++attempt)
        {
            const auto stored = store.AdvanceTaskGitWorktreeFinalization(command);
            if (stored.Status != TaskStoreCommandStatus::VersionConflict || attempt >= 3)
                return stored;
            const auto current = store.LoadTask(command.Task);
            if (!current || !current->GitWorktree ||
                current->GitWorktree->Revision != worktree.Revision ||
                current->GitWorktree->State != worktree.State ||
                current->GitWorktree->ExecutionContext != command.ExecutionContext)
                return stored;
            // Provider notifications can advance only the aggregate version
            // while Git is running. Retry publication against that version,
            // retaining the Worktree CAS and without repeating filesystem work.
            command.Metadata.ExpectedTaskVersion = current->Task.Version;
            command.Metadata.TimestampUnixMilliseconds = (std::max)(
                command.Metadata.TimestampUnixMilliseconds, current->Task.UpdatedAtUnixMilliseconds);
        }
    }
}
