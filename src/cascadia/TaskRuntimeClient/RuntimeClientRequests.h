// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientTypes.h"

#include <memory>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    // Pure caller-serialized request correlation. It performs no IO, waiting,
    // callback dispatch, retry creation, or logical-operation completion.
    class RuntimeClientRequestRegistry final
    {
    public:
        explicit RuntimeClientRequestRegistry(RuntimeClientRequestRegistryConfiguration configuration);
        ~RuntimeClientRequestRegistry();

        RuntimeClientRequestRegistry(const RuntimeClientRequestRegistry&) = delete;
        RuntimeClientRequestRegistry& operator=(const RuntimeClientRequestRegistry&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeClientRequestAddStatus Add(RuntimeClientRequestAttemptInfo attempt);
        [[nodiscard]] bool BeginWrite(std::string_view requestId, RuntimeClientEpoch epoch);
        [[nodiscard]] bool MarkWriteCompleted(std::string_view requestId, RuntimeClientEpoch epoch);
        [[nodiscard]] RuntimeClientCorrelatedResponse CompleteResponse(
            RuntimeClientEpoch receivedEpoch,
            RuntimeClientEpoch activeEpoch,
            std::string_view requestId);
        [[nodiscard]] std::vector<RuntimeClientRetiredRequest> RetireEpoch(RuntimeClientEpoch epoch);
        [[nodiscard]] std::optional<RuntimeClientRequestAttemptInfo> CancelQueued(
            RuntimeClientOperationId operationId);
        [[nodiscard]] std::vector<RuntimeClientRequestAttemptInfo> DrainQueuedForStop();
        void ClearRetiredEpochTombstones(RuntimeClientEpoch epoch) noexcept;

        [[nodiscard]] size_t ActiveCount() const noexcept;
        [[nodiscard]] size_t QueuedCount() const noexcept;
        [[nodiscard]] size_t TombstoneCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
