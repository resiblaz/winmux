// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceItemViewModel.h"
#include "WorkspaceNumberInput.h"

namespace winrt::TerminalApp::implementation
{
    class WorkspaceSidebarController final
    {
    public:
        static constexpr size_t MaximumNavigationHistory{ 100 };

        void Stop() noexcept
        {
            ListeningPortsTimer.Stop();
            NumberInput.Reset();
            ListeningPortsRefreshInFlight = false;
            ListeningPortsRefreshPending = false;
            ++ListeningPortsGeneration;
            BackHistory.clear();
            ForwardHistory.clear();
            NavigationCurrent.reset();
            Reordering = false;
            ReorderFrom.reset();
            ReorderItem = nullptr;
        }

        bool Enabled{};
        WorkspaceNumberInput NumberInput;
        winrt::guid NumberInputWorkspaceId{};
        bool LayoutInitialized{};
        SafeDispatcherTimer ListeningPortsTimer;
        bool ListeningPortsRefreshInFlight{};
        bool ListeningPortsRefreshPending{};
        uint64_t ListeningPortsGeneration{};
        bool SyncingSelection{};
        std::vector<winrt::guid> BackHistory;
        std::vector<winrt::guid> ForwardHistory;
        std::optional<winrt::guid> NavigationCurrent;
        bool Reordering{};
        std::optional<uint32_t> ReorderFrom;
        TerminalApp::WorkspaceItemViewModel ReorderItem{ nullptr };
    };
}
