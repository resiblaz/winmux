// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"
#include "RuntimeTaskTerminalBackend.h"
#include "RuntimeTerminalEndpoint.h"

#include "TaskProviderTypes.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTaskTerminalOpenRequest
    {
        std::string TaskId;
        std::string TerminalSessionId;
        uint64_t Generation{};
        std::string OwnerClientInstanceId;
        std::string Title;
    };

    enum class RuntimeTaskTerminalServiceStatus
    {
        Succeeded,
        AlreadyOpen,
        OwnedElsewhere,
        NotFound,
        StaleGeneration,
        InvalidRequest,
        ResourceFailure,
    };

    struct RuntimeTaskTerminalServiceResult
    {
        RuntimeTaskTerminalServiceStatus Status{ RuntimeTaskTerminalServiceStatus::ResourceFailure };
        RuntimeTerminalEndpoint Endpoint;
        size_t ClientCount{};
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeTaskTerminalServiceStatus::Succeeded ||
                   Status == RuntimeTaskTerminalServiceStatus::AlreadyOpen;
        }
    };

    class RuntimeTaskTerminalService final
    {
    public:
        RuntimeTaskTerminalService(
            RuntimeIdentity identity,
            std::string runtimeInstanceId,
            RuntimeTaskTerminalInputHandler input,
            RuntimeTaskTerminalInterruptHandler interrupt) noexcept;
        ~RuntimeTaskTerminalService();

        RuntimeTaskTerminalService(const RuntimeTaskTerminalService&) = delete;
        RuntimeTaskTerminalService& operator=(const RuntimeTaskTerminalService&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTerminalEndpointResult Endpoint(
            std::string_view terminalSessionId,
            uint64_t generation) const;
        [[nodiscard]] RuntimeTaskTerminalServiceResult Open(const RuntimeTaskTerminalOpenRequest& request);
        [[nodiscard]] RuntimeTaskTerminalServiceResult PublishEvent(
            std::string_view taskId,
            const Microsoft::Terminal::TaskProvider::TaskProviderEvent& event);
        [[nodiscard]] RuntimeTaskTerminalServiceResult PublishText(
            std::string_view taskId,
            std::span<const uint8_t> output,
            uint32_t flags = 0);
        [[nodiscard]] RuntimeTaskTerminalServiceResult Close(
            std::string_view taskId,
            uint64_t generation,
            bool graceful = true);
        [[nodiscard]] size_t ClientCount(std::string_view taskId) const noexcept;
        [[nodiscard]] size_t SessionCount() const noexcept;
        void Stop() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
