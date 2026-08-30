// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"

#include <filesystem>
#include <memory>

namespace Microsoft::Terminal::TaskCore
{
    class SqliteTaskStore final : public ITaskStore
    {
    public:
        explicit SqliteTaskStore(const std::filesystem::path& path);
        ~SqliteTaskStore() override;

        SqliteTaskStore(const SqliteTaskStore&) = delete;
        SqliteTaskStore& operator=(const SqliteTaskStore&) = delete;
        SqliteTaskStore(SqliteTaskStore&&) = delete;
        SqliteTaskStore& operator=(SqliteTaskStore&&) = delete;

        [[nodiscard]] bool IsReady() const noexcept override;
        [[nodiscard]] TaskStoreOpenStatus OpenStatus() const noexcept override;
        [[nodiscard]] int32_t SchemaVersion() const noexcept override;
        [[nodiscard]] const std::string& LastError() const noexcept override;

        [[nodiscard]] TaskStoreCommandResult CreateTask(const CreateTaskCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult ChangeTaskStatus(const ChangeTaskStatusCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult CreateTaskRun(const CreateTaskRunCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult ChangeTaskRunState(const ChangeTaskRunStateCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult ChangeTaskTerminalState(const ChangeTaskTerminalStateCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult StartTaskIntent(const StartTaskIntentCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult AdvanceTaskIntent(const AdvanceTaskIntentCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult ChangeTaskManualHold(const ChangeTaskManualHoldCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult DeleteTask(const DeleteTaskCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult EnqueueTaskMessage(const EnqueueTaskMessageCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult ReviseTaskMessage(const ReviseTaskMessageCommand& command) override;
        [[nodiscard]] TaskStoreCommandResult ClaimTaskMessagesAndCreateRun(const ClaimTaskMessagesAndCreateRunCommand& command) override;

        [[nodiscard]] std::optional<TaskAggregateSnapshot> LoadTask(const TaskId& task) const override;
        [[nodiscard]] std::vector<TaskRecord> ListTasks(const std::optional<WorkspaceId>& workspace) const override;
        [[nodiscard]] std::vector<TaskEventRecord> ListEvents(const TaskId& task, uint64_t afterSequence, size_t limit) const override;
        [[nodiscard]] std::optional<DeleteTombstoneRecord> LoadDeleteTombstone(const TaskId& task) const override;
        [[nodiscard]] TaskRecoveryQueryResult LoadRecoverySnapshot(size_t limit) const override;
        [[nodiscard]] TaskStoreMaintenanceResult PurgeExpiredTombstones(int64_t nowUnixMilliseconds, size_t limit) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
