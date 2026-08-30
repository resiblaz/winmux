// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlRouter.h"
#include "RuntimeFoundationRouter.h"
#include "RuntimeRtaskCommandService.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeProductRouter final : public IRuntimeControlRequestRouter
    {
    public:
        RuntimeProductRouter(
            std::shared_ptr<RuntimeFoundationRouter> foundation,
            std::shared_ptr<RuntimeRtaskCommandService> rtask) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeControlRouteResult Route(
            const RuntimeControlClientContext& client,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) override;

        [[nodiscard]] std::shared_ptr<RuntimeRtaskCommandService> Rtask() const noexcept;

    private:
        std::shared_ptr<RuntimeFoundationRouter> _foundation;
        std::shared_ptr<RuntimeRtaskCommandService> _rtask;
        bool _valid{};
    };
}
