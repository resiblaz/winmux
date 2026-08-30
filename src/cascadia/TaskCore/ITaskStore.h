// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreStatus.h"
#include "TaskStoreCommandTypes.h"
#include "TaskStoreResults.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskCore
{
    class ITaskStore
    {
    public:
        virtual ~ITaskStore() = default;

        [[nodiscard]] virtual bool IsReady() const noexcept = 0;
        [[nodiscard]] virtual TaskStoreOpenStatus OpenStatus() const noexcept = 0;
        [[nodiscard]] virtual int32_t SchemaVersion() const noexcept = 0;
        [[nodiscard]] virtual const std::string& LastError() const noexcept = 0;

        [[nodiscard]] virtual TaskStoreCommandResult CreateTask(const CreateTaskCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ChangeTaskStatus(const ChangeTaskStatusCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult CreateTaskRun(const CreateTaskRunCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult InitializeTaskGitWorktree(const InitializeTaskGitWorktreeCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ChangeTaskRunState(const ChangeTaskRunStateCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ApplyTaskAgentMutation(const ApplyTaskAgentMutationCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ChangeTaskTerminalState(const ChangeTaskTerminalStateCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult StartTaskIntent(const StartTaskIntentCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult AdvanceTaskIntent(const AdvanceTaskIntentCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ChangeTaskManualHold(const ChangeTaskManualHoldCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult DeleteTask(const DeleteTaskCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult EnqueueTaskMessage(const EnqueueTaskMessageCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ReviseTaskMessage(const ReviseTaskMessageCommand& command) = 0;
        [[nodiscard]] virtual TaskStoreCommandResult ClaimTaskMessagesAndCreateRun(const ClaimTaskMessagesAndCreateRunCommand& command) = 0;

        [[nodiscard]] virtual std::optional<TaskAggregateSnapshot> LoadTask(const TaskId& task) const = 0;
        [[nodiscard]] virtual std::vector<TaskRecord> ListTasks(const std::optional<WorkspaceId>& workspace) const = 0;
        [[nodiscard]] virtual std::vector<TaskEventRecord> ListEvents(const TaskId& task, uint64_t afterSequence, size_t limit) const = 0;
        [[nodiscard]] virtual std::optional<DeleteTombstoneRecord> LoadDeleteTombstone(const TaskId& task) const = 0;
        [[nodiscard]] virtual TaskRecoveryQueryResult LoadRecoverySnapshot(size_t limit) const = 0;
        [[nodiscard]] virtual TaskStoreRuntimeActivityQueryResult LoadRuntimeActivity() const = 0;
        [[nodiscard]] virtual TaskStoreMaintenanceResult PurgeExpiredTombstones(int64_t nowUnixMilliseconds, size_t limit) = 0;
    };
}
