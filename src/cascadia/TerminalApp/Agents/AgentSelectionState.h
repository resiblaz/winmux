// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "IAgentCatalogSource.h"

namespace winrt::TerminalApp::implementation::agents
{
    struct AgentSelectionState
    {
        std::vector<protocol::AgentProfileProjection> Profiles;
        std::optional<protocol::AgentProfileProjection> Selected;
        std::wstring Model;
        std::wstring Instructions;
        std::string PendingAgentId;
        std::string After;
        bool ExplicitSelection{};
        bool Ready{};
    };
}
