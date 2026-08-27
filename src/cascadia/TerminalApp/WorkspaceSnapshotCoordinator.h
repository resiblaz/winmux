// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceItemViewModel.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;

    // Owns the save/open/cache state machines for durable Workspace snapshots.
    // It is UI-thread affine and explicitly moves Git and repository work to
    // background continuations.
    class WorkspaceSnapshotCoordinator final
    {
    public:
        explicit WorkspaceSnapshotCoordinator(TerminalPage& page) noexcept;
        ~WorkspaceSnapshotCoordinator();

        WorkspaceSnapshotCoordinator(const WorkspaceSnapshotCoordinator&) = delete;
        WorkspaceSnapshotCoordinator& operator=(const WorkspaceSnapshotCoordinator&) = delete;

        void Stop() noexcept;
        void SaveRequested(const TerminalApp::WorkspaceItemViewModel& item);
        void Open(const winrt::guid& snapshotId, bool replaceCurrentWorkspace);
        void RefreshCache();
        void PopulateFlyout(bool refreshCache = true);
        void BindItem(const TerminalApp::WorkspaceItemViewModel& item,
                      const winrt::guid& snapshotId,
                      uint64_t revision);
        void ClearItemBinding(const TerminalApp::WorkspaceItemViewModel& item);

    private:
        safe_void_coroutine _SaveWorkspaceSnapshotAsync(TerminalApp::WorkspaceItemViewModel item,
                                                         std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                                         winrt::hstring workingDirectory,
                                                         winrt::hstring title,
                                                         uint64_t generation,
                                                         std::optional<winrt::guid> sourceSnapshotId,
                                                         uint64_t sourceRevision);
        void _CommitWorkspaceSnapshot(const TerminalApp::WorkspaceItemViewModel& item,
                                      std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                      const Microsoft::Terminal::Settings::Model::WorkspaceGitBinding& gitBinding,
                                      std::optional<winrt::guid> sourceSnapshotId,
                                      uint64_t sourceRevision);
        void _OpenWorkspaceSnapshot(const winrt::guid& snapshotId);
        void _ReplayWorkspaceSnapshot(const Microsoft::Terminal::Settings::Model::WorkspaceSnapshot& snapshot);
        safe_void_coroutine _ValidateAndOpenWorkspaceSnapshotAsync(winrt::guid snapshotId, uint64_t revision);
        void _FinishWorkspaceSnapshotReplacement(bool removePlaceholder);
        void _MarkWorkspaceSnapshotOpened(const winrt::guid& snapshotId, const winrt::guid& workspaceId);
        safe_void_coroutine _RefreshWorkspaceSnapshotCacheAsync();

        TerminalPage& _page;
        bool _openOperationInFlight{};
        TerminalApp::Tab _replacementTab{ nullptr };
        std::unordered_map<winrt::guid, uint64_t> _saveGenerations;
        std::vector<Microsoft::Terminal::Settings::Model::WorkspaceSnapshot> _menuCache;
        bool _cacheRefreshInFlight{};
        bool _cacheRefreshPending{};
        bool _stopped{};
    };
}
