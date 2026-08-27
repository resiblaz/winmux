// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceAgentResume.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    struct PendingWorkspaceContent
    {
        std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> Actions;
        uint32_t TabIndex{};
        winrt::guid WorkspaceId{};
        winrt::guid SourceSnapshotId{};
        uint64_t SourceSnapshotRevision{};
    };

    struct WorkspaceSessionRestoreState
    {
        std::vector<winrt::guid> StartupWorkspaceIds;
        std::vector<winrt::guid> StartupSurfaceIds;
        std::unordered_set<winrt::guid> PinnedWorkspaceIds;
        std::unordered_map<winrt::guid, winrt::guid> ActiveSurfaceIds;
        std::vector<Microsoft::Terminal::Settings::Model::AgentResumeInfo> AgentResumeEntries;
        uint32_t RestoreIndex{};
        uint32_t StateVersion{};
        bool Restoring{};
        std::unordered_map<winrt::guid, std::pair<winrt::guid, uint64_t>> SnapshotSources;
        bool SnapshotReplayInFlight{};
        bool PreparedEmptyWindow{};
        std::deque<PendingWorkspaceContent> PendingContent;
        bool DrainingPending{};

        void Reset() noexcept
        {
            StartupWorkspaceIds.clear();
            StartupSurfaceIds.clear();
            PinnedWorkspaceIds.clear();
            ActiveSurfaceIds.clear();
            AgentResumeEntries.clear();
            RestoreIndex = 0;
            StateVersion = 0;
            Restoring = false;
            SnapshotSources.clear();
            SnapshotReplayInFlight = false;
            PreparedEmptyWindow = false;
            PendingContent.clear();
            DrainingPending = false;
        }
    };
}
