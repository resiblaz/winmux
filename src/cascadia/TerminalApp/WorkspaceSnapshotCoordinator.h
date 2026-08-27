// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceItemViewModel.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;
    struct WorkspaceGitBinding;

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
        void ValidateRestoredItemBinding(const TerminalApp::WorkspaceItemViewModel& item,
                                         const winrt::guid& snapshotId,
                                         uint64_t revision);
        void ClearItemBinding(const TerminalApp::WorkspaceItemViewModel& item);

    private:
        struct SaveRequest;

        safe_void_coroutine _SaveWorkspaceSnapshotAsync(std::shared_ptr<SaveRequest> request);
        void _FinishWorkspaceSnapshotSave(const winrt::guid& workspaceId);
        void _OpenWorkspaceSnapshot(const winrt::guid& snapshotId);
        safe_void_coroutine _OpenWorkspaceSnapshotAsync(winrt::guid snapshotId);
        void _ReplayWorkspaceSnapshot(const Microsoft::Terminal::Settings::Model::WorkspaceSnapshot& snapshot);
        safe_void_coroutine _ValidateAndOpenWorkspaceSnapshotAsync(Microsoft::Terminal::Settings::Model::WorkspaceSnapshot snapshot);
        safe_void_coroutine _QueryAndConfirmWorkspaceSnapshotRebindAsync(winrt::guid snapshotId,
                                                                         uint64_t revision,
                                                                         WorkspaceGitBinding savedBinding,
                                                                         winrt::guid workspaceId,
                                                                         winrt::hstring workingDirectory);
        safe_void_coroutine _CommitWorkspaceSnapshotRebindAsync(winrt::guid snapshotId,
                                                                 uint64_t revision,
                                                                 WorkspaceGitBinding savedBinding,
                                                                 WorkspaceGitBinding reboundBinding,
                                                                 winrt::guid workspaceId,
                                                                 winrt::hstring workingDirectory);
        safe_void_coroutine _ValidateRestoredItemBindingAsync(winrt::weak_ref<TerminalApp::WorkspaceItemViewModel> item,
                                                               winrt::guid workspaceId,
                                                               winrt::guid snapshotId,
                                                               uint64_t revision);
        void _FinishWorkspaceSnapshotReplacement(bool removePlaceholder);
        safe_void_coroutine _MarkWorkspaceSnapshotOpenedAsync(winrt::guid snapshotId, winrt::guid workspaceId);
        safe_void_coroutine _RemoveWorkspaceSnapshotAsync(winrt::guid snapshotId);
        safe_void_coroutine _RefreshWorkspaceSnapshotCacheAsync();

        TerminalPage& _page;
        bool _openOperationInFlight{};
        TerminalApp::Tab _replacementTab{ nullptr };
        std::unordered_map<winrt::guid, uint64_t> _saveGenerations;
        std::unordered_set<winrt::guid> _saveOperationsInFlight;
        std::unordered_map<winrt::guid, TerminalApp::WorkspaceItemViewModel> _pendingSaveItems;
        std::vector<Microsoft::Terminal::Settings::Model::WorkspaceSnapshot> _menuCache;
        bool _cacheRefreshInFlight{};
        bool _cacheRefreshPending{};
        bool _stopped{};
    };
}
