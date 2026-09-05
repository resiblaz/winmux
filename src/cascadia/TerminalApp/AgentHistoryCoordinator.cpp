// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agent history lifecycle, persistence and provider coordination for one page.
//

#include "pch.h"
#include "AgentHistoryCoordinator.h"
#include "TerminalPage.h"
#include "WorkspaceFeatureSet.h"

#include "../../types/inc/utils.hpp"

#include "AgentHistoryInbox.h"
#include "AgentHistoryStore.h"
#include "CodexAppServerClient.h"
#include "Utils.h"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace std::chrono_literals;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr size_t MaximumHistoryIdentifierCharacters{ 128 };
        constexpr size_t MaximumHistoryWorkspaceTitleCharacters{ 512 };
        constexpr size_t MaximumHistorySourceKindCharacters{ 64 };
        constexpr size_t MaximumHistoryTitleCharacters{ 4096 };
        constexpr int64_t MaximumUnixSecondsForHistory{ 253402300799 };

        std::optional<int64_t> _codexSecondsToMilliseconds(const int64_t seconds) noexcept
        {
            if (seconds < 0 || seconds > MaximumUnixSecondsForHistory)
            {
                return std::nullopt;
            }
            return seconds * 1000;
        }

        winrt::hstring _sanitizeHistorySingleLine(const std::wstring_view value, const size_t maximumCharacters)
        {
            std::wstring result;
            result.reserve(std::min(value.size(), maximumCharacters));
            for (const auto character : value)
            {
                if (result.size() == maximumCharacters || character == L'\r' || character == L'\n')
                {
                    break;
                }
                if (character >= L' ' && character != 0x7f)
                {
                    result.push_back(character);
                }
                else if (!result.empty() && result.back() != L' ')
                {
                    result.push_back(L' ');
                }
            }

            while (!result.empty() && iswspace(result.back()))
            {
                result.pop_back();
            }
            const auto first = std::ranges::find_if_not(result, [](const auto character) { return iswspace(character); });
            if (first != result.begin())
            {
                result.erase(result.begin(), first);
            }
            return winrt::hstring{ result };
        }

        bool _sameHistoryCwd(const std::wstring_view lhs, const std::wstring_view rhs) noexcept
        {
            try
            {
                const auto normalize = [](const std::wstring_view value) {
                    std::wstring normalized{ value };
                    std::ranges::replace(normalized, L'/', L'\\');
                    while (normalized.size() > 3 && normalized.back() == L'\\')
                    {
                        normalized.pop_back();
                    }
                    return normalized;
                };
                const auto left = normalize(lhs);
                const auto right = normalize(rhs);
                if (left.empty() || right.empty() ||
                    left.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    right.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    return false;
                }
                return CompareStringOrdinal(left.data(),
                                            static_cast<int>(left.size()),
                                            right.data(),
                                            static_cast<int>(right.size()),
                                            TRUE) == CSTR_EQUAL;
            }
            catch (...)
            {
                return false;
            }
        }

        CodexHistoryUiState _codexUiStateForError(const CodexAppServerError& error) noexcept
        {
            switch (error.Kind)
            {
            case CodexAppServerErrorKind::CliUnavailable:
            case CodexAppServerErrorKind::LaunchFailed:
            case CodexAppServerErrorKind::TransportFailed:
            case CodexAppServerErrorKind::TransportClosed:
            case CodexAppServerErrorKind::Timeout:
            case CodexAppServerErrorKind::Cancelled:
            case CodexAppServerErrorKind::UnsupportedMethod:
                return CodexHistoryUiState::Unavailable;
            default:
                return CodexHistoryUiState::Error;
            }
        }

        std::optional<AgentHistorySessionRecord> _codexSessionRecord(const CodexAppServerThreadSummary& summary,
                                                                     const winrt::guid& workspaceId,
                                                                     const winrt::hstring& workspaceTitle,
                                                                     const winrt::hstring& expectedCwd,
                                                                     const int64_t seenAt)
        {
            const auto createdAt = _codexSecondsToMilliseconds(summary.CreatedAtUnixSeconds);
            const auto updatedAt = _codexSecondsToMilliseconds(summary.UpdatedAtUnixSeconds);
            if (workspaceId == winrt::guid{} ||
                summary.Id.empty() || summary.Id.size() > MaximumHistoryIdentifierCharacters ||
                !createdAt || !updatedAt || updatedAt.value() < createdAt.value() ||
                !_sameHistoryCwd(summary.Cwd, expectedCwd))
            {
                return std::nullopt;
            }

            AgentHistorySessionRecord record;
            record.WorkspaceId = workspaceId;
            record.WorkspaceTitle = _sanitizeHistorySingleLine(workspaceTitle, MaximumHistoryWorkspaceTitleCharacters);
            record.Provider = L"codex";
            record.SessionId = summary.Id;
            record.Cwd = expectedCwd;
            record.DisplayName = _sanitizeHistorySingleLine(summary.Name, MaximumHistoryWorkspaceTitleCharacters);
            record.Preview = summary.Preview;
            record.SourceKind = _sanitizeHistorySingleLine(summary.SourceKind, MaximumHistorySourceKindCharacters);
            if (record.SourceKind.empty())
            {
                record.SourceKind = L"unknown";
            }
            record.CreatedAt = createdAt.value();
            record.UpdatedAt = updatedAt.value();
            record.SeenAt = seenAt;
            return record;
        }

        bool _persistCodexTurn(AgentHistoryStore& store,
                               const CodexAppServerTurn& turn,
                               const winrt::guid& workspaceId,
                               const winrt::hstring& workspaceTitle,
                               const winrt::hstring& cwd,
                               const winrt::hstring& sessionId,
                               const int64_t receivedAt)
        {
            if (turn.TurnId.empty() || turn.TurnId.size() > MaximumHistoryIdentifierCharacters || turn.FinalMarkdown.empty())
            {
                return false;
            }

            AgentHistoryRecord record;
            record.WorkspaceId = workspaceId;
            record.WorkspaceTitle = _sanitizeHistorySingleLine(workspaceTitle, MaximumHistoryWorkspaceTitleCharacters);
            record.Provider = L"codex";
            record.SessionId = sessionId;
            record.TurnId = turn.TurnId;
            record.Cwd = cwd;
            record.Title = _sanitizeHistorySingleLine(turn.FinalMarkdown, MaximumHistoryTitleCharacters);
            if (record.Title.empty())
            {
                record.Title = L"Codex";
            }
            record.Prompt = turn.Prompt;
            record.FinalMarkdown = turn.FinalMarkdown;
            record.Model = turn.Model;
            record.EventAt = turn.EventAtUnixMilliseconds;
            record.ReceivedAt = receivedAt;
            return store.UpsertFinal(record);
        }
    }

    AgentHistoryCoordinator::AgentHistoryCoordinator(TerminalPage& page) noexcept :
        _page{ page }
    {
    }

    AgentHistoryCoordinator::~AgentHistoryCoordinator()
    {
        Stop();
    }

    void AgentHistoryCoordinator::SetTaskSessionRegistry(
        std::shared_ptr<tasks::TaskAgentSessionRegistry> registry) noexcept
    {
        _taskSessions = std::move(registry);
    }

    void AgentHistoryCoordinator::Attach(const TerminalApp::ConclusionSidebar& sidebar)
    {
        assert(_page.Dispatcher().HasThreadAccess());
        Stop();
        _sidebar = sidebar;
        _stopped = false;

        const auto weakPage = _page.get_weak();
        winrt::get_self<implementation::ConclusionSidebar>(_sidebar)->SetCodexHistoryHandlers(
            [weakPage](const auto& workspaceId, const auto& workspaceTitle, const auto& cwd) {
                if (const auto page = weakPage.get(); page)
                {
                    page->WorkspaceHistory()._SyncCodexHistoryRequested(workspaceId, workspaceTitle, cwd);
                }
            },
            [weakPage](const auto& workspaceId, const auto& workspaceTitle, const auto& cwd, const auto& sessionId) {
                if (const auto page = weakPage.get(); page)
                {
                    page->WorkspaceHistory()._LoadCodexHistoryRequested(workspaceId, workspaceTitle, cwd, sessionId);
                }
            },
            [weakPage](const auto& workspaceId, const auto&, const auto& cwd, const auto& sessionId) {
                if (const auto page = weakPage.get(); page)
                {
                    page->WorkspaceHistory()._ResumeCodexHistoryRequested(workspaceId, {}, cwd, sessionId);
                }
            });

        _cleanupTimer.Interval(1h);
        _cleanupTimer.Tick([weakPage](const auto&, const auto&) {
            if (const auto page = weakPage.get(); page)
            {
                page->WorkspaceHistory()._CleanupAgentHistoryIfDue();
            }
        });
        _cleanupTimer.Start();
        _CleanupAgentHistoryIfDue();
        _ImportAgentHistoryInbox();
        _UpdateConclusionSidebar();
    }

    void AgentHistoryCoordinator::Stop() noexcept
    {
        _stopped = true;
        _cleanupTimer.Stop();
        _sidebar = nullptr;
        _inboxImportPending = false;
        _autoSyncAttempted.clear();
    }

    void AgentHistoryCoordinator::OnWorkspaceItemsChanged()
    {
        if (!_stopped)
        {
            _ImportAgentHistoryInbox();
            _UpdateConclusionSidebar();
        }
    }

    void AgentHistoryCoordinator::UpdateView()
    {
        if (!_stopped)
        {
            _UpdateConclusionSidebar();
        }
    }

    safe_void_coroutine AgentHistoryCoordinator::ResumeSession(winrt::guid workspaceId,
                                                                 winrt::hstring sessionId,
                                                                 winrt::hstring workingDirectory)
    {
        return _ResumeCodexSession(workspaceId, std::move(sessionId), std::move(workingDirectory));
    }

    void AgentHistoryCoordinator::_UpdateConclusionSidebar()
    {
        if (_stopped || !_sidebar || !_page.WorkspaceUiReady())
        {
            return;
        }

        const auto sidebar = winrt::get_self<implementation::ConclusionSidebar>(_sidebar);
        const auto activeItem = _page.ActiveWorkspace();
        if (_page.WorkspaceSidebarEnabled() && activeItem)
        {
            const auto item = activeItem;
            const auto workspaceId = item.WorkspaceId();
            const auto workspaceTitle = item.Title();
            const auto cwd = item.WorkingDirectory();
            sidebar->SetActiveWorkspace(workspaceId, workspaceTitle, cwd);

            size_t matchingCwdCount{};
            if (!cwd.empty())
            {
                for (const auto& candidate : _page.WorkspaceItemsView())
                {
                    if (_sameHistoryCwd(candidate.WorkingDirectory(), cwd))
                    {
                        ++matchingCwdCount;
                    }
                }
            }

            // SQLite is rendered synchronously first. The first selection of a
            // workspace then performs a read-only App Server discovery in the
            // background; failures leave the local History projection intact.
            // Cwd is only an inference for old sessions, so duplicate-cwd tabs
            // require an explicit Sync click instead of an automatic binding.
            if (!_providerOperationInFlight &&
                workspaceId != winrt::guid{} &&
                !cwd.empty() &&
                matchingCwdCount == 1 &&
                !_autoSyncAttempted.contains(workspaceId))
            {
                _autoSyncAttempted.emplace(workspaceId);
                _SyncCodexHistoryRequested(workspaceId, workspaceTitle, cwd);
            }
        }
        else
        {
            sidebar->SetActiveWorkspace(winrt::guid{}, winrt::hstring{}, winrt::hstring{});
        }
    }

    void AgentHistoryCoordinator::_ImportAgentHistoryInbox()
    {
        if (_inboxImportInFlight)
        {
            _inboxImportPending = true;
            return;
        }
        if (_page.WorkspaceItemsView().Size() == 0)
        {
            return;
        }

        std::vector<AgentHistoryInboxWorkspace> workspaces;
        workspaces.reserve(_page.WorkspaceItemsView().Size());
        for (const auto& item : _page.WorkspaceItemsView())
        {
            if (item.WorkspaceId() != winrt::guid{})
            {
                workspaces.emplace_back(AgentHistoryInboxWorkspace{
                    .Id = item.WorkspaceId(),
                    .Title = item.Title(),
                    .Cwd = item.WorkingDirectory(),
                });
            }
        }
        if (workspaces.empty())
        {
            return;
        }

        _inboxImportInFlight = true;
        _ImportAgentHistoryInboxAsync(std::move(workspaces));
    }

    safe_void_coroutine AgentHistoryCoordinator::_ImportAgentHistoryInboxAsync(std::vector<AgentHistoryInboxWorkspace> workspaces)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        co_await winrt::resume_background();

        AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
        size_t committed{};
        if (store.IsAvailable())
        {
            AgentHistoryInbox inbox{ store, AgentHistoryInbox::DefaultPaths() };
            committed = inbox.Import(workspaces).Committed;
        }

        co_await wil::resume_foreground(dispatcher);
        if (const auto page = weakThis.get(); page)
        {
            auto& coordinator = page->WorkspaceHistory();
            if (coordinator._stopped)
            {
                co_return;
            }
            coordinator._inboxImportInFlight = false;
            if (committed > 0 && coordinator._sidebar)
            {
                const auto sidebar = winrt::get_self<implementation::ConclusionSidebar>(coordinator._sidebar);
                sidebar->RefreshHistory();
                sidebar->RefreshSessions();
            }
            if (std::exchange(coordinator._inboxImportPending, false))
            {
                coordinator._ImportAgentHistoryInbox();
            }
        }
    }

    void AgentHistoryCoordinator::_CleanupAgentHistoryIfDue()
    {
        if (_cleanupInFlight)
        {
            return;
        }

        constexpr int64_t cleanupIntervalMilliseconds{ 24ll * 60 * 60 * 1000 };

        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto state = ApplicationState::SharedInstance();
        const auto lastCleanup = state.AgentHistoryLastCleanupAt();
        if (lastCleanup >= 0 && lastCleanup <= now && now - lastCleanup < cleanupIntervalMilliseconds)
        {
            return;
        }

        // Persist the lease before starting background work. The app's
        // single-instance UI thread makes this atomic across its windows, and
        // a crash cannot create a cleanup
        // storm on the next launch.
        state.AgentHistoryLastCleanupAt(now);
        state.Flush();

        const auto policy = _page.WorkspaceHistoryRetention();

        _cleanupInFlight = true;
        _CleanupAgentHistoryAsync(policy);
    }

    safe_void_coroutine AgentHistoryCoordinator::_CleanupAgentHistoryAsync(AgentHistoryRetentionPolicy policy)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        co_await winrt::resume_background();

        AgentHistoryCleanupResult result;
        AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
        if (store.IsAvailable())
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            result = store.Cleanup(policy, now);
        }

        co_await wil::resume_foreground(dispatcher);
        if (const auto page = weakThis.get(); page)
        {
            auto& coordinator = page->WorkspaceHistory();
            if (coordinator._stopped)
            {
                co_return;
            }
            coordinator._cleanupInFlight = false;
            const auto retryNeeded = !result.Succeeded ||
                                     (result.SoftLimitUnsatisfied &&
                                      (result.TurnsDeleted != 0 || result.ReceiptsDeleted != 0));
            if (retryNeeded)
            {
                // The timer ticks hourly. Backdate the durable lease so an
                // intentionally bounded cleanup continues on the next tick,
                // while pinned-only soft-limit excess returns to the daily
                // cadence after one no-progress retry.
                constexpr int64_t cleanupIntervalMilliseconds{ 24ll * 60 * 60 * 1000 };
                constexpr int64_t retryIntervalMilliseconds{ 60ll * 60 * 1000 };
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                const auto retryLease = std::max<int64_t>(0, now - (cleanupIntervalMilliseconds - retryIntervalMilliseconds));
                const auto state = ApplicationState::SharedInstance();
                state.AgentHistoryLastCleanupAt(retryLease);
                state.Flush();
            }
            if (result.Succeeded && (result.TurnsDeleted != 0 || result.SessionsDeleted != 0) && coordinator._sidebar)
            {
                const auto sidebar = winrt::get_self<implementation::ConclusionSidebar>(coordinator._sidebar);
                sidebar->RefreshHistory();
                sidebar->RefreshSessions();
            }
        }
    }

    void AgentHistoryCoordinator::_SyncCodexHistoryRequested(const winrt::guid& workspaceId,
                                                   const winrt::hstring& workspaceTitle,
                                                   const winrt::hstring& cwd)
    {
        if (_providerOperationInFlight ||
            workspaceId == winrt::guid{} ||
            cwd.empty() || cwd.size() > CodexAppServerClient::MaximumCwdCharacters)
        {
            return;
        }

        _autoSyncAttempted.emplace(workspaceId);
        _providerOperationInFlight = true;
        if (_sidebar)
        {
            winrt::get_self<implementation::ConclusionSidebar>(_sidebar)->SetCodexOperationState(true, CodexHistoryUiState::Syncing);
        }
        _SyncCodexHistoryAsync(workspaceId, workspaceTitle, cwd);
    }

    safe_void_coroutine AgentHistoryCoordinator::_SyncCodexHistoryAsync(winrt::guid workspaceId,
                                                               winrt::hstring workspaceTitle,
                                                               winrt::hstring cwd)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        const auto taskSessions = _taskSessions;
        co_await winrt::resume_background();

        auto completionState = CodexHistoryUiState::Error;
        size_t saved{};
        try
        {
            CodexAppServerClient client;
            const auto result = client.ListThreads(cwd);
            if (!result)
            {
                completionState = _codexUiStateForError(result.Error);
            }
            else
            {
                AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
                if (store.IsAvailable())
                {
                    const auto seenAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
                    bool rejected{};
                    for (const auto& summary : result.Threads)
                    {
                        if (taskSessions && taskSessions->Contains(summary.Id))
                        {
                            continue;
                        }
                        if (const auto record = _codexSessionRecord(summary, workspaceId, workspaceTitle, cwd, seenAt))
                        {
                            if (store.UpsertSessionSummary(record.value()))
                            {
                                ++saved;
                            }
                            else
                            {
                                rejected = true;
                            }
                        }
                        else
                        {
                            rejected = true;
                        }
                    }
                    completionState = rejected ? CodexHistoryUiState::Error : CodexHistoryUiState::Ready;
                }
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto page = weakThis.get(); page)
        {
            page->WorkspaceHistory()._CompleteCodexHistoryOperation(workspaceId, completionState, false, saved > 0);
        }
    }

    void AgentHistoryCoordinator::_LoadCodexHistoryRequested(const winrt::guid& workspaceId,
                                                   const winrt::hstring& workspaceTitle,
                                                   const winrt::hstring& cwd,
                                                   const winrt::hstring& sessionId)
    {
        if (_taskSessions && _taskSessions->Contains(sessionId))
        {
            return;
        }
        if (_providerOperationInFlight ||
            workspaceId == winrt::guid{} ||
            cwd.empty() || cwd.size() > CodexAppServerClient::MaximumCwdCharacters ||
            sessionId.empty() || sessionId.size() > MaximumHistoryIdentifierCharacters)
        {
            return;
        }

        _providerOperationInFlight = true;
        if (_sidebar)
        {
            winrt::get_self<implementation::ConclusionSidebar>(_sidebar)->SetCodexOperationState(true, CodexHistoryUiState::Loading);
        }
        _LoadCodexHistoryAsync(workspaceId, workspaceTitle, cwd, sessionId);
    }

    safe_void_coroutine AgentHistoryCoordinator::_LoadCodexHistoryAsync(winrt::guid workspaceId,
                                                               winrt::hstring workspaceTitle,
                                                               winrt::hstring cwd,
                                                               winrt::hstring sessionId)
    {
        const auto weakThis = _page.get_weak();
        const auto dispatcher = _page.Dispatcher();
        co_await winrt::resume_background();

        auto completionState = CodexHistoryUiState::Error;
        size_t savedTurns{};
        try
        {
            CodexAppServerClient client;
            const auto result = client.ReadThread(sessionId, true);
            if (!result)
            {
                completionState = _codexUiStateForError(result.Error);
            }
            else if (result.Thread.Summary.Id != sessionId ||
                     !_sameHistoryCwd(result.Thread.Summary.Cwd, cwd))
            {
                completionState = CodexHistoryUiState::Error;
            }
            else
            {
                AgentHistoryStore store{ AgentHistoryStore::DefaultPath() };
                if (store.IsAvailable())
                {
                    const auto receivedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count();
                    const auto summary = _codexSessionRecord(result.Thread.Summary,
                                                             workspaceId,
                                                             workspaceTitle,
                                                             cwd,
                                                             receivedAt);
                    bool rejected = !summary || !store.UpsertSessionSummary(summary.value());
                    for (const auto& turn : result.Thread.Turns)
                    {
                        if (turn.FinalMarkdown.empty())
                        {
                            continue;
                        }
                        if (_persistCodexTurn(store,
                                              turn,
                                              workspaceId,
                                              workspaceTitle,
                                              cwd,
                                              sessionId,
                                              receivedAt))
                        {
                            ++savedTurns;
                        }
                        else
                        {
                            rejected = true;
                        }
                    }
                    completionState = rejected ? CodexHistoryUiState::Error : CodexHistoryUiState::Ready;
                }
            }
        }
        CATCH_LOG();

        co_await wil::resume_foreground(dispatcher);
        if (const auto page = weakThis.get(); page)
        {
            page->WorkspaceHistory()._CompleteCodexHistoryOperation(workspaceId, completionState, savedTurns > 0, true);
        }
    }

    bool AgentHistoryCoordinator::_IsCodexHistoryWorkspaceActive(const winrt::guid& workspaceId) const
    {
        if (!_page.WorkspaceSidebarEnabled() || !_page.WorkspaceUiReady() || workspaceId == winrt::guid{})
        {
            return false;
        }
        if (const auto item = _page.ActiveWorkspace())
        {
            return item.WorkspaceId() == workspaceId;
        }
        return false;
    }

    void AgentHistoryCoordinator::_CompleteCodexHistoryOperation(const winrt::guid& workspaceId,
                                                       const CodexHistoryUiState state,
                                                       const bool refreshHistory,
                                                       const bool refreshSessions)
    {
        _providerOperationInFlight = false;
        const auto stillActive = _IsCodexHistoryWorkspaceActive(workspaceId);
        if (_sidebar)
        {
            const auto sidebar = winrt::get_self<implementation::ConclusionSidebar>(_sidebar);
            if (stillActive)
            {
                if (refreshHistory)
                {
                    sidebar->RefreshHistory();
                }
                if (refreshSessions)
                {
                    sidebar->RefreshSessions();
                }
            }
            sidebar->SetCodexOperationState(false, stillActive ? state : CodexHistoryUiState::Unavailable);
        }

        // If the user switched from A to B while A was in flight, B was not
        // marked as attempted. Re-evaluating now starts B's first discovery and
        // prevents A's status from being shown against the wrong Workspace.
        _UpdateConclusionSidebar();
    }

    void AgentHistoryCoordinator::_ResumeCodexHistoryRequested(const winrt::guid& workspaceId,
                                                     const winrt::hstring&,
                                                     const winrt::hstring& cwd,
                                                     const winrt::hstring& sessionId)
    {
        _ResumeCodexSession(workspaceId, sessionId, cwd);
    }

    safe_void_coroutine AgentHistoryCoordinator::_ResumeCodexSession(winrt::guid workspaceId,
                                                           winrt::hstring sessionId,
                                                           winrt::hstring workingDirectory)
    {
        if (_providerOperationInFlight)
        {
            co_return;
        }

        sessionId = WorkspaceAgentResume::NormalizeCodexSessionId(sessionId);
        if (_taskSessions && _taskSessions->Contains(sessionId))
        {
            co_return;
        }
        AgentResumeInfo info{};
        info.Version = WorkspaceAgentResume::CurrentVersion;
        info.WorkspaceId = workspaceId;
        info.Provider = WorkspaceAgentResume::CodexProvider;
        info.SessionId = sessionId;
        info.WorkingDirectory = workingDirectory;
        info.RequiresConfirmation = true;
        if (!WorkspaceAgentResume::IsSupported(info))
        {
            co_return;
        }

        const auto weakThis = _page.get_weak();
        const auto presenter = _page.DialogPresenter();
        if (!presenter)
        {
            co_return;
        }

        ContentDialog dialog{};
        dialog.Title(box_value(RS_(L"WorkspaceSidebar_AgentResumeDialogTitle")));
        dialog.Content(box_value(winrt::hstring{ RS_fmt(L"WorkspaceSidebar_AgentResumeDialogBody",
                                                        info.SessionId,
                                                        workingDirectory.empty() ? RS_(L"WorkspaceSidebar_AgentDefaultDirectory") : workingDirectory) }));
        dialog.PrimaryButtonText(RS_(L"WorkspaceSidebar_AgentResumeDialogConfirm"));
        dialog.CloseButtonText(RS_(L"WorkspaceSidebar_AgentDialogCancel"));
        dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary)
        {
            co_return;
        }

        auto page = weakThis.get();
        if (!page)
        {
            co_return;
        }
        auto* coordinator = &page->WorkspaceHistory();
        if (coordinator->_stopped || coordinator->_providerOperationInFlight)
        {
            co_return;
        }

        const auto codexCliPath = WorkspaceAgentResume::ResolveCodexCliPath();
        const auto commandProcessorPath = WorkspaceAgentResume::ResolveSystemCommandProcessorPath();
        if (codexCliPath.empty() || commandProcessorPath.empty())
        {
            ContentDialog errorDialog{};
            errorDialog.Title(box_value(RS_(L"WorkspaceSidebar_AgentCliMissingDialogTitle")));
            errorDialog.Content(box_value(RS_(L"WorkspaceSidebar_AgentCliMissingDialogBody")));
            errorDialog.CloseButtonText(RS_(L"Ok"));
            co_await presenter.ShowDialog(errorDialog);
            co_return;
        }

        coordinator->_providerOperationInFlight = true;
        if (coordinator->_sidebar)
        {
            winrt::get_self<implementation::ConclusionSidebar>(coordinator->_sidebar)->SetCodexOperationState(true, CodexHistoryUiState::Resuming);
        }

        const auto dispatcher = page->Dispatcher();
        page = nullptr;
        coordinator = nullptr;
        co_await winrt::resume_background();

        CodexAppServerResumeResult preflight;
        preflight.Error.Kind = CodexAppServerErrorKind::InternalError;
        try
        {
            CodexAppServerClient client;
            preflight = client.ProbeResume(sessionId);
        }
        CATCH_LOG();

        const auto semanticError = preflight.Error.Kind == CodexAppServerErrorKind::RpcError ||
                                   preflight.Error.Kind == CodexAppServerErrorKind::UnexpectedResponse;
        const auto technicalFallback = !preflight && !semanticError;
        const auto semanticFailure = (!preflight && semanticError) ||
                                     (preflight && preflight.Thread.Id != sessionId);
        const auto completionState = preflight ? CodexHistoryUiState::Ready : _codexUiStateForError(preflight.Error);

        co_await wil::resume_foreground(dispatcher);
        page = weakThis.get();
        if (!page)
        {
            co_return;
        }
        coordinator = &page->WorkspaceHistory();
        if (coordinator->_stopped)
        {
            co_return;
        }
        coordinator->_CompleteCodexHistoryOperation(workspaceId, completionState, false, false);

        if (semanticFailure)
        {
            ContentDialog fallbackDialog{};
            fallbackDialog.Title(box_value(RS_(L"WorkspaceSidebar_AgentResumeDialogTitle")));
            fallbackDialog.Content(box_value(RS_(L"ConclusionSidebar_CodexError")));
            fallbackDialog.PrimaryButtonText(RS_(L"WorkspaceSidebar_AgentResumeDialogConfirm"));
            fallbackDialog.CloseButtonText(RS_(L"WorkspaceSidebar_AgentDialogCancel"));
            fallbackDialog.DefaultButton(ContentDialogButton::Close);
            if (co_await presenter.ShowDialog(fallbackDialog) != ContentDialogResult::Primary)
            {
                co_return;
            }
        }
        else if (!preflight && !technicalFallback)
        {
            co_return;
        }

        const auto currentCodexCliPath = WorkspaceAgentResume::ResolveCodexCliPath();
        const auto currentCommandProcessorPath = WorkspaceAgentResume::ResolveSystemCommandProcessorPath();
        if (currentCodexCliPath.empty() || currentCommandProcessorPath.empty() ||
            !_sameHistoryCwd(currentCodexCliPath, codexCliPath) ||
            !_sameHistoryCwd(currentCommandProcessorPath, commandProcessorPath))
        {
            ContentDialog errorDialog{};
            errorDialog.Title(box_value(RS_(L"WorkspaceSidebar_AgentCliMissingDialogTitle")));
            errorDialog.Content(box_value(RS_(L"WorkspaceSidebar_AgentCliMissingDialogBody")));
            errorDialog.CloseButtonText(RS_(L"Ok"));
            co_await presenter.ShowDialog(errorDialog);
            co_return;
        }

        // The App Server is only a capability/identity preflight. The actual
        // interactive resume remains the fixed, explicitly confirmed CLI path.
        NewTerminalArgs args{};
        args.Commandline(WorkspaceAgentResume::BuildCommandline(info, currentCommandProcessorPath, currentCodexCliPath));
        args.Elevate(false);
        if (Utils::IsValidDirectory(workingDirectory.c_str()))
        {
            args.StartingDirectory(workingDirectory);
        }

        page->OpenHistorySession(workspaceId, args);
    }
}
