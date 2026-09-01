// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeGitWorktreeFinalizeContext
    {
        Microsoft::Terminal::TaskCore::CommandId Command;
        Microsoft::Terminal::TaskCore::TaskEventId Event;
        int64_t UnixMilliseconds{};
        bool RetryPreserved{};
    };

    enum class RuntimeGitWorktreeFinalizeStatus
    {
        Advanced,
        Completed,
        OperationFailed,
        StoreFailure,
        InvalidRequest,
        NotFound,
        UnsupportedState,
        ResourceFailure,
    };

    struct RuntimeGitWorktreeFinalizeResult
    {
        RuntimeGitWorktreeFinalizeStatus Status{
            RuntimeGitWorktreeFinalizeStatus::InvalidRequest
        };
        Microsoft::Terminal::TaskCore::TaskGitWorktreeState CurrentState{
            Microsoft::Terminal::TaskCore::TaskGitWorktreeState::Ready
        };
        Microsoft::Terminal::TaskCore::TaskGitWorktreeState NextState{
            Microsoft::Terminal::TaskCore::TaskGitWorktreeState::Ready
        };
        Microsoft::Terminal::TaskCore::TaskStoreCommandStatus StoreStatus{
            Microsoft::Terminal::TaskCore::TaskStoreCommandStatus::StorageError
        };
        bool FailurePersisted{};
        bool ProducedWork{};
        bool BranchRetained{};
        bool BranchDeleted{};
        bool WorktreeRemoved{};
        std::string FinalCommit;
        std::string PreservedPath;
        std::string ReasonCode;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeGitWorktreeFinalizeStatus::Advanced ||
                   Status == RuntimeGitWorktreeFinalizeStatus::Completed;
        }
    };

    class RuntimeGitWorktreeFinalizeService final
    {
    public:
        RuntimeGitWorktreeFinalizeService(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::filesystem::path gitExecutable,
            std::filesystem::path runtimeDataRoot) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeGitWorktreeFinalizeResult Step(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const RuntimeGitWorktreeFinalizeContext& context,
            HANDLE cancellationEvent);

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::filesystem::path _gitExecutable;
        std::filesystem::path _runtimeDataRoot;
        bool _valid{};
    };
}
