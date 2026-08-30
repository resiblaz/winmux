// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRuntimeClientSubscriptionTypes.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime::details
{
    struct RuntimeClientSubscriptionStreamState;
    struct RuntimeClientSubscriptionStreamFactory;
}

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeClientSubscriptionStream final
    {
    public:
        ~RuntimeClientSubscriptionStream();

        RuntimeClientSubscriptionStream(const RuntimeClientSubscriptionStream&) = delete;
        RuntimeClientSubscriptionStream& operator=(const RuntimeClientSubscriptionStream&) = delete;

        [[nodiscard]] RuntimeClientSubscriptionPullResult Pull(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent = nullptr) noexcept;
        // Idempotent and non-blocking when the internal remove callback obeys
        // its contract. Returns true only for the first request.
        [[nodiscard]] bool RequestUnsubscribe() noexcept;

        [[nodiscard]] RuntimeClientLogicalSubscriptionId LogicalId() const noexcept;
        [[nodiscard]] RuntimeClientSubscriptionRevision Revision() const noexcept;

    private:
        explicit RuntimeClientSubscriptionStream(
            std::shared_ptr<details::RuntimeClientSubscriptionStreamState> state) noexcept;
        friend struct details::RuntimeClientSubscriptionStreamFactory;

        std::shared_ptr<details::RuntimeClientSubscriptionStreamState> _state;
    };
}
