// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "IAgentCatalogSource.h"
#include "TaskRuntimeClient.h"

namespace winrt::TerminalApp::implementation::agents
{
    [[nodiscard]] std::shared_ptr<IAgentCatalogSource> MakeRuntimeAgentCatalogSource(
        std::shared_ptr<::Microsoft::Terminal::TaskRuntime::TaskRuntimeClient> client,
        Windows::UI::Core::CoreDispatcher dispatcher);
}
