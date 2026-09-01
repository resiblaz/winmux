// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeGitWorktreeCheckpointContext
    {
        Microsoft::Terminal::TaskCore::CommandId Command;
        Microsoft::Terminal::TaskCore::TaskEventId Event;
        Microsoft::Terminal::TaskCore::GitCheckpointId Checkpoint;
        int64_t UnixMilliseconds{};
        bool RetryFailed{};
    };

    enum class RuntimeGitWorktreeCheckpointStatus
    {
        Started,
        Advanced,
        Completed,
        OperationFailed,
        StoreFailure,
        InvalidRequest,
        NotFound,
        UnsupportedState,
        ResourceFailure,
    };

    struct RuntimeGitWorktreeCheckpointResult
    {
        RuntimeGitWorktreeCheckpointStatus Status{
            RuntimeGitWorktreeCheckpointStatus::InvalidRequest
        };
        Microsoft::Terminal::TaskCore::TaskGitCheckpointState CurrentState{
            Microsoft::Terminal::TaskCore::TaskGitCheckpointState::Pending
        };
        Microsoft::Terminal::TaskCore::TaskGitCheckpointState NextState{
            Microsoft::Terminal::TaskCore::TaskGitCheckpointState::Pending
        };
        Microsoft::Terminal::TaskCore::TaskStoreCommandStatus StoreStatus{
            Microsoft::Terminal::TaskCore::TaskStoreCommandStatus::StorageError
        };
        bool FailurePersisted{};
        std::string StartCommit;
        std::string EndCommit;
        std::string CheckpointCommit;
        std::string ReasonCode;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeGitWorktreeCheckpointStatus::Started ||
                   Status == RuntimeGitWorktreeCheckpointStatus::Advanced ||
                   Status == RuntimeGitWorktreeCheckpointStatus::Completed;
        }
    };

    class RuntimeGitWorktreeCheckpointService final
    {
    public:
        RuntimeGitWorktreeCheckpointService(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::filesystem::path gitExecutable,
            std::filesystem::path runtimeDataRoot) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeGitWorktreeCheckpointResult Start(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const Microsoft::Terminal::TaskCore::TaskRunId& run,
            const RuntimeGitWorktreeCheckpointContext& context,
            HANDLE cancellationEvent);
        [[nodiscard]] RuntimeGitWorktreeCheckpointResult Step(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const Microsoft::Terminal::TaskCore::TaskRunId& run,
            const RuntimeGitWorktreeCheckpointContext& context,
            HANDLE cancellationEvent);

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::filesystem::path _gitExecutable;
        std::filesystem::path _runtimeDataRoot;
        bool _valid{};
    };
}
