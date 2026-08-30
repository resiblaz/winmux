// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlRouter.h"

#include <cstdint>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    // Routes the small control surface that is available before Task services
    // have been composed. This router intentionally does not expose Task,
    // subscription, terminal, or shutdown operations.
    class RuntimeFoundationRouter final : public IRuntimeControlRequestRouter
    {
    public:
        RuntimeFoundationRouter(
            std::string runtimeIdentity,
            std::string runtimeInstanceId,
            uint32_t processId);

        [[nodiscard]] bool Valid() const noexcept;

        [[nodiscard]] RuntimeControlRouteResult Route(
            const RuntimeControlClientContext& client,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) override;

    private:
        std::string _runtimeIdentity;
        std::string _runtimeInstanceId;
        uint32_t _processId{};
    };
}
