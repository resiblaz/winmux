// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlRouter.h"
#include "RuntimeFoundationRouter.h"
#include "RuntimeRtaskCommandService.h"
#include "RuntimeTaskProviderService.h"
#include "RuntimeTaskProjectionService.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    class RuntimeAgentCatalogService;
    class RuntimeProductRouter final : public IRuntimeControlRequestRouter
    {
    public:
        RuntimeProductRouter(
            std::shared_ptr<RuntimeFoundationRouter> foundation,
            std::shared_ptr<RuntimeRtaskCommandService> rtask,
            std::shared_ptr<RuntimeTaskProviderService> provider = nullptr,
            std::shared_ptr<RuntimeTaskProjectionService> tasks = nullptr,
            std::shared_ptr<RuntimeAgentCatalogService> agents = nullptr) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeControlRouteResult Route(
            const RuntimeControlClientContext& client,
            const Microsoft::Terminal::TaskRuntimeProtocol::ControlRequest& request,
            const RuntimeControlCancellationToken& cancellation) override;

        [[nodiscard]] std::shared_ptr<RuntimeRtaskCommandService> Rtask() const noexcept;
        [[nodiscard]] std::shared_ptr<RuntimeTaskProviderService> Provider() const noexcept;
        [[nodiscard]] std::shared_ptr<RuntimeTaskProjectionService> Tasks() const noexcept;

    private:
        std::shared_ptr<RuntimeFoundationRouter> _foundation;
        std::shared_ptr<RuntimeRtaskCommandService> _rtask;
        std::shared_ptr<RuntimeTaskProviderService> _provider;
        std::shared_ptr<RuntimeTaskProjectionService> _tasks;
        std::shared_ptr<RuntimeAgentCatalogService> _agents;
        bool _valid{};
    };
}
