// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeRtaskCapabilityRegistry.h"

#include "ITaskStore.h"
#include "RtaskCommand.h"

#include <memory>
#include <mutex>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeRtaskCommandContext
    {
        RuntimeRtaskMilliseconds MonotonicMilliseconds{};
        int64_t UnixMilliseconds{};
        std::string EventId;
    };

    enum class RuntimeRtaskCommandStatus
    {
        Succeeded,
        Replayed,
        InvalidRequest,
        Unauthorized,
        NotFound,
        Conflict,
        StoreUnavailable,
        ResourceFailure,
        InvalidConfiguration,
    };

    struct RuntimeRtaskCommandResult
    {
        RuntimeRtaskCommandStatus Status{
            RuntimeRtaskCommandStatus::InvalidConfiguration
        };
        Microsoft::Terminal::TaskRuntimeProtocol::RtaskCommandResult Result;
        RuntimeRtaskCapabilityStatus CapabilityStatus{
            RuntimeRtaskCapabilityStatus::InvalidConfiguration
        };
        Microsoft::Terminal::TaskCore::TaskStoreCommandStatus StoreStatus{
            Microsoft::Terminal::TaskCore::TaskStoreCommandStatus::StorageError
        };
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeRtaskCommandStatus::Succeeded ||
                   Status == RuntimeRtaskCommandStatus::Replayed;
        }
    };

    class RuntimeRtaskCommandService final
    {
    public:
        RuntimeRtaskCommandService(
            std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
            std::shared_ptr<RuntimeRtaskCapabilityRegistry> capabilities) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeRtaskCapabilityResult IssueForActiveRun(
            std::string_view taskId,
            std::string_view runId,
            Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethodSet methods,
            uint32_t lifetimeMilliseconds,
            RuntimeRtaskMilliseconds now);
        [[nodiscard]] RuntimeRtaskCapabilityResult InvalidateRun(
            std::string_view runId) noexcept;
        [[nodiscard]] RuntimeRtaskCommandResult Execute(
            const Microsoft::Terminal::TaskRuntimeProtocol::RtaskCommandRequest& request,
            const RuntimeRtaskCommandContext& context);

    private:
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> _store;
        std::shared_ptr<RuntimeRtaskCapabilityRegistry> _capabilities;
        mutable std::mutex _mutex;
        bool _valid{};
    };
}
