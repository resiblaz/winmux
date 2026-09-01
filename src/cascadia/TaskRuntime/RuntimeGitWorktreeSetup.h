// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeGitWorktreeSetupContext
    {
        Microsoft::Terminal::TaskCore::CommandId Command;
        Microsoft::Terminal::TaskCore::TaskEventId Event;
        int64_t UnixMilliseconds{};
    };

    enum class RuntimeGitWorktreeSetupStatus
    {
        Advanced,
        Ready,
        OperationFailed,
        StoreFailure,
        InvalidRequest,
        NotFound,
        UnsupportedState,
        ResourceFailure,
    };

    struct RuntimeGitWorktreeSetupResult
    {
        RuntimeGitWorktreeSetupStatus Status{
            RuntimeGitWorktreeSetupStatus::InvalidRequest
        };
        Microsoft::Terminal::TaskCore::TaskGitWorktreeState CurrentState{
            Microsoft::Terminal::TaskCore::TaskGitWorktreeState::Planning
        };
        Microsoft::Terminal::TaskCore::TaskGitWorktreeState NextState{
            Microsoft::Terminal::TaskCore::TaskGitWorktreeState::Planning
        };
        Microsoft::Terminal::TaskCore::TaskStoreCommandStatus StoreStatus{
            Microsoft::Terminal::TaskCore::TaskStoreCommandStatus::StorageError
        };
        bool FailurePersisted{};
        std::string SnapshotDigest;
        std::string BaselineCommit;
        std::string FailureReasonCode;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeGitWorktreeSetupStatus::Advanced ||
                   Status == RuntimeGitWorktreeSetupStatus::Ready;
        }
    };

    class RuntimeGitWorktreeSetupService final
    {
    public:
        RuntimeGitWorktreeSetupService(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::filesystem::path gitExecutable,
            std::filesystem::path runtimeDataRoot) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeGitWorktreeSetupResult Step(
            const Microsoft::Terminal::TaskCore::TaskId& task,
            const RuntimeGitWorktreeSetupContext& context,
            HANDLE cancellationEvent);

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::filesystem::path _gitExecutable;
        std::filesystem::path _runtimeDataRoot;
        bool _valid{};
    };
}
