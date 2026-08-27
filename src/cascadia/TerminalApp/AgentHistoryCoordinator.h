// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AgentHistoryInbox.h"
#include "AgentHistoryStore.h"
#include "ConclusionSidebar.h"

#include <unordered_set>

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;

    // Coordinates durable history ingestion, retention, provider discovery and
    // resume for one TerminalPage. UI state remains in ConclusionSidebar while
    // all operation lifetime state is owned here.
    class AgentHistoryCoordinator final
    {
    public:
        explicit AgentHistoryCoordinator(TerminalPage& page) noexcept;
        ~AgentHistoryCoordinator();

        AgentHistoryCoordinator(const AgentHistoryCoordinator&) = delete;
        AgentHistoryCoordinator& operator=(const AgentHistoryCoordinator&) = delete;

        void Attach(const TerminalApp::ConclusionSidebar& sidebar);
        void Stop() noexcept;
        void OnWorkspaceItemsChanged();
        void UpdateView();
        safe_void_coroutine ResumeSession(winrt::guid workspaceId,
                                          winrt::hstring sessionId,
                                          winrt::hstring workingDirectory);

    private:
        void _UpdateConclusionSidebar();
        void _ImportAgentHistoryInbox();
        safe_void_coroutine _ImportAgentHistoryInboxAsync(std::vector<AgentHistoryInboxWorkspace> workspaces);
        void _CleanupAgentHistoryIfDue();
        safe_void_coroutine _CleanupAgentHistoryAsync(AgentHistoryRetentionPolicy policy);
        void _SyncCodexHistoryRequested(const winrt::guid& workspaceId,
                                        const winrt::hstring& workspaceTitle,
                                        const winrt::hstring& cwd);
        safe_void_coroutine _SyncCodexHistoryAsync(winrt::guid workspaceId,
                                                    winrt::hstring workspaceTitle,
                                                    winrt::hstring cwd);
        void _LoadCodexHistoryRequested(const winrt::guid& workspaceId,
                                        const winrt::hstring& workspaceTitle,
                                        const winrt::hstring& cwd,
                                        const winrt::hstring& sessionId);
        safe_void_coroutine _LoadCodexHistoryAsync(winrt::guid workspaceId,
                                                    winrt::hstring workspaceTitle,
                                                    winrt::hstring cwd,
                                                    winrt::hstring sessionId);
        void _ResumeCodexHistoryRequested(const winrt::guid& workspaceId,
                                          const winrt::hstring& workspaceTitle,
                                          const winrt::hstring& cwd,
                                          const winrt::hstring& sessionId);
        safe_void_coroutine _ResumeCodexSession(winrt::guid workspaceId,
                                                 winrt::hstring sessionId,
                                                 winrt::hstring workingDirectory);
        bool _IsCodexHistoryWorkspaceActive(const winrt::guid& workspaceId) const;
        void _CompleteCodexHistoryOperation(const winrt::guid& workspaceId,
                                            CodexHistoryUiState state,
                                            bool refreshHistory,
                                            bool refreshSessions);

        TerminalPage& _page;
        TerminalApp::ConclusionSidebar _sidebar{ nullptr };
        SafeDispatcherTimer _cleanupTimer;
        bool _inboxImportInFlight{};
        bool _inboxImportPending{};
        bool _cleanupInFlight{};
        bool _providerOperationInFlight{};
        bool _stopped{ true };
        std::unordered_set<winrt::guid> _autoSyncAttempted;
    };
}
